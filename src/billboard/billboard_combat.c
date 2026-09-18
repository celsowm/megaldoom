#include "billboard_internal.h"
#include "billboard_effects.h"
#include "billboard_explosion.h"
#include "bsp_map.h"
#include "doom_random.h"

#define ENEMY_RADIUS 24
#define WALL_PUFF_DEPTH_BIAS 24

static bool is_position_blocked(s32 x, s32 y) {
#if DEBUG_PERF
    bsp_debug_set_query_owner(BSP_QUERY_ENEMY);
#endif
    return bsp_circle_blocked(x, y, ENEMY_RADIUS) ||
           billboard_position_blocked(x, y, ENEMY_RADIUS);
}

// Doom's P_DamageMobj on a monster, for the parts that are not hit points.
//
// Knockback: thrust = damage * 100 / mass (mass 100 for the zombieman, the
// shotgun guy and the imp) along the shot, which the monster's ground
// friction (29/32 a tic) bleeds off over its slide: damage * 8192 * 32/3 in
// Q16, i.e. about damage * 4/3 world units in all. This engine has no monster
// momentum, so the slide is applied at once, in steps of at most 16 units per
// axis so it stops at the first wall instead of tunnelling. The chainsaw
// pushes nothing, as in Doom.
#define DOOM_MONSTER_SLIDE_NUM 4
#define DOOM_MONSTER_SLIDE_DEN 3
#define MONSTER_SLIDE_STEP 16
static void knock_back_dummy(u16 index, BillboardObject *object, const PlayerState *player,
                             u16 damage) {
    if (object->type_id != BILLBOARD_TYPE_DUMMY) {
        return;
    }
    const u16 angle = billboard_vector_angle(object->x - player->x, object->y - player->y);
    const s32 slide = ((s32)damage * DOOM_MONSTER_SLIDE_NUM) / DOOM_MONSTER_SLIDE_DEN;
    // fx_cos/fx_sin carry the 303/256 basis gain; divide it back out.
    s32 remaining_x = (slide * fx_cos(angle)) / 303;
    s32 remaining_y = (slide * fx_sin(angle)) / 303;
    bool moved = FALSE;
    while ((remaining_x != 0) || (remaining_y != 0)) {
        s32 step_x = remaining_x;
        s32 step_y = remaining_y;
        if (step_x > MONSTER_SLIDE_STEP) step_x = MONSTER_SLIDE_STEP;
        if (step_x < -MONSTER_SLIDE_STEP) step_x = -MONSTER_SLIDE_STEP;
        if (step_y > MONSTER_SLIDE_STEP) step_y = MONSTER_SLIDE_STEP;
        if (step_y < -MONSTER_SLIDE_STEP) step_y = -MONSTER_SLIDE_STEP;
        if (is_position_blocked(object->x + step_x, object->y + step_y)) {
            break;
        }
        object->x = (s16)(object->x + step_x);
        object->y = (s16)(object->y + step_y);
        remaining_x -= step_x;
        remaining_y -= step_y;
        moved = TRUE;
    }
    if (moved) {
        billboard_invalidate_object_visibility(index);
    }
}

// Pain: a monster that survives rolls P_Random() < painchance (zombieman and
// imp 200, shotgun guy 170). On a pain it holds its pain state -- POSS/SPOS
// 3 + 3 tics, TROO 2 + 2 -- and MF_JUSTHIT makes its next attack check fire
// at once. Otherwise it keeps doing what it was doing.
static void roll_dummy_pain(BillboardObject *object) {
    const bool is_imp = (bool)(object->visual_id == BILLBOARD_VISUAL_IMP);
    const u8 painchance = object->shotgun_guy ? 170 : 200;
    if (doom_random() >= painchance) {
        return;
    }
    const u8 pain_tics = is_imp ? 4 : 6;
    if (object->move_cooldown < pain_tics) {
        object->move_cooldown = pain_tics;
    }
    object->attack_cooldown = 0;
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
    const BillboardFireResult result = {
        .status = BILLBOARD_SHOT_EXPLOSION,
        .player_damage = explosion.player_damage,
        .thrust_x = explosion.thrust_x,
        .thrust_y = explosion.thrust_y,
        .explosion_count = explosion.explosion_count,
        .hit_target = TRUE,
        .target_x = barrel->x,
        .target_y = barrel->y,
    };
    return result;
}

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
    return billboard_vector_angle(x - player->x, y - player->y);
}

u16 billboard_vector_angle(s32 dx, s32 dy) {
    // Maximise the dot product with the heading: 16 coarse headings, then
    // halve the step down to one angle unit. 24 evaluations, hits only.
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

void billboard_damage_thrust(s32 from_x, s32 from_y, s32 to_x, s32 to_y, u16 damage,
                             s32 *thrust_x, s32 *thrust_y) {
    // Doom: thrust = damage * (FRACUNIT >> 3) * 100 / mass, applied along
    // R_PointToAngle2(inflictor, target). The basis carries the 303/256 gain,
    // so each component divides it back out.
    const u16 angle = billboard_vector_angle(to_x - from_x, to_y - from_y);
    const s32 thrust = (s32)damage * 8192;
    *thrust_x = (thrust / 303) * fx_cos(angle);
    *thrust_y = (thrust / 303) * fx_sin(angle);
}

bool billboard_trace_crosses_box(s16 dir_x, s16 dir_y, s16 dx, s16 dy, s16 radius) {
    // See billboard_fire_hitscan for the geometry and the sign-equality note.
    const bool positive = (bool)((dir_x ^ dir_y) >= 0);
    const s32 den = positive ? ((s32)dir_x + dir_y) : ((s32)dir_x - dir_y);
    const s32 extent = (s32)((dir_x < 0) ? -dir_x : dir_x) +
                       (s32)((dir_y < 0) ? -dir_y : dir_y);
    s32 lateral = ((s32)dir_x * dy) - ((s32)dir_y * dx);
    if (lateral < 0) {
        lateral = -lateral;
    }
    if (lateral >= (s32)radius * extent) {
        return FALSE;
    }
    const s32 num = positive ? ((s32)dx + dy) : ((s32)dx - dy);
    return (bool)((num != 0) && ((num < 0) == (den < 0)));
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
                                           u16 wall_depth, u16 range, u16 damage,
                                           bool thrust) {
    BillboardFireResult result = {.status = BILLBOARD_SHOT_NONE};
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
            DOOM_RADIUS_BARREL : DOOM_RADIUS_MONSTER;
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

    // Knock back BEFORE spawning the decal: the blood/puff impact is a
    // static-position pooled effect that never tracks its target after
    // spawning (see spawn_impact in billboard_effects.c), so spawning first
    // left it stranded behind the shoved body. knock_back_dummy no-ops for
    // non-DUMMY types. Doom thrusts before it subtracts health, so a killing
    // shot still shoves the body.
    if (thrust) {
        knock_back_dummy(best_index, best_object, player, damage);
    }
    if (best_object->type_id == BILLBOARD_TYPE_DUMMY) {
        billboard_effects_spawn_blood(best_object->x, best_object->y);
    } else {
        billboard_effects_spawn_puff(best_object->x, best_object->y);
    }

    if (best_object->hp > damage) {
        best_object->hp = (u8)(best_object->hp - damage);
        result.status = BILLBOARD_SHOT_DAMAGE;
        if (best_object->type_id == BILLBOARD_TYPE_DUMMY) {
            roll_dummy_pain(best_object);
            result.pain = TRUE;
        }
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
