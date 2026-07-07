#include "billboard_internal.h"

static u16 billboard_project_one(const PlayerState *player,
                                 const BillboardObject *object,
                                 BillboardSpan *spans,
                                 u16 max_spans) {
    BillboardMeasure measure;

    if (!billboard_measure_object(player, object, &measure)) {
        return 0;
    }

    s16 half_h = (s16)(measure.half_w * 2);

    if (half_h > 28) {
        half_h = 28;
    }

    const s16 top = (s16)((BILLBOARD_VIEW_PIXEL_H / 2) - half_h);
    const s16 bottom = (s16)((BILLBOARD_VIEW_PIXEL_H / 2) + half_h);
    const s16 left = (s16)(measure.center_col - measure.half_w);
    const s16 right = (s16)(measure.center_col + measure.half_w);
    const s16 width = (s16)(right - left + 1);
    u16 count = 0;

    for (s16 col = left; col <= right; col++) {
        if ((col < 0) || (col >= BILLBOARD_VIEW_COLS)) {
            continue;
        }
        if (count >= max_spans) {
            break;
        }

        spans[count].column = col;
        spans[count].top = top;
        spans[count].bottom = bottom;
        spans[count].depth = (u16)measure.forward;
        // Store x as a 0-255 fraction of the sprite width so the renderer can scale
        // it to any texture size (16-wide items, 24-wide enemy) without projection
        // needing to know sprite dimensions.
        {
            s16 frac = (s16)(((col - left) * 256) / width);
            if (frac > 255) {
                frac = 255;
            }
            spans[count].tex_x = (u8)frac;
        }
        spans[count].visual_id = billboard_get_object_visual_id(object, measure.type);
        spans[count].frame = billboard_get_object_frame(object);
        count++;
    }

    return count;
}

u16 billboard_project_scene(const PlayerState *player, BillboardSpan *spans, u16 max_spans) {
    // Depth ordering is a per-object property, so sort the (<= 7) visible objects
    // once here instead of bubble-sorting the ~150 per-column spans they expand
    // into (the old O(spans^2) pass). Emitting objects far -> near means the nearer
    // object's span is written last and overwrites in any shared column — the same
    // visual result the span sort produced.
    struct {
        u8 index;
        s32 forward;
    } order[BILLBOARD_OBJECT_COUNT];
    u16 visible = 0;

    for (u16 i = 0; i < BILLBOARD_OBJECT_COUNT; i++) {
        BillboardMeasure measure;

        if (!billboard_measure_object(player, &g_billboards[i], &measure)) {
            continue;
        }
        order[visible].index = (u8)i;
        order[visible].forward = measure.forward;
        visible++;
    }

    // Insertion sort, far (large forward) -> near (small forward). n <= 7.
    for (u16 a = 1; a < visible; a++) {
        const u8 idx = order[a].index;
        const s32 fwd = order[a].forward;
        s16 b = (s16)(a - 1);

        while ((b >= 0) && (order[b].forward < fwd)) {
            order[b + 1] = order[b];
            b--;
        }
        order[b + 1].index = idx;
        order[b + 1].forward = fwd;
    }

    u16 count = 0;
    for (u16 k = 0; k < visible; k++) {
        if (count >= max_spans) {
            break;
        }
        count = (u16)(count + billboard_project_one(player, &g_billboards[order[k].index], &spans[count],
                                                    (u16)(max_spans - count)));
    }

    return count;
}
