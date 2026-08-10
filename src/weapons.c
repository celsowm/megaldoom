#include "weapons.h"
#include "resources.h"

// Cooldowns are Doom's per-weapon refire delays converted from 35 Hz tics to
// 60 Hz vblanks (tics * 12 / 7), which is the unit main.c's shot_cooldown counts.
// Doom: pistol 4 tics (~7 vb), shotgun 15 (~26), chaingun 2 (~3), fist/chainsaw
// 4-ish. The pistol keeps the project's shipped 12-vblank feel rather than
// Doom's faster 7, so the existing gun does not change under this table; the
// others are scaled around it.
//
// melee_range is Doom's MELEERANGE (64 units); the chainsaw reaches slightly
// further because it also pulls you in. Both are hard-clamped against the wall
// depth at the fire site, so neither can cut through geometry.
const WeaponDef WEAPON_DEFS[WEAPON_COUNT] = {
    [WEAPON_FIST] = {
        AMMO_NONE, 0, 1, 0, 64, 14, 6, FALSE,
        sfx_punch, sizeof(sfx_punch),
    },
    [WEAPON_CHAINSAW] = {
        AMMO_NONE, 0, 1, 0, 80, 7, 4, TRUE,
        sfx_chainsaw, sizeof(sfx_chainsaw),
    },
    [WEAPON_PISTOL] = {
        AMMO_BULLETS, 1, 1, 0, 0, 12, 6, FALSE,
        sfx_pistol, sizeof(sfx_pistol),
    },
    [WEAPON_SHOTGUN] = {
        AMMO_SHELLS, 1, 7, 6, 0, 26, 8, FALSE,
        sfx_shotgun, sizeof(sfx_shotgun),
    },
    [WEAPON_CHAINGUN] = {
        AMMO_BULLETS, 1, 1, 0, 0, 5, 3, TRUE,
        sfx_pistol, sizeof(sfx_pistol),
    },
};

const u16 AMMO_MAX[AMMO_TYPE_COUNT] = { 0, 200, 50 };

// Doom gives a dropped/placed weapon a starting clip: shotgun 8 shells,
// chaingun 20 bullets. The melee weapons carry no ammo.
const u8 WEAPON_PICKUP_AMMO[WEAPON_COUNT] = { 0, 0, 0, 8, 20 };

// 5/10/15 in the order Doom's own P_Random table produces most often at the
// start of a level; any fixed permutation with this multiset works, the point is
// only that it is the same every run. See weapon_roll_damage in weapons.h.
static const u8 DAMAGE_ROLL[3] = { 10, 5, 15 };
static u8 s_damage_roll_index = 0;

u16 weapon_roll_damage(void) {
    const u16 damage = DAMAGE_ROLL[s_damage_roll_index];
    s_damage_roll_index = (u8)((s_damage_roll_index + 1) % 3);
    return damage;
}

void weapon_reset_damage_roll(void) {
    s_damage_roll_index = 0;
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
