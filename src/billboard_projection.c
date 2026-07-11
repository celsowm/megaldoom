#include "billboard_internal.h"

typedef struct {
    u8 index;
    BillboardMeasure measure;
} BillboardProjectionOrder;

// Static rather than stack-resident: imported E1M1 content needs many more
// active objects than the original seven while the Mega Drive stack stays small.
static BillboardProjectionOrder s_order[BILLBOARD_OBJECT_COUNT];
#if DEBUG_PERF
static u16 s_debug_culled;
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
                            ProjectedBillboard *objects,
                            u16 max_objects) {
    // Depth ordering is a per-object property, so sort the (<= 7) visible objects
    // once here instead of bubble-sorting the ~150 per-column spans they expand
    // into (the old O(spans^2) pass). Emitting objects far -> near means the nearer
    // object's span is written last and overwrites in any shared column — the same
    // visual result the span sort produced.
    u16 visible = 0;

#if DEBUG_PERF
    s_debug_culled = 0;
    s_debug_projected = 0;
#endif

    // Compute the view basis once and share it across every object instead of
    // redoing fx_cos/fx_sin per object inside billboard_measure_object.
    const s16 cos_a = fx_cos(player->angle);
    const s16 sin_a = fx_sin(player->angle);

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
        s_order[visible].index = (u8)i;
        s_order[visible].measure = measure;
        visible++;
    }

    // Insertion sort, far (large forward) -> near (small forward). n <= 7.
    for (u16 a = 1; a < visible; a++) {
        const u8 idx = s_order[a].index;
        const BillboardMeasure measure = s_order[a].measure;
        s16 b = (s16)(a - 1);

        while ((b >= 0) && (s_order[b].measure.forward < measure.forward)) {
            s_order[b + 1] = s_order[b];
            b--;
        }
        s_order[b + 1].index = idx;
        s_order[b + 1].measure = measure;
    }

    u16 count = 0;
    // The sort is far -> near. When a dense room exceeds the draw budget, keep
    // the nearest objects (the ones that can materially affect the frame) while
    // preserving far-to-near painter order inside that selected suffix.
    const u16 first = (visible > max_objects) ? (u16)(visible - max_objects) : 0;
    for (u16 k = first; k < visible; k++) {
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
u16 billboard_get_debug_projected_count(void) { return s_debug_projected; }
#endif
