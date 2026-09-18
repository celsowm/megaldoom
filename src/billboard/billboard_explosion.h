#ifndef MEGALDOOM_BILLBOARD_EXPLOSION_H
#define MEGALDOOM_BILLBOARD_EXPLOSION_H

#include <genesis.h>
#include "raycast.h"

// Result of a single barrel detonation, returned to main.c so it can apply
// player damage and knockback on top of the existing enemy-update path. Kept
// as a sibling struct to BillboardEnemyUpdate so the two systems stay disjoint
// (explosions are not enemy AI and vice versa).
typedef struct {
    u16 player_damage;
    s32 thrust_x;  // Q16.16 momentum, summed over every blast (see BillboardFireResult)
    s32 thrust_y;
    u8 explosion_count;
} BarrelExplosionResult;

// Detonate a barrel at (origin_x, origin_y) and apply AoE:
//   - other BARREL with life_state == ENEMY_ALIVE in radius -> chain detonate;
//   - DUMMY/BARREL with life_state == ENEMY_ALIVE -> Doom's P_RadiusAttack:
//     128 - (Chebyshev distance - the target's Doom radius);
//   - player in radius AND with line-of-sight to the blast origin (raycast
//     occlusion test, mirroring billboard_fire_hitscan's wall-depth check) ->
//     accumulate damage and each blast's P_DamageMobj thrust.
//
// Pickups and decor are not damaged. Chain reactions are bounded by
// BARREL_EXPLOSION_MAX_CHAIN to prevent unbounded worklist growth.
BarrelExplosionResult billboard_apply_explosion(const PlayerState *player,
                                                s32 origin_x, s32 origin_y);

// Doom's P_RadiusAttack(128): Chebyshev distance minus the target's radius,
// and 128 minus that is the damage (zero from 128 units on).
u16 billboard_explosion_damage(s32 blast_x, s32 blast_y,
                               s32 target_x, s32 target_y,
                               u16 target_radius);

#endif
