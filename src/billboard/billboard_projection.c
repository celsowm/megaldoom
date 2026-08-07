#include "billboard_internal.h"
#include "billboard_effects.h"
#include "bsp_render.h"
#include "renderer_perf.h"

typedef struct {
    u8 index;
    BillboardMeasure measure;
} BillboardProjectionOrder;

// Static rather than stack-resident: imported E1M1 content needs many more
// active objects than the draw budget while the Mega Drive stack stays small.
// Only the nearest drawable objects are retained, so dense rooms never pay for
// sorting every active map thing.
static BillboardProjectionOrder s_order[BILLBOARD_MAX_PROJECTED_OBJECTS];

// Max-depth segment tree for the 80 sampled wall columns. A billboard span is
// visible iff its depth is smaller than the maximum wall depth in that span.
// This replaces as many as 80 dependent RAM reads per active THING with an
// exact O(log 128) query.
#define BILLBOARD_DEPTH_TREE_LEAVES 128
static u16 s_wall_depth_max[BILLBOARD_DEPTH_TREE_LEAVES * 2];

static void billboard_build_depth_tree(const RayColumn *columns) {
    for (u16 i = 0; i < BILLBOARD_DEPTH_TREE_LEAVES; i++) {
        s_wall_depth_max[BILLBOARD_DEPTH_TREE_LEAVES + i] =
            (i < (RAY_VIEW_COLS / RAY_COL_STRIDE)) ?
                columns[i * RAY_COL_STRIDE].depth : 0;
    }
    for (u16 i = BILLBOARD_DEPTH_TREE_LEAVES - 1; i > 0; i--) {
        const u16 left = s_wall_depth_max[i << 1];
        const u16 right = s_wall_depth_max[(i << 1) + 1];
        s_wall_depth_max[i] = (left > right) ? left : right;
    }
}

static u16 billboard_depth_range_max(u16 left, u16 right) {
    u16 maximum = 0;
    left = (u16)(left + BILLBOARD_DEPTH_TREE_LEAVES);
    right = (u16)(right + BILLBOARD_DEPTH_TREE_LEAVES);
    while (left <= right) {
        if (left & 1) {
            if (s_wall_depth_max[left] > maximum) maximum = s_wall_depth_max[left];
            left++;
        }
        if ((right & 1) == 0) {
            if (s_wall_depth_max[right] > maximum) maximum = s_wall_depth_max[right];
            right--;
        }
        left >>= 1;
        right >>= 1;
    }
    return maximum;
}

typedef struct {
    s32 object_x;
    s32 object_y;
    s32 forward;
    u16 camera_generation;
    u16 geometry_key;
    s16 left;
    s16 right;
    s16 top;
    s16 bottom;
    u8 atlas_x;
    u8 atlas_y;
    u8 atlas_w;
    u8 atlas_h;
    bool measured;
} BillboardMeasureCache;

// Overlay-only enemy animation used to reproject every active map object even
// while the camera and almost every object were unchanged. Cache both accepted
// and rejected measurements. Wall-span occlusion and nearest-object selection
// still run every redraw against the current column buffer.
static BillboardMeasureCache s_measure_cache[BILLBOARD_OBJECT_COUNT];
#if DEBUG_PERF || BILLBOARD_VISIBLE_SUBSECTOR_CULL
typedef struct {
    s32 object_x;
    s32 object_y;
    u16 subsector_id;
    bool contained;
    bool valid;
} BillboardSubsectorCache;

// The lookup is cached by object position so the oracle itself does not turn
// every redraw into active_count BSP searches. It deliberately keys on the
// point anchor only: measuring conservative sprite/leaf overlap is a separate
// correctness problem for any eventual culling implementation.
static BillboardSubsectorCache s_subsector_cache[BILLBOARD_OBJECT_COUNT];
#if DEBUG_PERF
static u16 s_debug_visible_subsector_objects;
static u16 s_debug_safe_subsector_objects;
static u16 s_debug_cullable_subsector_objects;
#endif

static void billboard_subsector_visibility(u16 index, const BillboardObject *object,
                                           bool *visited, bool *contained) {
    BillboardSubsectorCache *cache = &s_subsector_cache[index];
    if (!cache->valid || cache->object_x != object->x || cache->object_y != object->y) {
        cache->object_x = object->x;
        cache->object_y = object->y;
        cache->subsector_id = bsp_find_subsector_with_margin(
            object->x, object->y, BILLBOARD_SUBSECTOR_CULL_RADIUS, &cache->contained);
        cache->valid = TRUE;
    }
    *visited = bsp_subsector_was_visited(cache->subsector_id);
    *contained = cache->contained;
}
#endif
static s32 s_cache_player_x;
static s32 s_cache_player_y;
static u16 s_cache_player_angle;
static u16 s_cache_generation = 1;
static bool s_cache_context_valid;
#if DEBUG_PERF
static u16 s_debug_culled;
static u16 s_debug_candidates;
static u16 s_debug_occluded;
static u16 s_debug_projected;
static u16 s_debug_cache_hits;
static u16 s_debug_cache_misses;
#endif

static u16 billboard_project_one(const BillboardObject *object,
                                 const BillboardMeasure *measure,
                                 ProjectedBillboard *projected) {
    projected->left = measure->left;
    projected->right = measure->right;
    projected->top = measure->top;
    projected->bottom = measure->bottom;
    projected->depth = (u16)measure->forward;
    projected->visual_id = billboard_get_object_visual_id(object, measure->type);
    projected->frame = billboard_get_object_frame(object);
    projected->atlas_x = measure->atlas_x;
    projected->atlas_y = measure->atlas_y;
    projected->atlas_w = measure->atlas_w;
    projected->atlas_h = measure->atlas_h;
    return 1;
}

static u16 billboard_geometry_key(const BillboardObject *object) {
    const BillboardType *type = billboard_get_type(object->type_id);
    const u8 visual = billboard_get_object_visual_id(object, type);
    // Every visual whose geometry varies per frame must fold the frame in, or a
    // stationary object animating in front of a stationary camera keeps serving
    // the cached top/bottom of its first pose. Enemies joined this list on
    // 2026-08-07 when death poses got their own boxes (ENEMY_FRAME_GEOMETRY);
    // without it, a corpse would still be drawn at standing height.
    const u8 geometry_frame = ((visual == BILLBOARD_VISUAL_BARREL_EXPLODING) ||
                               (visual == BILLBOARD_VISUAL_DUMMY) ||
                               (visual == BILLBOARD_VISUAL_DUMMY_DAMAGED)) ?
        billboard_get_object_frame(object) : 0;

    return (u16)((object->type_id & 0x1Fu) |
                 ((u16)(visual & 0x1Fu) << 5) |
                 ((u16)(geometry_frame & 0x0Fu) << 10));
}

static void billboard_projection_cache_begin(const PlayerState *player) {
    if (s_cache_context_valid &&
        s_cache_player_x == player->x &&
        s_cache_player_y == player->y &&
        s_cache_player_angle == player->angle) {
        return;
    }

    s_cache_player_x = player->x;
    s_cache_player_y = player->y;
    s_cache_player_angle = player->angle;
    s_cache_context_valid = TRUE;
    s_cache_generation++;
    if (s_cache_generation == 0) {
        for (u16 i = 0; i < BILLBOARD_OBJECT_COUNT; i++) {
            s_measure_cache[i].camera_generation = 0;
        }
        s_cache_generation = 1;
    }
}

static bool billboard_measure_cached(u16 index,
                                     const PlayerState *player,
                                     s16 cos_a,
                                     s16 sin_a,
                                     BillboardMeasure *measure) {
    const BillboardObject *object = &g_billboards[index];
    BillboardMeasureCache *cache = &s_measure_cache[index];
    const u16 geometry_key = billboard_geometry_key(object);

    if (cache->camera_generation == s_cache_generation &&
        cache->object_x == object->x &&
        cache->object_y == object->y &&
        cache->geometry_key == geometry_key) {
#if DEBUG_PERF
        s_debug_cache_hits++;
#endif
        if (cache->measured) {
            measure->type = billboard_get_type(object->type_id);
            measure->forward = cache->forward;
            // These intermediate projection values are not consumed after a
            // cache hit, but initialize the complete public-internal record so
            // copying it into the nearest-object order never carries stale data.
            measure->side = 0;
            measure->center_col = 0;
            measure->half_w = 0;
            measure->projected_height = 0;
            measure->left = cache->left;
            measure->right = cache->right;
            measure->top = cache->top;
            measure->bottom = cache->bottom;
            measure->atlas_x = cache->atlas_x;
            measure->atlas_y = cache->atlas_y;
            measure->atlas_w = cache->atlas_w;
            measure->atlas_h = cache->atlas_h;
        }
        return cache->measured;
    }

#if DEBUG_PERF
    s_debug_cache_misses++;
#endif

    cache->object_x = object->x;
    cache->object_y = object->y;
    cache->geometry_key = geometry_key;
    cache->measured = billboard_measure_object(
        player, cos_a, sin_a, object, measure);
    cache->camera_generation = s_cache_generation;
    if (cache->measured) {
        cache->forward = measure->forward;
        cache->left = measure->left;
        cache->right = measure->right;
        cache->top = measure->top;
        cache->bottom = measure->bottom;
        cache->atlas_x = measure->atlas_x;
        cache->atlas_y = measure->atlas_y;
        cache->atlas_w = measure->atlas_w;
        cache->atlas_h = measure->atlas_h;
    }
    return cache->measured;
}

// The wall buffer is sampled once per RAY_COL_STRIDE pixels. A billboard is
// drawable when any sampled block it covers is in front of that block's wall;
// individual pixels are still z-tested while rasterizing. This avoids making a
// wide sprite vanish merely because its centre lies behind a pillar.
static bool billboard_span_has_visible_block(const BillboardMeasure *measure,
                                             const RayColumn *columns) {
    s16 left = measure->left;
    s16 right = measure->right;

    if (right < 0 || left >= RAY_VIEW_COLS) {
        return FALSE;
    }
    if (left < 0) left = 0;
    if (right >= RAY_VIEW_COLS) right = RAY_VIEW_COLS - 1;

    const u16 first = (u16)left / RAY_COL_STRIDE;
    const u16 last = (u16)right / RAY_COL_STRIDE;
    (void)columns;
    return (bool)(measure->forward < billboard_depth_range_max(first, last));
}

u16 billboard_project_scene(const PlayerState *player,
                            const RayColumn *columns,
                            ProjectedBillboard *objects,
                            u16 max_objects) {
    // Keep a fixed nearest-object set while scanning the full imported map. The
    // renderer's budget is intentionally small (12) on the Mega Drive, so this
    // is O(map_objects * draw_budget), not an O(visible_objects^2) insertion sort.
    u16 selected = 0;
    const u16 budget = (max_objects < BILLBOARD_MAX_PROJECTED_OBJECTS) ?
                           max_objects : BILLBOARD_MAX_PROJECTED_OBJECTS;
    u16 farthest = 0;
    bool farthest_valid = FALSE;

#if DEBUG_PERF
    s_debug_culled = 0;
    s_debug_candidates = 0;
    s_debug_occluded = 0;
    s_debug_projected = 0;
    s_debug_cache_hits = 0;
    s_debug_cache_misses = 0;
    s_debug_visible_subsector_objects = 0;
    s_debug_safe_subsector_objects = 0;
    s_debug_cullable_subsector_objects = 0;
#endif

    // Compute the view basis once and share it across every object instead of
    // redoing fx_cos/fx_sin per object inside billboard_measure_object.
    const s16 cos_a = fx_cos(player->angle);
    const s16 sin_a = fx_sin(player->angle);

    billboard_projection_cache_begin(player);
    billboard_build_depth_tree(columns);

    if (budget == 0) {
        return 0;
    }

    const u8 *active_indices = billboard_registry_active_indices();
    const u16 active_count = billboard_registry_active_count();
    for (u16 slot = 0; slot < active_count; slot++) {
        const u16 i = active_indices[slot];
        const BillboardObject *object = &g_billboards[i];
        BillboardMeasure measure;

#if DEBUG_PERF || BILLBOARD_VISIBLE_SUBSECTOR_CULL
        bool subsector_visited;
        bool subsector_contained;
        billboard_subsector_visibility(i, object, &subsector_visited, &subsector_contained);
#if DEBUG_PERF
        if (subsector_visited) {
            s_debug_visible_subsector_objects++;
        }
        if (subsector_contained) s_debug_safe_subsector_objects++;
        if (subsector_contained && !subsector_visited) {
            s_debug_cullable_subsector_objects++;
        }
#endif
#if BILLBOARD_VISIBLE_SUBSECTOR_CULL
        if (subsector_contained && !subsector_visited) continue;
#endif
#endif

        if (!billboard_measure_cached(i, player, cos_a, sin_a, &measure)) {
            continue;
        }
        if ((measure.right < 0) || (measure.left >= RAY_VIEW_COLS)) {
#if DEBUG_PERF
            s_debug_culled++;
#endif
            continue;
        }
#if DEBUG_PERF
        s_debug_candidates++;
#endif
        if (!billboard_span_has_visible_block(&measure, columns)) {
#if DEBUG_PERF
            s_debug_occluded++;
#endif
            continue;
        }
        if (selected < budget) {
            s_order[selected].index = (u8)i;
            s_order[selected].measure = measure;
            selected++;
            farthest_valid = FALSE;
        } else {
            // Replace only the farthest retained object. At the cutoff, retain
            // the later source entries: that matches the old stable full sort
            // followed by its nearest-object suffix.
            //
            // The farthest slot only changes when it's actually replaced below,
            // so cache it across candidates instead of rescanning all `selected`
            // slots for every overflow candidate: a candidate that isn't nearer
            // than the cached farthest is rejected in O(1), and the O(budget)
            // scan only runs again once, lazily, after an actual replacement.
            if (!farthest_valid) {
                farthest = 0;
                for (u16 candidate = 1; candidate < selected; candidate++) {
                    if ((s_order[candidate].measure.forward > s_order[farthest].measure.forward) ||
                        ((s_order[candidate].measure.forward == s_order[farthest].measure.forward) &&
                         (s_order[candidate].index < s_order[farthest].index))) {
                        farthest = candidate;
                    }
                }
                farthest_valid = TRUE;
            }
            if ((measure.forward < s_order[farthest].measure.forward) ||
                ((measure.forward == s_order[farthest].measure.forward) &&
                 (i > s_order[farthest].index))) {
                s_order[farthest].index = (u8)i;
                s_order[farthest].measure = measure;
                farthest_valid = FALSE;
            }
        }
    }

    // Painter order is far (large forward) -> near (small forward), so nearer
    // sprite pixels overwrite farther ones in any shared column.
    for (u16 a = 1; a < selected; a++) {
        const u8 idx = s_order[a].index;
        const BillboardMeasure measure = s_order[a].measure;
        s16 b = (s16)(a - 1);

        while ((b >= 0) &&
               ((s_order[b].measure.forward < measure.forward) ||
                ((s_order[b].measure.forward == measure.forward) &&
                 (s_order[b].index > idx)))) {
            s_order[b + 1] = s_order[b];
            b--;
        }
        s_order[b + 1].index = idx;
        s_order[b + 1].measure = measure;
    }

    u16 count = 0;
    for (u16 k = 0; k < selected; k++) {
        count = (u16)(count + billboard_project_one(
                                                    &g_billboards[s_order[k].index],
                                                    &s_order[k].measure,
                                                    &objects[count]));
    }
    count = (u16)(count + billboard_project_effects(
        player, cos_a, sin_a, &objects[count], (u16)(max_objects - count)));

    // Effects and world objects share painter order so blood remains on its
    // target while nearer sprites can still occlude it.
    for (u16 a = 1; a < count; a++) {
        const ProjectedBillboard item = objects[a];
        s16 b = (s16)(a - 1);
        while (b >= 0 && objects[b].depth < item.depth) {
            objects[b + 1] = objects[b];
            b--;
        }
        objects[b + 1] = item;
    }
#if DEBUG_PERF
    s_debug_projected = count;
    renderer_perf_record_visible_subsectors(bsp_get_debug_visible_subsector_count(),
                                            s_debug_visible_subsector_objects,
                                            s_debug_safe_subsector_objects,
                                            s_debug_cullable_subsector_objects);
#endif

    return count;
}

#if DEBUG_PERF
u16 billboard_get_debug_culled_count(void) { return s_debug_culled; }
u16 billboard_get_debug_candidate_count(void) { return s_debug_candidates; }
u16 billboard_get_debug_occluded_count(void) { return s_debug_occluded; }
u16 billboard_get_debug_projected_count(void) { return s_debug_projected; }
u16 billboard_get_debug_projection_cache_hits(void) { return s_debug_cache_hits; }
u16 billboard_get_debug_projection_cache_misses(void) { return s_debug_cache_misses; }
#endif
