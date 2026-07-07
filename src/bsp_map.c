#include "bsp_map.h"
#include "fixed_math.h"

// Geometry (bsp_vertices/segs/subsectors/nodes, root, seg count, player start)
// is provided by the active map data file: src/generated_e1m1_map.c by default,
// or src/bsp_map_test.c when BSP_USE_HAND_MAP is defined. This file holds only
// the map logic (collision, doors, exit) which operates generically over the
// bsp_segs[0..bsp_seg_count) arrays.

// Interaction reach for the "use" action, squared (one cell).
#define BSP_USE_REACH2 ((s32)FX_ONE * FX_ONE)

// Runtime open state per seg (only door segs are ever set open).
static bool g_seg_open[BSP_MAX_SEGS];

void bsp_map_reset(u16 phase_index) {
    (void)phase_index;
    for (u16 i = 0; i < bsp_seg_count; i++) {
        g_seg_open[i] = FALSE;
    }
}

bool bsp_seg_is_open(u16 seg_index) {
    return (seg_index < bsp_seg_count) ? g_seg_open[seg_index] : FALSE;
}

// Squared distance from point (px, py) to the finite segment of seg s.
static s32 seg_point_dist2(const BspSeg *s, s32 px, s32 py) {
    const BspVertex *a = &bsp_vertices[s->v1];
    const BspVertex *b = &bsp_vertices[s->v2];
    const s32 abx = (s32)b->x - a->x;
    const s32 aby = (s32)b->y - a->y;
    const s32 apx = px - a->x;
    const s32 apy = py - a->y;
    const s32 ab2 = abx * abx + aby * aby;
    s32 cx, cy;

    if (ab2 <= 0) {
        cx = a->x;
        cy = a->y;
    } else {
        const s32 dot = apx * abx + apy * aby;
        if (dot <= 0) {
            cx = a->x;
            cy = a->y;
        } else if (dot >= ab2) {
            cx = b->x;
            cy = b->y;
        } else {
            const s32 tq = (dot << FX_SHIFT) / ab2; // 0..256
            cx = a->x + ((abx * tq) >> FX_SHIFT);
            cy = a->y + ((aby * tq) >> FX_SHIFT);
        }
    }

    const s32 dx = px - cx;
    const s32 dy = py - cy;
    return dx * dx + dy * dy;
}

bool bsp_circle_blocked(s32 x, s32 y, s32 radius) {
    const s32 r2 = radius * radius;
    for (u16 i = 0; i < bsp_seg_count; i++) {
        if (g_seg_open[i]) {
            continue;
        }
        if (seg_point_dist2(&bsp_segs[i], x, y) < r2) {
            return TRUE;
        }
    }
    return FALSE;
}

static s32 cross3(s32 ax, s32 ay, s32 bx, s32 by, s32 cx, s32 cy) {
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

bool bsp_segment_hits_wall(s32 x0, s32 y0, s32 x1, s32 y1) {
    for (u16 i = 0; i < bsp_seg_count; i++) {
        if (g_seg_open[i]) {
            continue;
        }
        const BspVertex *a = &bsp_vertices[bsp_segs[i].v1];
        const BspVertex *b = &bsp_vertices[bsp_segs[i].v2];
        const s32 d1 = cross3(x0, y0, x1, y1, a->x, a->y);
        const s32 d2 = cross3(x0, y0, x1, y1, b->x, b->y);
        const s32 d3 = cross3(a->x, a->y, b->x, b->y, x0, y0);
        const s32 d4 = cross3(a->x, a->y, b->x, b->y, x1, y1);

        if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) &&
            ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0))) {
            return TRUE;
        }
    }
    return FALSE;
}

// Toggle a door and every coincident door seg (same vertex pair) so the two
// back-to-back one-sided door faces open/close together.
static void toggle_door(u16 seg_index) {
    const bool new_state = !g_seg_open[seg_index];
    const u16 a = bsp_segs[seg_index].v1;
    const u16 b = bsp_segs[seg_index].v2;

    for (u16 j = 0; j < bsp_seg_count; j++) {
        if (bsp_segs[j].type != BSP_SEG_DOOR && bsp_segs[j].type != BSP_SEG_LOCKED_DOOR) {
            continue;
        }
        const u16 ja = bsp_segs[j].v1;
        const u16 jb = bsp_segs[j].v2;
        if ((ja == a && jb == b) || (ja == b && jb == a)) {
            g_seg_open[j] = new_state;
        }
    }
}

DoorActionResult bsp_use_in_front(s32 x, s32 y, u16 angle, bool has_key, bool *consumed_key) {
    const s16 dir_x = fx_cos(angle);
    const s16 dir_y = fx_sin(angle);

    if (consumed_key != NULL) {
        *consumed_key = FALSE;
    }

    for (s32 dist = FX_ONE / 2; dist <= FX_ONE * 2; dist += FX_ONE / 2) {
        const s32 px = x + (((s32)dir_x * dist) >> FX_SHIFT);
        const s32 py = y + (((s32)dir_y * dist) >> FX_SHIFT);

        for (u16 i = 0; i < bsp_seg_count; i++) {
            const BspSeg *s = &bsp_segs[i];
            if (s->type == BSP_SEG_WALL) {
                continue;
            }
            if (seg_point_dist2(s, px, py) >= BSP_USE_REACH2) {
                continue;
            }

            if (s->type == BSP_SEG_EXIT) {
                return DOOR_ACTION_EXIT;
            }
            if (s->type == BSP_SEG_DOOR) {
                toggle_door(i);
                return DOOR_ACTION_TOGGLED;
            }
            if (s->type == BSP_SEG_LOCKED_DOOR) {
                if (!has_key) {
                    return DOOR_ACTION_LOCKED;
                }
                toggle_door(i);
                if (consumed_key != NULL) {
                    *consumed_key = TRUE;
                }
                return DOOR_ACTION_UNLOCKED;
            }
        }
    }

    return DOOR_ACTION_NONE;
}
