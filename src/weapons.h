#ifndef MEGALDOOM_WEAPONS_H
#define MEGALDOOM_WEAPONS_H

#include <genesis.h>
#include "doom_random.h"

// The DOOM1.WAD shareware arsenal minus the rocket launcher (a rocket is a
// moving projectile, which this engine has no object kind for). All five are
// hitscan, so they share one fire path: N pellets, each a world-space trace
// through billboard_fire_hitscan() (Doom's P_LineAttack against thing radii).
//
// The order is Doom's weapon-cycle order and is the order the overlay sprites
// are baked in (FREEDOOM_WEAPON_IDLE/FIRE, tools/convert-freedoom-assets.ps1).
// MEGALDOOM_WEAPON_COUNT in generated_renderer_assets.h must equal WEAPON_COUNT;
// tools/test-weapons.py asserts it.
typedef enum {
    WEAPON_FIST = 0,
    WEAPON_CHAINSAW,
    WEAPON_PISTOL,
    WEAPON_SHOTGUN,
    WEAPON_CHAINGUN,
    WEAPON_COUNT
} WeaponId;

// Doom's ammo types, minus rockets and cells (nothing here consumes them).
typedef enum {
    AMMO_NONE = 0,
    AMMO_BULLETS,
    AMMO_SHELLS,
    AMMO_TYPE_COUNT
} AmmoType;

// A weapon's timing is Doom's psprite state sequence (linuxdoom-1.10 info.c),
// in 35 Hz tics, reduced to the five spans that decide when shots happen:
//
//   trigger -> [windup] fire (-> [gap] fire) -> [tail] A_ReFire
//   A_ReFire, trigger held -> next attack's windup (refire++)
//   A_ReFire, trigger up   -> [release] ready (refire = 0)
//
// main.c runs this on the player's own tic count, the clock the monsters and
// movement already use, so the ratio of the player's fire to theirs is Doom's.
typedef struct {
    u8 ammo_type;         // AmmoType; AMMO_NONE for the melee weapons
    u8 ammo_per_shot;
    u8 pellets;           // traces per fire action: 1, or Doom's 7 for the shotgun
    bool accurate_first;  // P_GunShot(!refire): the first attack of a held
                          // burst fires dead straight (pistol, chaingun)
    u16 melee_range;      // 0 = hitscan out to the wall; otherwise Doom's reach in world units
    u8 windup_tics;       // attack start to the fire action (the pistol's PISTOL1)
    u8 shots;             // fire actions per attack: 2 for the chaingun and saw
    u8 shot_gap_tics;     // between those two
    u8 tail_tics;         // last fire action to A_ReFire
    u8 release_tics;      // A_ReFire's own state, run only when the trigger is up
    u8 flash_vblanks;     // how long the fire pose is held (display only)
    const u8 *sfx;
    u32 sfx_len;
} WeaponDef;

extern const WeaponDef WEAPON_DEFS[WEAPON_COUNT];

// Per-type carry limits, indexed by AmmoType. Doom's backpack-less maxima.
extern const u16 AMMO_MAX[AMMO_TYPE_COUNT];

// Ammo granted by picking the weapon up, and the bit that marks it owned.
extern const u8 WEAPON_PICKUP_AMMO[WEAPON_COUNT];
#define WEAPON_OWNED_BIT(weapon) ((u8)(1u << (weapon)))
// Doom starts you with fists and a pistol.
#define WEAPON_START_OWNED ((u8)(WEAPON_OWNED_BIT(WEAPON_FIST) | WEAPON_OWNED_BIT(WEAPON_PISTOL)))
#define WEAPON_START_BULLETS 50

// One trace's damage and aim, rolled the way the weapon's Doom action
// function does on the shared P_Random table (doom_random.h): 5 * (1 +
// P_Random() % 3) per bullet, 2 * (1 + P_Random() % 10) per punch or saw
// tooth; the aim offset is Doom's (P_Random() - P_Random()) << 18 as tan in
// Q12, or zero for an accurate shot.
u16 weapon_roll_damage(const WeaponDef *weapon);
s16 weapon_roll_spread_q12(bool accurate);

// Next/previous owned weapon that can actually fire, skipping ones the player
// does not own and ones whose ammo pool is empty (Doom's cycle behaviour).
// `ammo` is indexed by AmmoType. Returns `current` when nothing else qualifies.
u8 weapon_cycle(u8 current, u8 owned, const u16 *ammo, bool forward);

// Whether the weapon can fire right now given the player's pools.
bool weapon_has_ammo(u8 weapon, const u16 *ammo);

#endif
