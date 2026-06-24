#include "billboard_internal.h"

static u16 billboard_project_one(const PlayerState *player,
                                 const BillboardObject *object,
                                 BillboardSpan *spans,
                                 u16 max_spans) {
    BillboardMeasure measure;

    if (!billboard_measure_object(player, object, &measure)) {
        return 0;
    }

    const s16 top = (s16)((BILLBOARD_VIEW_TILE_H / 2) - measure.half_w);
    const s16 bottom = (s16)((BILLBOARD_VIEW_TILE_H / 2) + measure.half_w);
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
        spans[count].tex_x = (u8)(((col - left) * 8) / width);
        spans[count].visual_id = billboard_get_object_visual_id(object, measure.type);
        count++;
    }

    return count;
}

static void sort_spans_far_to_near(BillboardSpan *spans, u16 count) {
    if (count < 2) {
        return;
    }

    for (u16 pass = 0; pass < (count - 1); pass++) {
        for (u16 i = 0; i < (count - 1 - pass); i++) {
            if (spans[i].depth < spans[i + 1].depth) {
                const BillboardSpan tmp = spans[i];
                spans[i] = spans[i + 1];
                spans[i + 1] = tmp;
            }
        }
    }
}

u16 billboard_project_scene(const PlayerState *player, BillboardSpan *spans, u16 max_spans) {
    u16 count = 0;

    for (u16 i = 0; i < BILLBOARD_OBJECT_COUNT; i++) {
        if (count >= max_spans) {
            break;
        }

        count = (u16)(count + billboard_project_one(player, &g_billboards[i], &spans[count], (u16)(max_spans - count)));
    }

    sort_spans_far_to_near(spans, count);

    return count;
}
