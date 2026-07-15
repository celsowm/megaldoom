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
    s16 push_x;
    s16 push_y;
    u8 explosion_count;
} BarrelExplosionResult;

// Detonate a barrel at (origin_x, origin_y) and apply AoE:
//   - other BARREL with life_state == ENEMY_ALIVE in radius -> chain detonate;
//   - DUMMY/BARREL with life_state == ENEMY_ALIVE -> Doom-style 128-distance
//     splash damage after subtracting the target collision radius;
//   - player in radius AND with line-of-sight to the blast origin (raycast
//     occlusion test, mirroring billboard_fire_center's wall-depth check) ->
//     accumulate damage and retain the strongest knockback direction.
//
// Pickups and decor are not damaged. Chain reactions are bounded by
// BARREL_EXPLOSION_MAX_CHAIN to prevent unbounded worklist growth.
BarrelExplosionResult billboard_apply_explosion(const PlayerState *player,
                                                s32 origin_x, s32 origin_y);

// Doom-style Chebyshev distance minus the target radius, with 128 maximum
// damage falling linearly to zero at the tuned 192-unit gameplay radius.
u16 billboard_explosion_damage(s32 blast_x, s32 blast_y,
                               s32 target_x, s32 target_y,
                               u16 target_radius);

#endif
