#include "billboard_internal.h"

typedef struct {
    u8 index;
    BillboardMeasure measure;
} BillboardProjectionOrder;

// Static rather than stack-resident: imported E1M1 content needs many more
// active objects than the draw budget while the Mega Drive stack stays small.
// Only the nearest drawable objects are retained, so dense rooms never pay for
// sorting every active map thing.
static BillboardProjectionOrder s_order[BILLBOARD_MAX_PROJECTED_OBJECTS];
#if DEBUG_PERF
static u16 s_debug_culled;
static u16 s_debug_candidates;
static u16 s_debug_occluded;
static u16 s_debug_los_culled;
static u16 s_debug_projected;
#endif

static u16 billboard_project_one(const PlayerState *player,
                                 const BillboardObject *object,
                                 const BillboardMeasure *measure,
                                 ProjectedBillboard *projected) {

    s16 half_h = (s16)(measure->half_w * 2);

    if (half_h > 28) {
        half_h = 28;
    }

    const s16 top = (s16)((RAY_VIEW_ROWS / 2) - half_h);
    const s16 bottom = (s16)((RAY_VIEW_ROWS / 2) + half_h);
    const s16 left = (s16)(measure->center_col - measure->half_w);
    const s16 right = (s16)(measure->center_col + measure->half_w);
    (void)player;
    projected->left = left;
    projected->right = right;
    projected->top = top;
    projected->bottom = bottom;
    projected->depth = (u16)measure->forward;
    projected->visual_id = billboard_get_object_visual_id(object, measure->type);
    projected->frame = billboard_get_object_frame(object);
    return 1;
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

#if DEBUG_PERF
    s_debug_culled = 0;
    s_debug_candidates = 0;
    s_debug_occluded = 0;
    s_debug_los_culled = 0;
    s_debug_projected = 0;
#endif

    // Compute the view basis once and share it across every object instead of
    // redoing fx_cos/fx_sin per object inside billboard_measure_object.
    const s16 cos_a = fx_cos(player->angle);
    const s16 sin_a = fx_sin(player->angle);

    if (budget == 0) {
        return 0;
    }

    for (u16 i = 0; i < BILLBOARD_OBJECT_COUNT; i++) {
        BillboardMeasure measure;

        if (!billboard_measure_object(player, cos_a, sin_a, &g_billboards[i], &measure)) {
            continue;
        }
        if ((measure.center_col + measure.half_w < 0) ||
            (measure.center_col - measure.half_w >= RAY_VIEW_COLS)) {
#if DEBUG_PERF
            s_debug_culled++;
#endif
            continue;
        }
#if DEBUG_PERF
        s_debug_candidates++;
#endif
        {
            s16 center = measure.center_col;
            if (center < 0) center = 0;
            else if (center >= RAY_VIEW_COLS) center = RAY_VIEW_COLS - 1;
            const u16 wall_col = (u16)(center & ~(RAY_COL_STRIDE - 1));
            const u16 next_wall_col = (wall_col + RAY_COL_STRIDE < RAY_VIEW_COLS) ?
                                      (u16)(wall_col + RAY_COL_STRIDE) : wall_col;
            const u16 wall_depth = (columns[next_wall_col].depth < columns[wall_col].depth) ?
                                   columns[next_wall_col].depth : columns[wall_col].depth;
            if (measure.forward >= wall_depth) {
#if DEBUG_PERF
                s_debug_occluded++;
#endif
                continue;
            }
        }
        if (!billboard_has_line_of_sight(i, player)) {
#if DEBUG_PERF
            s_debug_los_culled++;
#endif
            continue;
        }
        if (selected < budget) {
            s_order[selected].index = (u8)i;
            s_order[selected].measure = measure;
            selected++;
        } else {
            // Replace only the farthest retained object. At the cutoff, retain
            // the later source entries: that matches the old stable full sort
            // followed by its nearest-object suffix.
            u16 farthest = 0;
            for (u16 candidate = 1; candidate < selected; candidate++) {
                if ((s_order[candidate].measure.forward > s_order[farthest].measure.forward) ||
                    ((s_order[candidate].measure.forward == s_order[farthest].measure.forward) &&
                     (s_order[candidate].index < s_order[farthest].index))) {
                    farthest = candidate;
                }
            }
            if ((measure.forward < s_order[farthest].measure.forward) ||
                ((measure.forward == s_order[farthest].measure.forward) &&
                 (i > s_order[farthest].index))) {
                s_order[farthest].index = (u8)i;
                s_order[farthest].measure = measure;
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
        count = (u16)(count + billboard_project_one(player,
                                                    &g_billboards[s_order[k].index],
                                                    &s_order[k].measure,
                                                    &objects[count]));
    }
#if DEBUG_PERF
    s_debug_projected = count;
#endif

    return count;
}

#if DEBUG_PERF
u16 billboard_get_debug_culled_count(void) { return s_debug_culled; }
u16 billboard_get_debug_candidate_count(void) { return s_debug_candidates; }
u16 billboard_get_debug_occluded_count(void) { return s_debug_occluded; }
u16 billboard_get_debug_los_culled_count(void) { return s_debug_los_culled; }
u16 billboard_get_debug_projected_count(void) { return s_debug_projected; }
#endif
