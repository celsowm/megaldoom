#include "billboard_internal.h"

static u16 billboard_project_one(const PlayerState *player,
                                 const BillboardObject *object,
                                 const BillboardMeasure *measure,
                                 ProjectedBillboard *projected) {

    s16 half_h = (s16)(measure->half_w * 2);

    if (half_h > 28) {
        half_h = 28;
    }

    const s16 top = (s16)((BILLBOARD_VIEW_PIXEL_H / 2) - half_h);
    const s16 bottom = (s16)((BILLBOARD_VIEW_PIXEL_H / 2) + half_h);
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
    struct {
        u8 index;
        BillboardMeasure measure;
    } order[BILLBOARD_OBJECT_COUNT];
    u16 visible = 0;

    for (u16 i = 0; i < BILLBOARD_OBJECT_COUNT; i++) {
        BillboardMeasure measure;

        if (!billboard_measure_object(player, &g_billboards[i], &measure)) {
            continue;
        }
        order[visible].index = (u8)i;
        order[visible].measure = measure;
        visible++;
    }

    // Insertion sort, far (large forward) -> near (small forward). n <= 7.
    for (u16 a = 1; a < visible; a++) {
        const u8 idx = order[a].index;
        const BillboardMeasure measure = order[a].measure;
        s16 b = (s16)(a - 1);

        while ((b >= 0) && (order[b].measure.forward < measure.forward)) {
            order[b + 1] = order[b];
            b--;
        }
        order[b + 1].index = idx;
        order[b + 1].measure = measure;
    }

    u16 count = 0;
    for (u16 k = 0; k < visible; k++) {
        if (count >= max_objects) {
            break;
        }
        count = (u16)(count + billboard_project_one(player,
                                                    &g_billboards[order[k].index],
                                                    &order[k].measure,
                                                    &objects[count]));
    }

    return count;
}
