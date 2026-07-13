#include "billboard_internal.h"
#include "bsp_map.h"

#define ENEMY_RADIUS 24

static bool is_position_blocked(s32 x, s32 y) {
#if DEBUG_PERF
    bsp_debug_set_query_owner(BSP_QUERY_ENEMY);
#endif
    return bsp_circle_blocked(x, y, ENEMY_RADIUS) ||
           billboard_position_blocked(x, y, ENEMY_RADIUS);
}

static void push_dummy_on_hit(u16 index, BillboardObject *object, const PlayerState *player) {
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

    billboard_invalidate_object_visibility(index);

    object->move_cooldown = DUMMY_HIT_STUN_FRAMES;
}

u16 billboard_get_target_count(void) {
    return billboard_registry_living_enemy_count();
}

u16 billboard_get_target_health(void) {
    const u8 *indices = billboard_registry_enemy_indices();
    const u16 enemy_count = billboard_registry_enemy_count();
    for (u16 slot = 0; slot < enemy_count; slot++) {
        const u16 i = indices[slot];
        const BillboardObject *object = &g_billboards[i];
        const BillboardType *type = billboard_get_type(object->type_id);

        if (object->active && type->targetable && (object->life_state == ENEMY_ALIVE)) {
            return object->hp;
        }
    }

    return 0;
}

BillboardShotResult billboard_fire_center(const PlayerState *player, u16 wall_depth) {
    BillboardObject *best_object = NULL;
    u16 best_index = 0;
    s32 best_depth = 0x7FFFFFFF;

    // Compute the view basis once and share it across the target scan, matching
    // billboard_project_scene (avoids per-object fx_cos/fx_sin lookups).
    const s16 cos_a = fx_cos(player->angle);
    const s16 sin_a = fx_sin(player->angle);

    const u8 *indices = billboard_registry_enemy_indices();
    const u16 enemy_count = billboard_registry_enemy_count();
    for (u16 slot = 0; slot < enemy_count; slot++) {
        const u16 i = indices[slot];
        BillboardObject *object = &g_billboards[i];
        BillboardMeasure measure;

        if (!billboard_measure_object(player, cos_a, sin_a, object, &measure)) {
            continue;
        }
        if (!measure.type->targetable) {
            continue;
        }
        // A dying/dead enemy is a corpse: not a valid target anymore.
        if ((object->type_id == BILLBOARD_TYPE_DUMMY) && (object->life_state != ENEMY_ALIVE)) {
            continue;
        }
        if (measure.forward >= wall_depth) {
            continue;
        }
        if (RAY_VIEW_CENTER_X < (measure.center_col - measure.half_w) ||
            RAY_VIEW_CENTER_X > (measure.center_col + measure.half_w)) {
            continue;
        }

        if (measure.forward < best_depth) {
            best_depth = measure.forward;
            best_object = object;
            best_index = i;
        }
    }

    if (best_object == NULL) {
        return BILLBOARD_SHOT_NONE;
    }

    push_dummy_on_hit(best_index, best_object, player);

    if (best_object->hp > 1) {
        best_object->hp--;
        return BILLBOARD_SHOT_DAMAGE;
    }

    // Enemies play a death animation and leave a corpse instead of vanishing;
    // other targetable billboards (decor) still just deactivate on kill.
    if (best_object->type_id == BILLBOARD_TYPE_DUMMY) {
        best_object->hp = 0;
        billboard_registry_enemy_died(best_index);
        best_object->death_index = 0;
        best_object->death_timer = ENEMY_DEATH_HOLD;
        return BILLBOARD_SHOT_KILL;
    }

    billboard_registry_deactivate(best_index);
    best_object->hp = 0;
    return BILLBOARD_SHOT_KILL;
}
