#include "billboard.h"
#include "fixed_math.h"

#define BILLBOARD_MIN_DEPTH 32
#define BILLBOARD_MAX_DEPTH (6 * FX_ONE)
#define BILLBOARD_SCALE (3 * FX_ONE)
#define VIEW_COLS 32
#define VIEW_TILE_H 12
#define BILLBOARD_OBJECT_COUNT (sizeof(INITIAL_BILLBOARDS) / sizeof(INITIAL_BILLBOARDS[0]))
#define BILLBOARD_COLLECT_RADIUS (FX_ONE / 2)
#define BILLBOARD_COLLECT_RADIUS_SQ (BILLBOARD_COLLECT_RADIUS * BILLBOARD_COLLECT_RADIUS)

typedef struct {
    u8 visual_id;
    s32 scale;
    s32 max_depth;
    bool collectible;
} BillboardType;

typedef struct {
    s32 x;
    s32 y;
    u8 type_id;
    bool active;
} BillboardObject;

static const BillboardType BILLBOARD_TYPES[] = {
    {BILLBOARD_VISUAL_BONUS, BILLBOARD_SCALE, BILLBOARD_MAX_DEPTH, TRUE},
    {BILLBOARD_VISUAL_KEY, BILLBOARD_SCALE, BILLBOARD_MAX_DEPTH, TRUE},
    {BILLBOARD_VISUAL_DECOR, BILLBOARD_SCALE, BILLBOARD_MAX_DEPTH, FALSE},
};

static const BillboardObject INITIAL_BILLBOARDS[] = {
    {5 * FX_ONE + (FX_ONE / 2), 2 * FX_ONE + (FX_ONE / 2), 0, TRUE},
    {2 * FX_ONE + (FX_ONE / 2), 5 * FX_ONE + (FX_ONE / 2), 1, TRUE},
    {6 * FX_ONE + (FX_ONE / 2), 6 * FX_ONE + (FX_ONE / 2), 0, TRUE},
    {4 * FX_ONE + (FX_ONE / 2), 5 * FX_ONE + (FX_ONE / 2), 2, TRUE},
};

static BillboardObject g_billboards[BILLBOARD_OBJECT_COUNT];
static u16 g_collected_count;

static const BillboardType *get_billboard_type(u8 type_id) {
    if (type_id >= (sizeof(BILLBOARD_TYPES) / sizeof(BILLBOARD_TYPES[0]))) {
        return &BILLBOARD_TYPES[0];
    }

    return &BILLBOARD_TYPES[type_id];
}

static u16 billboard_project_one(const PlayerState *player,
                                 const BillboardObject *object,
                                 BillboardSpan *spans,
                                 u16 max_spans) {
    if (!object->active) {
        return 0;
    }

    const BillboardType *type = get_billboard_type(object->type_id);
    const s32 dx = object->x - player->x;
    const s32 dy = object->y - player->y;
    const s16 cos_a = fx_cos(player->angle);
    const s16 sin_a = fx_sin(player->angle);
    const s32 forward = (((s32)cos_a * dx) + ((s32)sin_a * dy)) >> FX_SHIFT;
    const s32 side = (((s32)cos_a * dy) - ((s32)sin_a * dx)) >> FX_SHIFT;

    if ((forward <= BILLBOARD_MIN_DEPTH) || (forward >= type->max_depth)) {
        return 0;
    }

    const s16 center_col = (s16)((VIEW_COLS / 2) + ((side * 24) / forward));
    s16 half_w = (s16)(type->scale / forward);

    if (half_w > 3) {
        half_w = 3;
    }

    const s16 top = (s16)((VIEW_TILE_H / 2) - half_w);
    const s16 bottom = (s16)((VIEW_TILE_H / 2) + half_w);
    const s16 left = (s16)(center_col - half_w);
    const s16 right = (s16)(center_col + half_w);
    const s16 width = (s16)(right - left + 1);
    u16 count = 0;

    for (s16 col = left; col <= right; col++) {
        if ((col < 0) || (col >= VIEW_COLS)) {
            continue;
        }
        if (count >= max_spans) {
            break;
        }

        spans[count].column = col;
        spans[count].top = top;
        spans[count].bottom = bottom;
        spans[count].depth = (u16)forward;
        spans[count].tex_x = (u8)(((col - left) * 8) / width);
        spans[count].visual_id = type->visual_id;
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

void billboard_init(void) {
    for (u16 i = 0; i < BILLBOARD_OBJECT_COUNT; i++) {
        g_billboards[i] = INITIAL_BILLBOARDS[i];
    }

    g_collected_count = 0;
}

bool billboard_collect_near(s32 x, s32 y) {
    for (u16 i = 0; i < BILLBOARD_OBJECT_COUNT; i++) {
        BillboardObject *object = &g_billboards[i];

        if (!object->active) {
            continue;
        }

        const BillboardType *type = get_billboard_type(object->type_id);
        if (!type->collectible) {
            continue;
        }

        const s32 dx = object->x - x;
        const s32 dy = object->y - y;
        const s32 dist_sq = (dx * dx) + (dy * dy);

        if (dist_sq <= BILLBOARD_COLLECT_RADIUS_SQ) {
            object->active = FALSE;
            g_collected_count++;
            return TRUE;
        }
    }

    return FALSE;
}

u16 billboard_get_collected_count(void) {
    return g_collected_count;
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
