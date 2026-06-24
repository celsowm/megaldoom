#include "billboard_internal.h"
#include "world_map.h"

static bool is_position_blocked(s32 x, s32 y) {
    const s16 cell_x = (s16)(x / WORLD_CELL_SIZE);
    const s16 cell_y = (s16)(y / WORLD_CELL_SIZE);

    return world_map_is_solid(cell_x, cell_y);
}

static void push_dummy_on_hit(BillboardObject *object, const PlayerState *player) {
    s16 step_x = 0;
    s16 step_y = 0;

    if (object->type_id != BILLBOARD_TYPE_DUMMY) {
        return;
    }

    if (object->x > player->x) {
        step_x = DUMMY_HIT_PUSH_STEP;
    } else if (object->x < player->x) {
        step_x = -DUMMY_HIT_PUSH_STEP;
    }

    if (object->y > player->y) {
        step_y = DUMMY_HIT_PUSH_STEP;
    } else if (object->y < player->y) {
        step_y = -DUMMY_HIT_PUSH_STEP;
    }

    if ((step_x != 0) && !is_position_blocked(object->x + step_x, object->y)) {
        object->x += step_x;
    }
    if ((step_y != 0) && !is_position_blocked(object->x, object->y + step_y)) {
        object->y += step_y;
    }

    object->move_cooldown = DUMMY_HIT_STUN_FRAMES;
}

u16 billboard_get_target_count(void) {
    u16 count = 0;

    for (u16 i = 0; i < BILLBOARD_OBJECT_COUNT; i++) {
        const BillboardObject *object = &g_billboards[i];
        const BillboardType *type = billboard_get_type(object->type_id);

        if (object->active && type->targetable) {
            count++;
        }
    }

    return count;
}

u16 billboard_get_target_health(void) {
    for (u16 i = 0; i < BILLBOARD_OBJECT_COUNT; i++) {
        const BillboardObject *object = &g_billboards[i];
        const BillboardType *type = billboard_get_type(object->type_id);

        if (object->active && type->targetable) {
            return object->hp;
        }
    }

    return 0;
}

BillboardShotResult billboard_fire_center(const PlayerState *player, u16 wall_depth) {
    BillboardObject *best_object = NULL;
    s32 best_depth = 0x7FFFFFFF;

    for (u16 i = 0; i < BILLBOARD_OBJECT_COUNT; i++) {
        BillboardObject *object = &g_billboards[i];
        BillboardMeasure measure;

        if (!billboard_measure_object(player, object, &measure)) {
            continue;
        }
        if (!measure.type->targetable) {
            continue;
        }
        if (measure.forward >= wall_depth) {
            continue;
        }
        if ((BILLBOARD_VIEW_COLS / 2) < (measure.center_col - measure.half_w) ||
            (BILLBOARD_VIEW_COLS / 2) > (measure.center_col + measure.half_w)) {
            continue;
        }

        if (measure.forward < best_depth) {
            best_depth = measure.forward;
            best_object = object;
        }
    }

    if (best_object == NULL) {
        return BILLBOARD_SHOT_NONE;
    }

    push_dummy_on_hit(best_object, player);

    if (best_object->hp > 1) {
        best_object->hp--;
        return BILLBOARD_SHOT_DAMAGE;
    }

    best_object->active = FALSE;
    best_object->hp = 0;
    return BILLBOARD_SHOT_KILL;
}
