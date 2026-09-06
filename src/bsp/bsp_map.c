#include "bsp_map.h"
#include "fixed_math.h"

#if BSP_USE_HAND_MAP
extern const BspMapData g_hand_map;
const BspMapData *g_bsp_map = &g_hand_map;
#else
const BspMapData *g_bsp_map = &g_e1m1_map;
#endif

// Geometry (bsp_vertices/segs/subsectors/nodes, root, seg count, player start)
// is provided by the active map data file: src/generated_e1m1_map.c by default,
// or src/bsp_map_test.c when BSP_USE_HAND_MAP is defined. This file holds only
// the map logic (collision, doors, exit) which operates generically over the
// bsp_segs[0..bsp_seg_count) arrays.

// Interaction reach for the "use" action, squared (one cell).
#define BSP_USE_REACH2 ((s32)FX_ONE * FX_ONE)

static s32 bsp_muls_word(s32 left, s32 right) {
    s32 result = (s16)left;
    const s16 operand = (s16)right;
    __asm__ volatile (
        "muls.w %1,%0"
        : "+d" (result)
        : "d" (operand)
        : "cc");
    return result;
}

// Exact floor((numerator * 256) / denominator) for 0<=numerator<denominator.
// Eight shift/subtract steps avoid a generic 32-bit divide in circle collision.
static u16 bsp_ratio_q8(u32 numerator, u32 denominator) {
    u16 quotient = 0;
    for (u16 bit = 0; bit < 8; bit++) {
        numerator <<= 1;
        quotient <<= 1;
        if (numerator >= denominator) {
            numerator -= denominator;
            quotient++;
        }
    }
    return quotient;
}

// Runtime state is stored once per physical door. Every BSP face generated for
// that door points at the same group, so collision/rendering can never disagree.
// Lift is Q8: 0 is closed, 256 is fully raised. Sixteen units per vblank makes
// a full trip take 16 vblanks (~0.27 s on NTSC hardware) — blazing-door speed,
// because the seg only becomes passable at the fully-open endpoint.
#define BSP_DOOR_LIFT_MAX 256u
#define BSP_DOOR_LIFT_PER_VBLANK 16u
static u16 g_door_lift[BSP_MAX_DOORS];
static bool g_door_target_open[BSP_MAX_DOORS];
/* Byte generations halve this maximum-map cache.  A wrap clears it once per
 * 255 spatial queries, preserving the same duplicate suppression semantics. */
static u8 g_query_seen_generation[BSP_MAX_SEGS];
static u8 g_query_generation = 1;
static u16 g_visibility_revision = 1;
static u8 g_automap_mapped_bits[(MEGALDOOM_MAP_MAX_AUTOMAP_LINES + 7) / 8];
static u8 g_automap_visited_sector_bits[(MEGALDOOM_MAP_MAX_SECTORS + 7) / 8];

#if DEBUG_PERF
static BspDebugQueryOwner g_query_owner;
static u32 g_player_collision_subticks;
static u32 g_enemy_collision_subticks;
static u32 g_los_subticks;
static u16 g_collision_candidates;
static u16 g_los_candidates;
#endif

// Collision and line-of-sight scan all bsp_seg_count segs; on the 68000 the real
// cost is the per-seg s32 multiplies in seg_point_dist2 / cross3. We reject
// far-away segs first with a few cheap compares against the seg's AABB (min/max of
// its two vertices), computed inline from the vertex table — no multiply, and no
// extra RAM (MD has only 64KB, so a per-seg bbox array is too costly). Almost every
// seg is far from any given query and rejects here.

bool bsp_select_map(u16 level_index) {
#if BSP_USE_HAND_MAP
    (void)level_index;
    g_bsp_map = &g_hand_map;
    return TRUE;
#else
    if (level_index == 0) g_bsp_map = &g_e1m1_map;
    else if (level_index == 1) g_bsp_map = &g_e1m2_map;
    else return FALSE;
    return TRUE;
#endif
}

const BspMapData *bsp_current_map(void) { return g_bsp_map; }

bool bsp_sector_is_secret(u16 sector_index) {
    if (sector_index >= g_bsp_map->sector_count ||
        g_bsp_map->secret_sector_bits == NULL) {
        return FALSE;
    }
    return (bool)((g_bsp_map->secret_sector_bits[sector_index >> 3] &
                   (1u << (sector_index & 7))) != 0);
}

bool bsp_sector_is_sky(u16 sector_index) {
    if (sector_index >= g_bsp_map->sector_count ||
        g_bsp_map->sky_sector_bits == NULL) {
        return FALSE;
    }
    return (bool)((g_bsp_map->sky_sector_bits[sector_index >> 3] &
                   (1u << (sector_index & 7))) != 0);
}

void bsp_automap_mark_seg(u16 seg_index) {
    if (seg_index >= bsp_seg_count || g_bsp_map->seg_automap_lines == NULL) return;
    const u16 line = g_bsp_map->seg_automap_lines[seg_index];
    if (line >= bsp_automap_line_count) return;
    g_automap_mapped_bits[line >> 3] |= (u8)(1u << (line & 7));
}

void bsp_automap_mark_sector(u16 sector_index) {
    if (sector_index >= g_bsp_map->sector_count) return;
    g_automap_visited_sector_bits[sector_index >> 3] |=
        (u8)(1u << (sector_index & 7));
}

static bool bsp_automap_sector_visited(u8 sector_index) {
    if (sector_index == 0xFF || sector_index >= g_bsp_map->sector_count) return FALSE;
    return (bool)((g_automap_visited_sector_bits[sector_index >> 3] &
        (u8)(1u << (sector_index & 7))) != 0);
}

bool bsp_automap_line_visible(u16 line_index) {
    if (line_index >= bsp_automap_line_count || bsp_automap_lines == NULL) return FALSE;
    if ((g_automap_mapped_bits[line_index >> 3] &
         (u8)(1u << (line_index & 7))) != 0) return TRUE;
    const BspAutomapLine *line = &bsp_automap_lines[line_index];
    if (line->kind != BSP_AUTOMAP_LINE_FLOOR &&
        line->kind != BSP_AUTOMAP_LINE_CEILING) return FALSE;
    return (bool)(bsp_automap_sector_visited(line->front_sector) ||
                  bsp_automap_sector_visited(line->back_sector));
}

void bsp_map_reset(u16 phase_index) {
    bsp_select_map(phase_index);
    for (u16 i = 0; i < (MEGALDOOM_MAP_MAX_AUTOMAP_LINES + 7) / 8; i++) {
        g_automap_mapped_bits[i] = 0;
    }
    for (u16 i = 0; i < (MEGALDOOM_MAP_MAX_SECTORS + 7) / 8; i++) {
        g_automap_visited_sector_bits[i] = 0;
    }
    for (u16 i = 0; i < bsp_door_count && i < BSP_MAX_DOORS; i++) {
        g_door_lift[i] = 0;
        g_door_target_open[i] = FALSE;
    }
    g_visibility_revision++;
    if (g_visibility_revision == 0) g_visibility_revision = 1;
}

bool bsp_seg_is_open(u16 seg_index) {
    if (seg_index >= bsp_seg_count) return FALSE;
    const BspSeg *seg = &bsp_segs[seg_index];
    if (seg->type == BSP_SEG_TRIGGER) return TRUE;
    return (bool)(seg->type == BSP_SEG_DOOR &&
                  seg->door_group < bsp_door_count &&
                  seg->door_group < BSP_MAX_DOORS &&
                  g_door_lift[seg->door_group] == BSP_DOOR_LIFT_MAX);
}

u16 bsp_seg_door_lift(u16 seg_index) {
    if (seg_index >= bsp_seg_count) return 0;
    const BspSeg *seg = &bsp_segs[seg_index];
    if (seg->type != BSP_SEG_DOOR ||
        seg->door_group >= bsp_door_count ||
        seg->door_group >= BSP_MAX_DOORS) {
        return 0;
    }
    return g_door_lift[seg->door_group];
}

bool bsp_update_doors(u16 elapsed_vblanks) {
    const u16 delta = (elapsed_vblanks > (BSP_DOOR_LIFT_MAX / BSP_DOOR_LIFT_PER_VBLANK))
        ? BSP_DOOR_LIFT_MAX
        : (u16)(elapsed_vblanks * BSP_DOOR_LIFT_PER_VBLANK);
    bool changed = FALSE;

    for (u16 i = 0; i < bsp_door_count && i < BSP_MAX_DOORS; i++) {
        const bool was_open = (bool)(g_door_lift[i] == BSP_DOOR_LIFT_MAX);
        const u16 old_lift = g_door_lift[i];

        if (g_door_target_open[i]) {
            const u16 remaining = (u16)(BSP_DOOR_LIFT_MAX - g_door_lift[i]);
            g_door_lift[i] = (delta >= remaining)
                ? BSP_DOOR_LIFT_MAX : (u16)(g_door_lift[i] + delta);
        } else {
            g_door_lift[i] = (delta >= g_door_lift[i])
                ? 0 : (u16)(g_door_lift[i] - delta);
        }

        if (g_door_lift[i] != old_lift) changed = TRUE;
        if (was_open != (g_door_lift[i] == BSP_DOOR_LIFT_MAX)) {
            g_visibility_revision++;
            if (g_visibility_revision == 0) g_visibility_revision = 1;
        }
    }
    return changed;
}

u16 bsp_get_visibility_revision(void) { return g_visibility_revision; }

u16 bsp_find_subsector(s32 x, s32 y) {
    u16 child = bsp_root_node;
    while (!BSP_CHILD_IS_SUBSECTOR(child)) {
        const BspNode *node = &bsp_nodes[child];
        const s32 cross = bsp_muls_word(x - node->px, node->dy) -
                          bsp_muls_word(y - node->py, node->dx);
        child = (cross >= 0) ? node->front : node->back;
    }
    return BSP_CHILD_INDEX(child);
}

u16 bsp_find_subsector_with_margin(s32 x, s32 y, s32 radius, bool *contained) {
    u16 child = bsp_root_node;
    bool stays_in_leaf = TRUE;

    if (radius < 0) radius = 0;
    while (!BSP_CHILD_IS_SUBSECTOR(child)) {
        const BspNode *node = &bsp_nodes[child];
        const s32 dx = x - node->px;
        const s32 dy = y - node->py;
        const s32 cross = bsp_muls_word(dx, node->dy) - bsp_muls_word(dy, node->dx);
        const s32 abs_cross = (cross < 0) ? -cross : cross;
        const s32 abs_node_dx = (node->dx < 0) ? -(s32)node->dx : node->dx;
        const s32 abs_node_dy = (node->dy < 0) ? -(s32)node->dy : node->dy;

        // Distance to this partition is |cross| / hypot(dx,dy). L1 is never
        // smaller than hypot, so |cross| > radius * L1 proves the whole disk
        // remains on this side. Any ambiguous boundary is conservatively kept.
        if (abs_cross <= radius * (abs_node_dx + abs_node_dy)) {
            stays_in_leaf = FALSE;
        }
        child = (cross >= 0) ? node->front : node->back;
    }

    if (contained != NULL) *contained = stays_in_leaf;
    return BSP_CHILD_INDEX(child);
}

// Squared distance from point (px, py) to the finite segment of seg s.
static s32 seg_point_dist2(const BspSeg *s, s32 px, s32 py) {
    const BspVertex *a = &bsp_vertices[s->v1];
    const BspVertex *b = &bsp_vertices[s->v2];
    const s32 abx = (s32)b->x - a->x;
    const s32 aby = (s32)b->y - a->y;
    const s32 apx = px - a->x;
    const s32 apy = py - a->y;
    const s32 ab2 = bsp_muls_word(abx, abx) + bsp_muls_word(aby, aby);
    s32 cx, cy;

    if (ab2 <= 0) {
        cx = a->x;
        cy = a->y;
    } else {
        const s32 dot = bsp_muls_word(apx, abx) + bsp_muls_word(apy, aby);
        if (dot <= 0) {
            cx = a->x;
            cy = a->y;
        } else if (dot >= ab2) {
            cx = b->x;
            cy = b->y;
        } else {
            const s32 tq = bsp_ratio_q8((u32)dot, (u32)ab2);
            cx = a->x + (bsp_muls_word(abx, tq) >> FX_SHIFT);
            cy = a->y + (bsp_muls_word(aby, tq) >> FX_SHIFT);
        }
    }

    const s32 dx = px - cx;
    const s32 dy = py - cy;
    return bsp_muls_word(dx, dx) + bsp_muls_word(dy, dy);
}

static void clear_query_seen(void) {
    g_query_generation++;
    if (g_query_generation == 0) {
        for (u16 i = 0; i < BSP_MAX_SEGS; i++) g_query_seen_generation[i] = 0;
        g_query_generation = 1;
    }
}

static bool mark_query_seg(u16 seg_index) {
    if (g_query_seen_generation[seg_index] == g_query_generation) return FALSE;
    g_query_seen_generation[seg_index] = g_query_generation;
    return TRUE;
}

// Mathematical floor division by the 256-unit cell size. C right-shift/division
// of negatives is implementation-dependent/toward zero, so handle it explicitly.
static s32 grid_coord(s32 value, s32 origin) {
    const s32 relative = value - origin;
    if (relative >= 0) return relative >> BSP_GRID_CELL_SHIFT;
    return -(((-relative) + BSP_GRID_CELL_SIZE - 1) >> BSP_GRID_CELL_SHIFT);
}

static bool grid_cell_valid(s32 cx, s32 cy) {
    return (bool)((cx >= 0) && (cy >= 0) &&
                  (cx < bsp_grid_width) && (cy < bsp_grid_height));
}

bool bsp_circle_blocked(s32 x, s32 y, s32 radius) {
    const s32 r2 = bsp_muls_word(radius, radius);
    s32 cx0 = grid_coord(x - radius, bsp_grid_min_x);
    s32 cx1 = grid_coord(x + radius, bsp_grid_min_x);
    s32 cy0 = grid_coord(y - radius, bsp_grid_min_y);
    s32 cy1 = grid_coord(y + radius, bsp_grid_min_y);
#if DEBUG_PERF
    const u32 query_start = getSubTick();
#endif
    bool blocked = FALSE;

    if (cx0 < 0) cx0 = 0;
    if (cy0 < 0) cy0 = 0;
    if (cx1 >= bsp_grid_width) cx1 = bsp_grid_width - 1;
    if (cy1 >= bsp_grid_height) cy1 = bsp_grid_height - 1;
    clear_query_seen();
    for (s32 cy = cy0; cy <= cy1 && !blocked; cy++) {
        for (s32 cx = cx0; cx <= cx1 && !blocked; cx++) {
            if (!grid_cell_valid(cx, cy)) continue;
            const u16 cell = (u16)(cy * bsp_grid_width + cx);
            const u16 end = bsp_grid_cell_offsets[cell + 1];
            for (u16 p = bsp_grid_cell_offsets[cell]; p < end; p++) {
                const u16 i = bsp_grid_seg_indices[p];
                if (!mark_query_seg(i)) continue;
#if DEBUG_PERF
                g_collision_candidates++;
#endif
                if (bsp_seg_is_open(i)) {
                    continue;
                }
                const BspSeg *s = &bsp_segs[i];
                const BspVertex *a = &bsp_vertices[s->v1];
                const BspVertex *b = &bsp_vertices[s->v2];
                // Cheap AABB rejection avoids the exact distance calculation
                // for almost every segment in the candidate cells.
                const s16 minx = (a->x < b->x) ? a->x : b->x;
                const s16 maxx = (a->x > b->x) ? a->x : b->x;
                const s16 miny = (a->y < b->y) ? a->y : b->y;
                const s16 maxy = (a->y > b->y) ? a->y : b->y;
                if ((x + radius < minx) || (x - radius > maxx) ||
                    (y + radius < miny) || (y - radius > maxy)) {
                    continue;
                }
                if (seg_point_dist2(s, x, y) < r2) {
                    blocked = TRUE;
                    break;
                }
            }
        }
    }
#if DEBUG_PERF
    if (g_query_owner == BSP_QUERY_PLAYER) g_player_collision_subticks += getSubTick() - query_start;
    else g_enemy_collision_subticks += getSubTick() - query_start;
#endif
    return blocked;
}

static s32 cross3(s32 ax, s32 ay, s32 bx, s32 by, s32 cx, s32 cy) {
    return bsp_muls_word(bx - ax, cy - ay) -
           bsp_muls_word(by - ay, cx - ax);
}

static bool point_on_segment(s32 ax, s32 ay, s32 bx, s32 by, s32 px, s32 py) {
    const s32 min_x = (ax < bx) ? ax : bx;
    const s32 max_x = (ax > bx) ? ax : bx;
    const s32 min_y = (ay < by) ? ay : by;
    const s32 max_y = (ay > by) ? ay : by;
    return (bool)((px >= min_x) && (px <= max_x) &&
                  (py >= min_y) && (py <= max_y));
}

static bool segment_hits_wall(s32 x0, s32 y0, s32 x1, s32 y1,
                              bool count_endpoint_touch) {
    // Ray AABB, used to reject non-overlapping segs before the 4 cross3 multiplies.
    const s32 ray_minx = (x0 < x1) ? x0 : x1;
    const s32 ray_maxx = (x0 > x1) ? x0 : x1;
    const s32 ray_miny = (y0 < y1) ? y0 : y1;
    const s32 ray_maxy = (y0 > y1) ? y0 : y1;

    s32 cx = grid_coord(x0, bsp_grid_min_x);
    s32 cy = grid_coord(y0, bsp_grid_min_y);
    const s32 end_cx = grid_coord(x1, bsp_grid_min_x);
    const s32 end_cy = grid_coord(y1, bsp_grid_min_y);
    const s32 step_x = (end_cx > cx) ? 1 : ((end_cx < cx) ? -1 : 0);
    const s32 step_y = (end_cy > cy) ? 1 : ((end_cy < cy) ? -1 : 0);
    const s32 ray_dx = (x1 >= x0) ? x1 - x0 : x0 - x1;
    const s32 ray_dy = (y1 >= y0) ? y1 - y0 : y0 - y1;
    bool hit = FALSE;
#if DEBUG_PERF
    const u32 query_start = getSubTick();
#endif

    clear_query_seen();
    while (TRUE) {
        if (grid_cell_valid(cx, cy)) {
            const u16 cell = (u16)(cy * bsp_grid_width + cx);
            const u16 end = bsp_grid_cell_offsets[cell + 1];
            for (u16 p = bsp_grid_cell_offsets[cell]; p < end; p++) {
                const u16 i = bsp_grid_seg_indices[p];
                if (!mark_query_seg(i)) continue;
#if DEBUG_PERF
                g_los_candidates++;
#endif
                if (bsp_seg_is_open(i)) {
                    continue;
                }
                const BspVertex *a = &bsp_vertices[bsp_segs[i].v1];
                const BspVertex *b = &bsp_vertices[bsp_segs[i].v2];
                // Broad-phase: if the ray AABB and seg AABB don't overlap,
                // there can be no crossing.
                const s16 seg_minx = (a->x < b->x) ? a->x : b->x;
                const s16 seg_maxx = (a->x > b->x) ? a->x : b->x;
                const s16 seg_miny = (a->y < b->y) ? a->y : b->y;
                const s16 seg_maxy = (a->y > b->y) ? a->y : b->y;
                if ((ray_maxx < seg_minx) || (ray_minx > seg_maxx) ||
                    (ray_maxy < seg_miny) || (ray_miny > seg_maxy)) {
                    continue;
                }
                const s32 d1 = cross3(x0, y0, x1, y1, a->x, a->y);
                const s32 d2 = cross3(x0, y0, x1, y1, b->x, b->y);
                const s32 d3 = cross3(a->x, a->y, b->x, b->y, x0, y0);
                const s32 d4 = cross3(a->x, a->y, b->x, b->y, x1, y1);

                const bool proper_cross =
                    ((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) &&
                    ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0));
                const bool endpoint_touch =
                    (d1 == 0 && point_on_segment(x0, y0, x1, y1, a->x, a->y)) ||
                    (d2 == 0 && point_on_segment(x0, y0, x1, y1, b->x, b->y)) ||
                    (d3 == 0 && point_on_segment(a->x, a->y, b->x, b->y, x0, y0)) ||
                    (d4 == 0 && point_on_segment(a->x, a->y, b->x, b->y, x1, y1));

                if (proper_cross || (count_endpoint_touch && endpoint_touch)) {
                    hit = TRUE;
                    break;
                }
            }
        }
        if (hit || (cx == end_cx && cy == end_cy)) break;

        // Supercover grid walk. At a corner crossing advance both axes; segment
        // AABBs are inserted inclusively, so either adjacent cell contains every
        // wall lying exactly on the shared boundary.
        if (step_x == 0) { cy += step_y; continue; }
        if (step_y == 0) { cx += step_x; continue; }
        if (cx == end_cx) { cy += step_y; continue; }
        if (cy == end_cy) { cx += step_x; continue; }
        const s32 x_boundary = bsp_grid_min_x +
            ((step_x > 0 ? cx + 1 : cx) * BSP_GRID_CELL_SIZE);
        const s32 y_boundary = bsp_grid_min_y +
            ((step_y > 0 ? cy + 1 : cy) * BSP_GRID_CELL_SIZE);
        const s32 x_distance = (step_x > 0) ? x_boundary - x0 : x0 - x_boundary;
        const s32 y_distance = (step_y > 0) ? y_boundary - y0 : y0 - y_boundary;
        const s32 lhs = bsp_muls_word(x_distance, ray_dy);
        const s32 rhs = bsp_muls_word(y_distance, ray_dx);
        if (lhs == rhs) { cx += step_x; cy += step_y; }
        else if (lhs < rhs) cx += step_x;
        else cy += step_y;
    }
#if DEBUG_PERF
    g_los_subticks += getSubTick() - query_start;
#endif
    return hit;
}

bool bsp_segment_hits_wall(s32 x0, s32 y0, s32 x1, s32 y1) {
    return segment_hits_wall(x0, y0, x1, y1, TRUE);
}

bool bsp_segment_crosses_wall(s32 x0, s32 y0, s32 x1, s32 y1) {
    return segment_hits_wall(x0, y0, x1, y1, FALSE);
}

#if DEBUG_PERF
void bsp_debug_set_query_owner(BspDebugQueryOwner owner) { g_query_owner = owner; }
void bsp_debug_reset_query_stats(void) {
    g_player_collision_subticks = 0;
    g_enemy_collision_subticks = 0;
    g_los_subticks = 0;
    g_collision_candidates = 0;
    g_los_candidates = 0;
}
u32 bsp_get_debug_player_collision_subticks(void) { return g_player_collision_subticks; }
u32 bsp_get_debug_enemy_collision_subticks(void) { return g_enemy_collision_subticks; }
u32 bsp_get_debug_los_subticks(void) { return g_los_subticks; }
u16 bsp_get_debug_collision_candidates(void) { return g_collision_candidates; }
u16 bsp_get_debug_los_candidates(void) { return g_los_candidates; }
#endif

static void toggle_door(u8 door_group) {
    if (door_group >= bsp_door_count || door_group >= BSP_MAX_DOORS) return;
    g_door_target_open[door_group] = !g_door_target_open[door_group];
}

BspUseResult bsp_use_in_front(s32 x, s32 y, u16 angle, u8 owned_keys) {
    const s16 dir_x = fx_cos(angle);
    const s16 dir_y = fx_sin(angle);
    BspUseResult result = {DOOR_ACTION_NONE, BSP_KEY_NONE, BSP_USE_TARGET_NONE};
    const BspSeg *best = NULL;
    u16 best_index = BSP_USE_TARGET_NONE;
    u32 best_dist2 = 0;
    s32 best_probe_dist = 0;

    for (s32 dist = FX_ONE / 2; dist <= FX_ONE * 2; dist += FX_ONE / 2) {
        const s32 px = x + (((s32)dir_x * dist) >> FX_SHIFT);
        const s32 py = y + (((s32)dir_y * dist) >> FX_SHIFT);

        for (u16 i = 0; i < bsp_seg_count; i++) {
            const BspSeg *s = &bsp_segs[i];
            if (s->type == BSP_SEG_WALL ||
                (s->type == BSP_SEG_DOOR &&
                 (s->flags & BSP_SEG_FLAG_DIRECT_USE) == 0)) {
                continue;
            }
            const u32 candidate_dist2 = seg_point_dist2(s, px, py);
            if (candidate_dist2 >= BSP_USE_REACH2) {
                continue;
            }
            // A BSP's storage order is unrelated to what the player is aiming
            // at. Keep evaluating every probe and choose the surface nearest
            // the aim ray. This keeps a nearby door from becoming an exit,
            // while the exit panel wins only when the player actually aims at
            // it instead of whichever SEG was emitted first.
            if (best == NULL || candidate_dist2 < best_dist2 ||
                (candidate_dist2 == best_dist2 && dist < best_probe_dist)) {
                best = s;
                best_index = i;
                best_dist2 = candidate_dist2;
                best_probe_dist = dist;
            }
        }
    }

    if (best == NULL) return result;
    result.target = (best->type == BSP_SEG_EXIT) ? (u8)best_index : best->door_group;
    if (best->type == BSP_SEG_EXIT) {
        result.action = DOOR_ACTION_EXIT;
        return result;
    }
    result.required_key = best->required_key;
    if ((owned_keys & best->required_key) != best->required_key) {
        result.action = DOOR_ACTION_LOCKED;
        return result;
    }
    toggle_door(best->door_group);
    result.action = (best->required_key == BSP_KEY_NONE)
        ? DOOR_ACTION_TOGGLED : DOOR_ACTION_UNLOCKED;
    return result;
}
