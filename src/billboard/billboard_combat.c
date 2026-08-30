#include "billboard_internal.h"
#include "billboard_effects.h"
#include "billboard_explosion.h"
#include "bsp_map.h"

#define ENEMY_RADIUS 24
#define WALL_PUFF_DEPTH_BIAS 24

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

static BillboardFireResult explosion_fire_result(BarrelExplosionResult explosion) {
    BillboardFireResult result;
    result.status = BILLBOARD_SHOT_EXPLOSION;
    result.player_damage = explosion.player_damage;
    result.push_x = explosion.push_x;
    result.push_y = explosion.push_y;
    result.explosion_count = explosion.explosion_count;
    return result;
}

static void spawn_wall_puff(const PlayerState *player, u16 wall_depth) {
    // Pull the impact far enough in front of the hit plane to survive the
    // renderer's 4-column wall-depth blocks. A four-unit bias could round back
    // onto/behind the wall and make PUFF disappear completely.
    if (wall_depth <= 2 || wall_depth >= 0xFF00u) return;
    const s32 distance = (wall_depth > WALL_PUFF_DEPTH_BIAS) ?
        (wall_depth - WALL_PUFF_DEPTH_BIAS) : (wall_depth / 2);
    const s16 cos_a = fx_cos(player->angle);
    const s16 sin_a = fx_sin(player->angle);
    billboard_effects_spawn_puff(
        player->x + (((s32)cos_a * distance) >> FX_SHIFT),
        player->y + (((s32)sin_a * distance) >> FX_SHIFT));
}

// One hitscan pellet. `aim_col` is the view column it is aimed down (the screen
// centre for a single-pellet weapon, a fanned offset for each shotgun pellet)
// and `wall_depth` is the wall distance at THAT column, so an off-centre pellet
// is blocked by the geometry it actually points at. `damage` is the weapon's
// per-pellet damage in Doom hit points.
BillboardFireResult billboard_fire_center(const PlayerState *player, u16 wall_depth,
                                          s16 aim_col, u16 damage) {
    BillboardFireResult result = {BILLBOARD_SHOT_NONE, 0, 0, 0, 0};
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
        if (aim_col < (measure.center_col - measure.half_w) ||
            aim_col > (measure.center_col + measure.half_w)) {
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
            }
        }
        if (closest_barrel != NULL) {
            closest_barrel->life_state = ENEMY_DYING;
            closest_barrel->hp = 0;
            closest_barrel->death_index = 0;
            closest_barrel->death_timer = (u8)(BARREL_DEATH_FRAME_HOLDS[0] + 1);
            billboard_effects_spawn_puff(closest_barrel->x, closest_barrel->y);
            return explosion_fire_result(billboard_apply_explosion(
                player, closest_barrel->x, closest_barrel->y));
        }
        spawn_wall_puff(player, wall_depth);
        return result;
    }

    // Knock back BEFORE spawning the decal: push_dummy_on_hit can move a DUMMY
    // up to DUMMY_HIT_PUSH_STEP (64u) on each axis, and the blood/puff impact
    // is a static-position pooled effect that never tracks its target after
    // spawning (see spawn_impact in billboard_effects.c). Spawning first at
    // the pre-knockback position left the decal visibly stranded behind the
    // (now-shoved) body. push_dummy_on_hit no-ops for non-DUMMY types, so this
    // ordering is a no-op change for the barrel/puff path.
    push_dummy_on_hit(best_index, best_object, player);
    if (best_object->type_id == BILLBOARD_TYPE_DUMMY) {
        billboard_effects_spawn_blood(best_object->x, best_object->y);
    } else {
        billboard_effects_spawn_puff(best_object->x, best_object->y);
    }

    if (best_object->hp > damage) {
        best_object->hp = (u8)(best_object->hp - damage);
        result.status = BILLBOARD_SHOT_DAMAGE;
        return result;
    }

    // Enemies play a death animation and leave a corpse instead of vanishing;
    // barrels detonate immediately and apply AoE splash via billboard_apply_explosion.
    if (best_object->type_id == BILLBOARD_TYPE_DUMMY) {
        best_object->hp = 0;
        billboard_registry_enemy_died(best_index);
        best_object->death_index = 0;
        best_object->death_timer = ENEMY_DEATH_HOLD_TICS;
        result.status = BILLBOARD_SHOT_KILL;
        return result;
    }

    if (best_object->type_id == BILLBOARD_TYPE_BARREL) {
        // Flag the shot barrel as detonating and let billboard_update_barrels()
        // play the BEXP animation over the next few frames before pulling it
        // out of the registry. The AoE itself is applied immediately so chain
        // reactions and player damage land in one tick.
        best_object->life_state = ENEMY_DYING;
        best_object->hp = 0;
        best_object->death_index = 0;
        best_object->death_timer = (u8)(BARREL_DEATH_FRAME_HOLDS[0] + 1);
        return explosion_fire_result(billboard_apply_explosion(
            player, best_object->x, best_object->y));
    }

    billboard_registry_deactivate(best_index);
    best_object->hp = 0;
    result.status = BILLBOARD_SHOT_KILL;
    return result;
}
