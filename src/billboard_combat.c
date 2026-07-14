#include "billboard_internal.h"
#include "billboard_explosion.h"
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

    const u8 *indices = billboard_registry_target_indices();
    const u16 target_count = billboard_registry_target_count();
    for (u16 slot = 0; slot < target_count; slot++) {
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
        // A barrel that is already detonating is removed from the active list
        // by billboard_apply_explosion; this guard is defensive in case the
        // AoE worklist visits the original shot barrel during the same call.
        if ((object->type_id == BILLBOARD_TYPE_BARREL) && (object->life_state != ENEMY_ALIVE)) {
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

    // Point-blank barrel rescue: billboard_measure_object rejects anything
    // within BILLBOARD_MIN_DEPTH (32u) of the camera, so a barrel you are
    // literally pressed against can never be aim-acquired by the loop above.
    // Doom's pistol still detonates such barrels (the player ends up taking
    // splash damage), so fall back to a small Euclidean check around the
    // player and detonate the closest live barrel found. Keep this AFTER the
    // aim scan so aimed hits always win over proximity when both apply.
    if (best_object == NULL) {
        s32 closest_dist_sq = BILLBOARD_POINT_BLANK_RADIUS_SQ;
        BillboardObject *closest_barrel = NULL;
        u16 closest_index = 0;
        for (u16 slot = 0; slot < target_count; slot++) {
            const u16 i = indices[slot];
            BillboardObject *object = &g_billboards[i];
            if (!object->active || (object->type_id != BILLBOARD_TYPE_BARREL)) {
                continue;
            }
            if (object->life_state != ENEMY_ALIVE) {
                continue;
            }
            const s32 dx = object->x - player->x;
            const s32 dy = object->y - player->y;
            const s32 dist_sq = (dx * dx) + (dy * dy);
            if (dist_sq < closest_dist_sq) {
                closest_dist_sq = dist_sq;
                closest_barrel = object;
                closest_index = i;
            }
        }
        if (closest_barrel != NULL) {
            closest_barrel->life_state = ENEMY_DYING;
            closest_barrel->hp = 0;
            closest_barrel->death_index = 0;
            closest_barrel->death_timer = BARREL_DEATH_HOLD;
            billboard_apply_explosion(player, closest_barrel->x, closest_barrel->y);
            return BILLBOARD_SHOT_EXPLOSION;
        }
        return BILLBOARD_SHOT_NONE;
    }

    push_dummy_on_hit(best_index, best_object, player);

    if (best_object->hp > 1) {
        best_object->hp--;
        return BILLBOARD_SHOT_DAMAGE;
    }

    // Enemies play a death animation and leave a corpse instead of vanishing;
    // barrels detonate immediately and apply AoE splash via billboard_apply_explosion.
    if (best_object->type_id == BILLBOARD_TYPE_DUMMY) {
        best_object->hp = 0;
        billboard_registry_enemy_died(best_index);
        best_object->death_index = 0;
        best_object->death_timer = ENEMY_DEATH_HOLD;
        return BILLBOARD_SHOT_KILL;
    }

    if (best_object->type_id == BILLBOARD_TYPE_BARREL) {
        // Flag the shot barrel as detonating and let billboard_update_barrels()
        // play the BEXP animation over the next few frames before pulling it
        // out of the registry. The AoE itself is applied immediately so chain
        // reactions and player damage land in one tick.
        best_object->life_state = ENEMY_DYING;
        best_object->hp = 0;
        best_object->death_index = 0;
        best_object->death_timer = BARREL_DEATH_HOLD;
        billboard_apply_explosion(player, best_object->x, best_object->y);
        return BILLBOARD_SHOT_EXPLOSION;
    }

    billboard_registry_deactivate(best_index);
    best_object->hp = 0;
    return BILLBOARD_SHOT_KILL;
}
