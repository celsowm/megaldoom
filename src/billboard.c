#include "billboard_internal.h"

static const BillboardType BILLBOARD_TYPES[] = {
    {BILLBOARD_VISUAL_BONUS, BILLBOARD_VISUAL_BONUS, 1, BILLBOARD_SCALE, BILLBOARD_MAX_DEPTH, TRUE, FALSE},
    {BILLBOARD_VISUAL_KEY, BILLBOARD_VISUAL_KEY, 1, BILLBOARD_SCALE, BILLBOARD_MAX_DEPTH, TRUE, FALSE},
    {BILLBOARD_VISUAL_DECOR, BILLBOARD_VISUAL_DECOR_DAMAGED, 2, BILLBOARD_SCALE, BILLBOARD_MAX_DEPTH, FALSE, TRUE},
    {BILLBOARD_VISUAL_DUMMY, BILLBOARD_VISUAL_DUMMY_DAMAGED, 3, BILLBOARD_SCALE, BILLBOARD_MAX_DEPTH, FALSE, TRUE},
};

const BillboardObject INITIAL_BILLBOARDS[BILLBOARD_PHASE_COUNT][BILLBOARD_OBJECT_COUNT] = {
    {
        {5 * FX_ONE + (FX_ONE / 2), 2 * FX_ONE + (FX_ONE / 2), 0, 1, TRUE, 0, 0, 0, 0, 0, FALSE, FALSE, 0, 0, 0, FALSE},
        {2 * FX_ONE + (FX_ONE / 2), 5 * FX_ONE + (FX_ONE / 2), 1, 1, TRUE, 0, 0, 0, 0, 0, FALSE, FALSE, 0, 0, 0, FALSE},
        {6 * FX_ONE + (FX_ONE / 2), 6 * FX_ONE + (FX_ONE / 2), 0, 1, TRUE, 0, 0, 0, 0, 0, FALSE, FALSE, 0, 0, 0, FALSE},
        {4 * FX_ONE + (FX_ONE / 2), 5 * FX_ONE + (FX_ONE / 2), 2, 2, TRUE, 0, 0, 0, 0, 0, FALSE, FALSE, 0, 0, 0, FALSE},
        {6 * FX_ONE + (FX_ONE / 2), 4 * FX_ONE + (FX_ONE / 2), 2, 2, TRUE, 0, 0, 0, 0, 0, FALSE, FALSE, 0, 0, 0, FALSE},
        {3 * FX_ONE + (FX_ONE / 2), 6 * FX_ONE + (FX_ONE / 2), 3, 3, TRUE, 0, 0, 0, 0, 0, FALSE, FALSE, 0, 0, 0, FALSE},
        {5 * FX_ONE + (FX_ONE / 2), 3 * FX_ONE + (FX_ONE / 2), 3, 3, TRUE, 0, 0, 0, 0, 0, FALSE, FALSE, 0, 0, 0, FALSE},
    },
    {
        {1 * FX_ONE + (FX_ONE / 2), 6 * FX_ONE + (FX_ONE / 2), 0, 1, TRUE, 0, 0, 0, 0, 0, FALSE, FALSE, 0, 0, 0, FALSE},
        {5 * FX_ONE + (FX_ONE / 2), 5 * FX_ONE + (FX_ONE / 2), 1, 1, TRUE, 0, 0, 0, 0, 0, FALSE, FALSE, 0, 0, 0, FALSE},
        {6 * FX_ONE + (FX_ONE / 2), 2 * FX_ONE + (FX_ONE / 2), 0, 1, TRUE, 0, 0, 0, 0, 0, FALSE, FALSE, 0, 0, 0, FALSE},
        {2 * FX_ONE + (FX_ONE / 2), 2 * FX_ONE + (FX_ONE / 2), 2, 2, TRUE, 0, 0, 0, 0, 0, FALSE, FALSE, 0, 0, 0, FALSE},
        {4 * FX_ONE + (FX_ONE / 2), 6 * FX_ONE + (FX_ONE / 2), 2, 2, TRUE, 0, 0, 0, 0, 0, FALSE, FALSE, 0, 0, 0, FALSE},
        {6 * FX_ONE + (FX_ONE / 2), 3 * FX_ONE + (FX_ONE / 2), 3, 3, TRUE, 0, 0, 0, 0, 0, FALSE, FALSE, 0, 0, 0, FALSE},
        {3 * FX_ONE + (FX_ONE / 2), 4 * FX_ONE + (FX_ONE / 2), 3, 3, TRUE, 0, 0, 0, 0, 0, FALSE, FALSE, 0, 0, 0, FALSE},
    }
};

BillboardObject g_billboards[BILLBOARD_OBJECT_COUNT];
u16 g_collected_count;
BillboardPickupCounts g_pickup_counts;
BillboardPickupKind g_last_pickup_kind;

const BillboardType *billboard_get_type(u8 type_id) {
    if (type_id >= (sizeof(BILLBOARD_TYPES) / sizeof(BILLBOARD_TYPES[0]))) {
        return &BILLBOARD_TYPES[0];
    }

    return &BILLBOARD_TYPES[type_id];
}

u8 billboard_get_object_visual_id(const BillboardObject *object, const BillboardType *type) {
    if (object->hp < type->hit_points) {
        return type->damaged_visual_id;
    }

    return type->visual_id;
}

bool billboard_measure_object(const PlayerState *player, const BillboardObject *object, BillboardMeasure *measure) {
    if (!object->active) {
        return FALSE;
    }

    const BillboardType *type = billboard_get_type(object->type_id);
    const s32 dx = object->x - player->x;
    const s32 dy = object->y - player->y;
    const s16 cos_a = fx_cos(player->angle);
    const s16 sin_a = fx_sin(player->angle);
    const s32 forward = (((s32)cos_a * dx) + ((s32)sin_a * dy)) >> FX_SHIFT;
    const s32 side = (((s32)cos_a * dy) - ((s32)sin_a * dx)) >> FX_SHIFT;

    if ((forward <= BILLBOARD_MIN_DEPTH) || (forward >= type->max_depth)) {
        return FALSE;
    }

    measure->type = type;
    measure->forward = forward;
    measure->side = side;
    measure->center_col = (s16)((BILLBOARD_VIEW_COLS / 2) + ((side * 24) / forward));
    measure->half_w = (s16)(type->scale / forward);

    if (measure->half_w > 3) {
        measure->half_w = 3;
    }

    return TRUE;
}

void billboard_init(u16 phase_index) {
    const u16 phase = (u16)(phase_index % BILLBOARD_PHASE_COUNT);

    for (u16 i = 0; i < BILLBOARD_OBJECT_COUNT; i++) {
        g_billboards[i] = INITIAL_BILLBOARDS[phase][i];
        g_billboards[i].hp = billboard_get_type(g_billboards[i].type_id)->hit_points;
        g_billboards[i].home_x = g_billboards[i].x;
        g_billboards[i].home_y = g_billboards[i].y;
        g_billboards[i].patrol_dir = 1;
        g_billboards[i].patrol_axis_x = (bool)((i & 1) == 0);
        g_billboards[i].saw_player = FALSE;
        g_billboards[i].spot_cooldown = 0;
        g_billboards[i].last_seen_x = g_billboards[i].x;
        g_billboards[i].last_seen_y = g_billboards[i].y;
        g_billboards[i].has_last_seen = FALSE;
    }

    g_collected_count = 0;
    g_pickup_counts.bonus = 0;
    g_pickup_counts.key = 0;
    g_last_pickup_kind = BILLBOARD_PICKUP_NONE;
}

bool billboard_collect_near(s32 x, s32 y) {
    for (u16 i = 0; i < BILLBOARD_OBJECT_COUNT; i++) {
        BillboardObject *object = &g_billboards[i];

        if (!object->active) {
            continue;
        }

        const BillboardType *type = billboard_get_type(object->type_id);
        if (!type->collectible) {
            continue;
        }

        const s32 dx = object->x - x;
        const s32 dy = object->y - y;
        const s32 dist_sq = (dx * dx) + (dy * dy);

        if (dist_sq <= BILLBOARD_COLLECT_RADIUS_SQ) {
            object->active = FALSE;
            g_collected_count++;
            if (type->visual_id == BILLBOARD_VISUAL_KEY) {
                g_pickup_counts.key++;
                g_last_pickup_kind = BILLBOARD_PICKUP_KEY;
            } else if (type->visual_id == BILLBOARD_VISUAL_BONUS) {
                g_pickup_counts.bonus++;
                g_last_pickup_kind = BILLBOARD_PICKUP_BONUS;
            }
            return TRUE;
        }
    }

    return FALSE;
}

bool billboard_consume_key(void) {
    if (g_pickup_counts.key == 0) {
        return FALSE;
    }

    g_pickup_counts.key--;
    return TRUE;
}

u16 billboard_get_collected_count(void) {
    return g_collected_count;
}

BillboardPickupCounts billboard_get_pickup_counts(void) {
    return g_pickup_counts;
}

BillboardPickupKind billboard_get_last_pickup_kind(void) {
    return g_last_pickup_kind;
}

u16 billboard_get_enemy_count(void) {
    u16 count = 0;

    for (u16 i = 0; i < BILLBOARD_OBJECT_COUNT; i++) {
        const BillboardObject *object = &g_billboards[i];

        if (object->active && (object->type_id == BILLBOARD_TYPE_DUMMY)) {
            count++;
        }
    }

    return count;
}
