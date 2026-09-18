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
    const u16 *indices = billboard_registry_enemy_indices();
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

static BillboardFireResult explosion_fire_result(BarrelExplosionResult explosion,
                                                 const BillboardObject *barrel) {
    BillboardFireResult result = {BILLBOARD_SHOT_NONE, 0, 0, 0, 0, TRUE,
                                  barrel->x, barrel->y, FALSE};
    result.status = BILLBOARD_SHOT_EXPLOSION;
    result.player_damage = explosion.player_damage;
    result.push_x = explosion.push_x;
    result.push_y = explosion.push_y;
    result.explosion_count = explosion.explosion_count;
    return result;
}

// Doom's thing radii for the hitscan cross-section (info.c): 20 for the
// zombieman, the shotgun guy and the imp, 10 for a barrel. These are NOT the
// BillboardType radii, which are this engine's movement-collision tuning; a
// trace is tested against Doom's own box, never against the drawn sprite.
#define DOOM_SHOOT_RADIUS_MONSTER 20
#define DOOM_SHOOT_RADIUS_BARREL 10
// The view basis carries a 1.1839 gain (fixed_math.h), so view depths are
// world lengths times 303/256. A melee reach in world units is converted once.
#define WORLD_TO_VIEW_DEPTH(units) ((u16)(((u32)(units) * 303u) >> 8))
#define NO_WALL_DEPTH 0xFF00u

static void spawn_wall_puff(const PlayerState *player, s16 dir_x, s16 dir_y,
                            s32 dir_depth, u16 wall_depth) {
    // Pull the impact far enough in front of the hit plane to survive the
    // renderer's 4-column wall-depth blocks. A four-unit bias could round back
    // onto/behind the wall and make PUFF disappear completely.
    if (wall_depth <= 2 || wall_depth >= NO_WALL_DEPTH) return;
    const s32 depth = (wall_depth > WALL_PUFF_DEPTH_BIAS) ?
        (wall_depth - WALL_PUFF_DEPTH_BIAS) : (wall_depth / 2);
    // The point at view depth `depth` along the trace is t * dir with
    // t = depth * 256 / (f . dir) (see billboard_fire_hitscan).
    billboard_effects_spawn_puff(
        player->x + ((s32)dir_x * depth) / dir_depth,
        player->y + ((s32)dir_y * depth) / dir_depth);
}

u16 billboard_angle_to(const PlayerState *player, s32 x, s32 y) {
    // Maximise the dot product with the heading: 16 coarse headings, then
    // halve the step down to one angle unit. 24 evaluations, melee hits only.
    const s32 dx = x - player->x;
    const s32 dy = y - player->y;
    u16 best = 0;
    s32 best_dot = -0x7FFFFFFF;
    for (u16 a = 0; a < ANGLE_STEPS; a += ANGLE_STEPS / 16) {
        const s32 dot = (s32)fx_cos(a) * dx + (s32)fx_sin(a) * dy;
        if (dot > best_dot) {
            best_dot = dot;
            best = a;
        }
    }
    for (u16 step = ANGLE_STEPS / 32; step > 0; step >>= 1) {
        const u16 left = (u16)((best - step) & ANGLE_MASK);
        const u16 right = (u16)((best + step) & ANGLE_MASK);
        const s32 dot_l = (s32)fx_cos(left) * dx + (s32)fx_sin(left) * dy;
        const s32 dot_r = (s32)fx_cos(right) * dx + (s32)fx_sin(right) * dy;
        if (dot_l > best_dot) {
            best_dot = dot_l;
            best = left;
        }
        if (dot_r > best_dot) {
            best_dot = dot_r;
            best = right;
        }
    }
    return best;
}

// Doom's P_LineAttack against things (p_maputl.c PIT_AddThingIntercepts): a
// thing is hit when the trace crosses the diagonal of its 2r x 2r box that
// faces the trace -- (x-r,y+r)..(x+r,y-r) when the trace's dx and dy share a
// sign, the other diagonal otherwise. The nearest crossing wins; a wall nearer
// than it stops the trace. That makes the target a fixed width of 2r to 2.83r
// by heading, independent of the sprite's drawn size or animation frame.
//
// The trace direction is the view basis f = (cos, sin) tilted by tan(offset):
// dir = f + k * r_hat, r_hat = (-sin, cos). With the target at (dx, dy):
//   crossing   <=>  |dir x (dx,dy)| < r * (|dir_x| + |dir_y|)
//   crossing at t = (dx + dy) / (dir_x + dir_y)  (or dx - dy over dir_x - dir_y)
//   its view depth is t * (f . dir) / 256,
// the same unit as a ray column's depth and BillboardMeasure.forward.
BillboardFireResult billboard_fire_hitscan(const PlayerState *player, s16 spread_q12,
                                           u16 wall_depth, u16 range, u16 damage) {
    BillboardFireResult result = {BILLBOARD_SHOT_NONE, 0, 0, 0, 0, FALSE, 0, 0, FALSE};
    BillboardObject *best_object = NULL;
    u16 best_index = 0;
    s32 best_depth = 0x7FFFFFFF;

    const s16 cos_a = fx_cos(player->angle);
    const s16 sin_a = fx_sin(player->angle);
    const s16 dir_x = (s16)(cos_a - (s16)(((s32)spread_q12 * sin_a) >> 12));
    const s16 dir_y = (s16)(sin_a + (s16)(((s32)spread_q12 * cos_a) >> 12));
    // (f . dir) / 256, ~358: fits a 16-bit multiplier and keeps depth in view
    // units. It is |f|^2 up to the rounding of the tilt, which it absorbs.
    const s32 dir_depth = (((s32)cos_a * dir_x) + ((s32)sin_a * dir_y)) >> 8;
    // Doom tests (dx ^ dy) > 0, which is false when dx == dy: the trace then
    // takes the diagonal parallel to itself and can hit nothing. Doom's fine
    // angles make that practically unreachable; here 90 of the 256 x 801
    // heading/spread pairs a trace can take land on it (tools/test-hitscan.py),
    // so this tests sign equality instead, which picks the crossing diagonal
    // there and keeps `den` nonzero.
    const bool trace_positive = (bool)((dir_x ^ dir_y) >= 0);
    const s32 den = trace_positive ? ((s32)dir_x + dir_y) : ((s32)dir_x - dir_y);
    const s32 dir_extent = (s32)((dir_x < 0) ? -dir_x : dir_x) +
                           (s32)((dir_y < 0) ? -dir_y : dir_y);

    // Where the trace stops: the wall it meets, or a melee weapon's reach.
    u16 stop_depth = wall_depth;
    const u16 range_depth = (range > 0) ? WORLD_TO_VIEW_DEPTH(range) : 0xFFFFu;
    if (range_depth < stop_depth) {
        stop_depth = range_depth;
    }

    const u16 *indices = billboard_registry_target_indices();
    const u16 target_count = billboard_registry_target_count();
    for (u16 slot = 0; slot < target_count; slot++) {
        const u16 i = indices[slot];
        BillboardObject *object = &g_billboards[i];
        if (!object->active) {
            continue;
        }
        const BillboardType *type = billboard_get_type(object->type_id);
        if (!type->targetable) {
            continue;
        }
        // A dying/dead enemy is a corpse, and a detonating barrel is already
        // gone: neither is shootable any more (Doom clears MF_SHOOTABLE).
        if (object->life_state != ENEMY_ALIVE) {
            continue;
        }
        const s32 dx = object->x - player->x;
        const s32 dy = object->y - player->y;
        // Beyond this nothing can be nearer than the draw distance, and the
        // products below stay in range.
        if ((dx > 2048) || (dx < -2048) || (dy > 2048) || (dy < -2048)) {
            continue;
        }
        const s32 radius = (object->type_id == BILLBOARD_TYPE_BARREL) ?
            DOOM_SHOOT_RADIUS_BARREL : DOOM_SHOOT_RADIUS_MONSTER;
        s32 lateral = ((s32)dir_x * (s16)dy) - ((s32)dir_y * (s16)dx);
        if (lateral < 0) {
            lateral = -lateral;
        }
        if (lateral >= radius * dir_extent) {
            continue;  // the trace passes beside the box
        }
        const s32 num = trace_positive ? (dx + dy) : (dx - dy);
        if ((num == 0) || ((num < 0) != (den < 0))) {
            continue;  // the crossing is behind the player
        }
        const s32 depth = ((s32)(s16)num * (s16)dir_depth) / den;
        // Nothing past the wall or the reach, and nothing the renderer does
        // not draw: a shot must not land on an invisible target.
        if ((depth >= stop_depth) || (depth >= type->max_depth)) {
            continue;
        }
        if (depth < best_depth) {
            best_depth = depth;
            best_object = object;
            best_index = i;
        }
    }

    if (best_object == NULL) {
        // Doom leaves a puff only where the trace meets a wall within range;
        // a punch or saw swing into open air leaves nothing.
        if (wall_depth <= range_depth) {
            spawn_wall_puff(player, dir_x, dir_y, dir_depth, wall_depth);
        }
        return result;
    }

    result.hit_target = TRUE;
    result.target_x = best_object->x;
    result.target_y = best_object->y;

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
        result.pain = (bool)(best_object->type_id == BILLBOARD_TYPE_DUMMY);
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
            player, best_object->x, best_object->y), best_object);
    }

    billboard_registry_deactivate(best_index);
    best_object->hp = 0;
    result.status = BILLBOARD_SHOT_KILL;
    return result;
}
