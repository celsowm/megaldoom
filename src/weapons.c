#include "weapons.h"
#include "resources.h"

// Cooldowns are Doom's refire cycles (linuxdoom-1.10 info.c), converted from
// 35 Hz tics to 60 Hz vblanks (tics * 12 / 7, rounded). A held trigger loops
// back through A_ReFire, which skips each weapon's last state, so the cycle is:
//   fist     PUNCH1-4    4+4+5+4         = 17 tics -> 29 vb
//   chainsaw SAW1 or 2   4 (A_Saw each)  =  4 tics ->  7 vb
//   pistol   PISTOL1-3   4+6+4           = 14 tics -> 24 vb
//   shotgun  SGUN1-8     3+7+5+5+4+5+5+3 = 37 tics -> 63 vb
//   chaingun CHAIN1 or 2 4 (A_FireCGun)  =  4 tics ->  7 vb
// main.c carries a cooldown's overshoot into the next shot, so a burst keeps
// this rate even when a frame takes longer than one cycle (the chaingun and
// chainsaw at ~10 vb a motion frame).
//
// melee_range is Doom's MELEERANGE (64 units), and MELEERANGE + 1 for the saw.
// It is measured, as in P_LineAttack, to where the trace crosses the target's
// radius box, not to its centre, and it is hard-clamped against the wall depth.
const WeaponDef WEAPON_DEFS[WEAPON_COUNT] = {
    [WEAPON_FIST] = {
        AMMO_NONE, 0, 1, 0, 64, 29, 6,
        sfx_punch, sizeof(sfx_punch),
    },
    [WEAPON_CHAINSAW] = {
        AMMO_NONE, 0, 1, 0, 65, 7, 4,
        sfx_chainsaw, sizeof(sfx_chainsaw),
    },
    [WEAPON_PISTOL] = {
        AMMO_BULLETS, 1, 1, 1, 0, 24, 6,
        sfx_pistol, sizeof(sfx_pistol),
    },
    [WEAPON_SHOTGUN] = {
        AMMO_SHELLS, 1, 7, 0, 0, 63, 8,
        sfx_shotgun, sizeof(sfx_shotgun),
    },
    [WEAPON_CHAINGUN] = {
        AMMO_BULLETS, 1, 1, 2, 0, 7, 3,
        sfx_pistol, sizeof(sfx_pistol),
    },
};

const u16 AMMO_MAX[AMMO_TYPE_COUNT] = { 0, 200, 50 };

// Doom gives a dropped/placed weapon a starting clip: shotgun 8 shells,
// chaingun 20 bullets. The melee weapons carry no ammo.
const u8 WEAPON_PICKUP_AMMO[WEAPON_COUNT] = { 0, 0, 0, 8, 20 };

// Doom's rndtable (m_random.c), verbatim.
static const u8 RNDTABLE[256] = {
    0,   8, 109, 220, 222, 241, 149, 107,  75, 248, 254, 140,  16,  66,
    74,  21, 211,  47,  80, 242, 154,  27, 205, 128, 161,  89,  77,  36,
    95, 110,  85,  48, 212, 140, 211, 249,  22,  79, 200,  50,  28, 188,
    52, 140, 202, 120,  68, 145,  62,  70, 184, 190,  91, 197, 152, 224,
    149, 104,  25, 178, 252, 182, 202, 182, 141, 197,   4,  81, 181, 242,
    145,  42,  39, 227, 156, 198, 225, 193, 219,  93, 122, 175, 249,   0,
    175, 143,  70, 239,  46, 246, 163,  53, 163, 109, 168, 135,   2, 235,
    25,  92,  20, 145, 138,  77,  69, 166,  78, 176, 173, 212, 166, 113,
    94, 161,  41,  50, 239,  49, 111, 164,  70,  60,   2,  37, 171,  75,
    136, 156,  11,  56,  42, 146, 138, 229,  73, 146,  77,  61,  98, 196,
    135, 106,  63, 197, 195,  86,  96, 203, 113, 101, 170, 247, 181, 113,
    80, 250, 108,   7, 255, 237, 129, 226,  79, 107, 112, 166, 103, 241,
    24, 223, 239, 120, 198,  58,  60,  82, 128,   3, 184,  66, 143, 224,
    145, 224,  81, 206, 163,  45,  63,  90, 168, 114,  59,  33, 159,  95,
    28, 139, 123,  98, 125, 196,  15,  70, 194, 253,  54,  14, 109, 226,
    71,  17, 161,  93, 186,  87, 244, 138,  20,  52, 123, 251,  26,  36,
    17,  46,  52, 231, 232,  76,  31, 221,  84,  37, 216, 165, 212, 106,
    197, 242,  98,  43,  39, 175, 254, 145, 190,  84, 118, 222, 187, 136,
    120, 163, 236, 249
};
static u8 s_rng_index = 0;

u8 weapon_rng_next(void) {
    s_rng_index++;  // u8: wraps at 256 like Doom's (prndindex + 1) & 0xff
    return RNDTABLE[s_rng_index];
}

void weapon_rng_reset(void) {
    s_rng_index = 0;
}

u16 weapon_roll_damage(const WeaponDef *weapon) {
    if (weapon->melee_range > 0) {
        return (u16)(2 * ((weapon_rng_next() % 10) + 1));  // A_Punch, A_Saw
    }
    return (u16)(5 * ((weapon_rng_next() % 3) + 1));       // P_GunShot
}

s16 weapon_roll_spread_q12(bool accurate) {
    if (accurate) {
        return 0;
    }
    // (P_Random() - P_Random()) << 18 is d * 2^18 / 2^32 of a turn, i.e.
    // d * 3.835e-4 rad; tan() of at most 0.098 rad is that angle to 0.3%.
    // In Q12 that is d * 1.5708, and 201 / 128 = 1.5703.
    const s16 first = weapon_rng_next();
    const s16 d = (s16)(first - (s16)weapon_rng_next());
    return (s16)(((s32)d * 201) / 128);
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
