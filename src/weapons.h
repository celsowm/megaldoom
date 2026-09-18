#ifndef MEGALDOOM_WEAPONS_H
#define MEGALDOOM_WEAPONS_H

#include <genesis.h>

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

typedef struct {
    u8 ammo_type;         // AmmoType; AMMO_NONE for the melee weapons
    u8 ammo_per_shot;
    u8 pellets;           // 1, or Doom's 7 for the shotgun
    u8 accurate_shots;    // shots of a held burst fired dead straight (Doom's
                          // refire == 0): pistol 1, chaingun 2, others 0
    u16 melee_range;      // 0 = hitscan out to the wall; otherwise Doom's reach in world units
    u8 cooldown_vblanks;  // real vblanks between shots of a held burst
    u8 flash_vblanks;     // how long the fire pose is held
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

// Doom's P_Random: the fixed 256-entry rndtable walked by an index. Damage and
// spread use Doom's own formulas on it (5 * (1 + P_Random() % 3) per bullet,
// 2 * (1 + P_Random() % 10) per punch or saw tooth, (P_Random() - P_Random())
// << 18 of angle for an inaccurate shot). It is a table, not a PRNG, so combat
// stays reproducible: enter_level resets the index, exactly as Doom's
// M_ClearRandom does, and the BlastEm route harness replays fixed input.
u8 weapon_rng_next(void);
void weapon_rng_reset(void);
// One shot's damage, rolled the way the weapon's Doom action function does.
u16 weapon_roll_damage(const WeaponDef *weapon);
// One shot's aim offset, as tan(angle) in Q12 (the pellet's view-space slope).
// Zero for an accurate shot; otherwise Doom's (P_Random() - P_Random()) << 18,
// a triangular spread of at most +-5.6 degrees.
s16 weapon_roll_spread_q12(bool accurate);

// Next/previous owned weapon that can actually fire, skipping ones the player
// does not own and ones whose ammo pool is empty (Doom's cycle behaviour).
// `ammo` is indexed by AmmoType. Returns `current` when nothing else qualifies.
u8 weapon_cycle(u8 current, u8 owned, const u16 *ammo, bool forward);

// Whether the weapon can fire right now given the player's pools.
bool weapon_has_ammo(u8 weapon, const u16 *ammo);

#endif
