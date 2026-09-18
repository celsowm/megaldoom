#include "weapons.h"
#include "resources.h"

// Timelines from linuxdoom-1.10 info.c (tics; A_ReFire's state is the release):
//   fist     PUNCH1 4 | A_Punch PUNCH2 4, PUNCH3 5, PUNCH4 4 | PUNCH5 5
//            -> windup 4, tail 13, release 5: a punch every 17 tics held
//   chainsaw A_Saw SAW1 4, A_Saw SAW2 4 | SAW3 0
//            -> windup 0, 2 shots 4 apart, tail 4, release 0: every 4 tics
//   pistol   PISTOL1 4 | A_FirePistol PISTOL2 6, PISTOL3 4 | PISTOL4 5
//            -> windup 4, tail 10, release 5: every 14 tics held
//   shotgun  SGUN1 3 | A_FireShotgun SGUN2 7, SGUN3-8 5+5+4+5+5+3 | SGUN9 7
//            -> windup 3, tail 34, release 7: every 37 tics held
//   chaingun A_FireCGun CHAIN1 4, A_FireCGun CHAIN2 4 | CHAIN3 0
//            -> windup 0, 2 shots 4 apart, tail 4, release 0: every 4 tics
// So a tap of the chaingun or saw is two shots, and a fresh pistol press
// waits 4 tics (~0.11 s) before the bullet leaves, as in Doom.
//
// melee_range is Doom's MELEERANGE (64 units), and MELEERANGE + 1 for the saw.
// It is measured, as in P_LineAttack, to where the trace crosses the target's
// radius box, not to its centre, and it is hard-clamped against the wall depth.
const WeaponDef WEAPON_DEFS[WEAPON_COUNT] = {
    [WEAPON_FIST] = {
        AMMO_NONE, 0, 1, FALSE, 64, 4, 1, 0, 13, 5, 6,
        sfx_punch, sizeof(sfx_punch),
    },
    [WEAPON_CHAINSAW] = {
        AMMO_NONE, 0, 1, FALSE, 65, 0, 2, 4, 4, 0, 4,
        sfx_chainsaw, sizeof(sfx_chainsaw),
    },
    [WEAPON_PISTOL] = {
        AMMO_BULLETS, 1, 1, TRUE, 0, 4, 1, 0, 10, 5, 6,
        sfx_pistol, sizeof(sfx_pistol),
    },
    [WEAPON_SHOTGUN] = {
        AMMO_SHELLS, 1, 7, FALSE, 0, 3, 1, 0, 34, 7, 8,
        sfx_shotgun, sizeof(sfx_shotgun),
    },
    [WEAPON_CHAINGUN] = {
        AMMO_BULLETS, 1, 1, TRUE, 0, 0, 2, 4, 4, 0, 3,
        sfx_pistol, sizeof(sfx_pistol),
    },
};

const u16 AMMO_MAX[AMMO_TYPE_COUNT] = { 0, 200, 50 };

// Doom gives a dropped/placed weapon a starting clip: shotgun 8 shells,
// chaingun 20 bullets. The melee weapons carry no ammo.
const u8 WEAPON_PICKUP_AMMO[WEAPON_COUNT] = { 0, 0, 0, 8, 20 };

u16 weapon_roll_damage(const WeaponDef *weapon) {
    if (weapon->melee_range > 0) {
        return (u16)(2 * ((doom_random() % 10) + 1));  // A_Punch, A_Saw
    }
    return (u16)(5 * ((doom_random() % 3) + 1));       // P_GunShot
}

s16 weapon_roll_spread_q12(bool accurate) {
    return accurate ? 0 : doom_random_spread_q12(18);
}

bool weapon_has_ammo(u8 weapon, const u16 *ammo) {
    const WeaponDef *def = &WEAPON_DEFS[weapon];
    if (def->ammo_type == AMMO_NONE) {
        return TRUE;
    }
    return ammo[def->ammo_type] >= def->ammo_per_shot;
}

u8 weapon_cycle(u8 current, u8 owned, const u16 *ammo, bool forward) {
    u8 candidate = current;
    for (u8 step = 0; step < (WEAPON_COUNT - 1); step++) {
        candidate = forward ? (u8)((candidate + 1) % WEAPON_COUNT)
                            : (u8)((candidate + WEAPON_COUNT - 1) % WEAPON_COUNT);
        if ((owned & WEAPON_OWNED_BIT(candidate)) == 0) {
            continue;
        }
        if (!weapon_has_ammo(candidate, ammo)) {
            continue;
        }
        return candidate;
    }
    return current;
}
