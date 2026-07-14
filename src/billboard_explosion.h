#ifndef MEGALDOOM_BILLBOARD_EXPLOSION_H
#define MEGALDOOM_BILLBOARD_EXPLOSION_H

#include <genesis.h>
#include "raycast.h"

// Result of a single barrel detonation, returned to main.c so it can apply
// player damage and knockback on top of the existing enemy-update path. Kept
// as a sibling struct to BillboardEnemyUpdate so the two systems stay disjoint
// (explosions are not enemy AI and vice versa).
typedef struct {
    u8 player_hits;
    s16 push_x;
    s16 push_y;
} BarrelExplosionResult;

// Detonate a barrel at (origin_x, origin_y) and apply AoE:
//   - other BARREL with life_state == ENEMY_ALIVE in radius -> chain detonate;
//   - DUMMY with life_state == ENEMY_ALIVE in radius -> splash-damage, killing
//     via the existing death path if hp drops to 0;
//   - player in radius AND with line-of-sight to the blast origin (raycast
//     occlusion test, mirroring billboard_fire_center's wall-depth check) ->
//     accumulate player_hits with a normalized knockback direction.
//
// Pickups and decor are not damaged. Chain reactions are bounded by
// BARREL_EXPLOSION_MAX_CHAIN to prevent unbounded worklist growth.
BarrelExplosionResult billboard_apply_explosion(const PlayerState *player,
                                                s32 origin_x, s32 origin_y);

// Returns the result of the most-recent billboard_apply_explosion call. main.c
// consults this only when billboard_fire_center() returned BILLBOARD_SHOT_EXPLOSION.
BarrelExplosionResult billboard_get_last_explosion_result(void);

#endif
