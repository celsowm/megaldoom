#!/usr/bin/env python3
"""The player's hitscan follows Doom's P_LineAttack, not the drawn sprite.

billboard_fire_hitscan (src/billboard/billboard_combat.c) replaced a
screen-column test against the projected sprite width with Doom's own rule
(p_maputl.c PIT_AddThingIntercepts): a trace hits a thing when it crosses the
diagonal of the thing's 2r x 2r box that faces it. This test

1. mirrors the C integer math in Python and checks it against an independent
   float geometry reference (segment/line intersection) over a fuzz;
2. proves the fuzz can fail: two deliberately broken variants must mismatch;
3. pins the Doom numbers the change carries (radii, HP, refire cycles, damage
   and spread formulas, the rndtable) against the source.
"""

from pathlib import Path
import math
import random
import re

ROOT = Path(__file__).resolve().parents[1]
COMBAT_C = (ROOT / "src/billboard/billboard_combat.c").read_text()
BILLBOARD_C = (ROOT / "src/billboard/billboard.c").read_text()
INTERNAL_H = (ROOT / "src/billboard/billboard_internal.h").read_text()
WEAPONS_C = (ROOT / "src/weapons.c").read_text()
WEAPONS_H = (ROOT / "src/weapons.h").read_text()
MAIN_C = (ROOT / "src/main.c").read_text()

ANGLE_STEPS = 256
ANGLE_90 = 64


# --- fx_sin / fx_cos, as fixed_math.c builds them ----------------------------
def _sin_quarter_q8(angle):
    a = angle & (ANGLE_90 - 1)
    x = (a * 256) // ANGLE_90
    x2 = (x * x) >> 8
    x3 = (x2 * x) >> 8
    x5 = (x3 * x2) >> 8
    return (479 * x - 196 * x3 + 24 * x5) >> 8


SIN = []
for i in range(ANGLE_STEPS):
    quadrant, local = i // ANGLE_90, i & (ANGLE_90 - 1)
    if quadrant == 0:
        v = _sin_quarter_q8(local)
    elif quadrant == 1:
        v = _sin_quarter_q8(ANGLE_90 - 1 - local)
    elif quadrant == 2:
        v = -_sin_quarter_q8(local)
    else:
        v = -_sin_quarter_q8(ANGLE_90 - 1 - local)
    SIN.append(v)


def fx_sin(a):
    return SIN[a & 255]


def fx_cos(a):
    return SIN[(a + ANGLE_90) & 255]


def c_div(a, b):
    """C99 integer division: truncates toward zero."""
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


def trace_dir(angle, spread_q12):
    c, s = fx_cos(angle), fx_sin(angle)
    dir_x = c - ((spread_q12 * s) >> 12)
    dir_y = s + ((spread_q12 * c) >> 12)
    return c, s, dir_x, dir_y


# --- the C, in Python ----------------------------------------------------------
def hitscan_depth(angle, spread_q12, dx, dy, radius, variant=None):
    """billboard_fire_hitscan's per-target test: the crossing's view depth, or
    None for a miss. `variant` selects a deliberately broken negative control."""
    c, s, dir_x, dir_y = trace_dir(angle, spread_q12)
    dir_depth = (c * dir_x + s * dir_y) >> 8
    positive = (dir_x ^ dir_y) >= 0
    if variant == "swapped-diagonal":
        positive = not positive
    if variant == "doom-strict-xor":
        positive = (dir_x ^ dir_y) > 0
    den = (dir_x + dir_y) if positive else (dir_x - dir_y)
    if den == 0:
        return None
    extent = abs(dir_x) + abs(dir_y)
    lateral = abs(dir_x * dy - dir_y * dx)
    if variant == "centre-only-width":
        extent = abs(den)  # the old habit: width measured along one axis only
        if positive:
            extent = max(abs(dir_x), abs(dir_y))
    if lateral >= radius * extent:
        return None
    num = (dx + dy) if positive else (dx - dy)
    if num == 0 or ((num < 0) != (den < 0)):
        return None
    return c_div(num * dir_depth, den)


# --- independent reference: Doom's crossing, in floats ---------------------------
def reference_depth(angle, spread_q12, dx, dy, radius):
    """Intersect the trace ray with the facing diagonal of the thing's box, as
    PIT_AddThingIntercepts does, with plain float geometry. Returns the view
    depth of the crossing (dot with the view basis / 256), or None."""
    c, s, dir_x, dir_y = trace_dir(angle, spread_q12)
    positive = (dir_x >= 0) == (dir_y >= 0)
    if positive:
        x1, y1, x2, y2 = dx - radius, dy + radius, dx + radius, dy - radius
    else:
        x1, y1, x2, y2 = dx - radius, dy - radius, dx + radius, dy + radius

    def side(px, py):
        return dir_x * py - dir_y * px

    s1, s2 = side(x1, y1), side(x2, y2)
    if (s1 > 0) == (s2 > 0) or s1 == 0 or s2 == 0:
        return None
    # Solve t*dir = p1 + u*(p2 - p1).
    ex, ey = x2 - x1, y2 - y1
    det = dir_x * (-ey) - dir_y * (-ex)
    if det == 0:
        return None
    t = (x1 * (-ey) - y1 * (-ex)) / det
    if t <= 0:
        return None
    px, py = t * dir_x, t * dir_y
    return (c * px + s * py) / 256.0


def fuzz(variant=None, cases=60000, seed=1993):
    rng = random.Random(seed)
    mismatches = 0
    compared = 0
    for _ in range(cases):
        angle = rng.randrange(ANGLE_STEPS)
        spread = rng.choice((0, 0, rng.randint(-400, 400)))
        radius = rng.choice((10, 20))
        dist = rng.uniform(26, 1600)
        # Aim mostly near the trace so hits and near misses both occur.
        off = rng.uniform(-3.0, 3.0) * radius
        a = 2 * math.pi * angle / ANGLE_STEPS + rng.uniform(-0.12, 0.12)
        dx = int(round(dist * math.cos(a) - off * math.sin(a)))
        dy = int(round(dist * math.sin(a) + off * math.cos(a)))
        want = reference_depth(angle, spread, dx, dy, radius)
        got = hitscan_depth(angle, spread, dx, dy, radius, variant)
        # Skip razor-edge cases: the reference is exact, the C is integer, and
        # a trace grazing a box corner within half a unit may go either way.
        c, s, dir_x, dir_y = trace_dir(angle, spread)
        margin = abs(abs(dir_x * dy - dir_y * dx) -
                     radius * (abs(dir_x) + abs(dir_y))) / math.hypot(dir_x, dir_y)
        if margin < 0.5:
            continue
        compared += 1
        if (want is None) != (got is None):
            mismatches += 1
        # The C scales by (f . dir) >> 8 (~358, truncated), so its depth runs
        # up to 0.3% short of exact: never more than that plus one unit.
        elif want is not None and abs(want - got) > 1.0 + 0.004 * want:
            mismatches += 1
    return compared, mismatches


compared, bad = fuzz()
assert compared > 50000, compared
assert bad == 0, f"hitscan math disagrees with Doom's crossing on {bad}/{compared} cases"
for variant in ("swapped-diagonal", "centre-only-width"):
    _, broken = fuzz(variant)
    assert broken > 100, f"negative control {variant!r} was not caught ({broken})"

# Where dir_x == dir_y, Doom's (dx ^ dy) > 0 picks the diagonal parallel to the
# trace and nothing can be hit. With Doom's fine angles that never happens in
# play; here some spread traces land on it, so the C tests sign equality.
equal = [(a, sp) for a in range(ANGLE_STEPS) for sp in range(-400, 401)
         if trace_dir(a, sp)[2] == trace_dir(a, sp)[3]]
assert equal, "no trace has dir_x == dir_y any more; drop this check"
for a, sp in equal:
    _, _, dx_dir, _ = trace_dir(a, sp)
    d = dx_dir * 3  # dead ahead along the trace
    assert hitscan_depth(a, sp, d, d, 20) is not None, (a, sp)
    assert reference_depth(a, sp, d, d, 20) is not None, (a, sp)
# Negative control: Doom's literal test misses every one of them.
assert all(hitscan_depth(a, sp, trace_dir(a, sp)[2] * 3, trace_dir(a, sp)[2] * 3, 20,
                         "doom-strict-xor") is None for a, sp in equal)
assert "(dir_x ^ dir_y) >= 0" in COMBAT_C


# The hit width is Doom's box, whatever the sprite: 2r facing an axis, 2r*sqrt2
# on a diagonal. Scan the lateral offset at 600 units and measure it.
def hit_width(angle, radius):
    c, s, dir_x, dir_y = trace_dir(angle, 0)
    norm = math.hypot(dir_x, dir_y)
    ux, uy = dir_x / norm, dir_y / norm
    hits = [off for off in range(-60, 61)
            if hitscan_depth(angle, 0, int(round(600 * ux - off * uy)),
                             int(round(600 * uy + off * ux)), radius) is not None]
    return (max(hits) - min(hits) + 1) if hits else 0


assert 39 <= hit_width(0, 20) <= 41, hit_width(0, 20)
assert 55 <= hit_width(32, 20) <= 58, hit_width(32, 20)
assert 19 <= hit_width(64, 10) <= 21, hit_width(64, 10)


# --- P_Random and the spread ------------------------------------------------------
DOOM_RANDOM_C = (ROOT / "src/doom_random.c").read_text()
ENEMY_C = (ROOT / "src/billboard/billboard_enemy.c").read_text()
EXPLOSION_C = (ROOT / "src/billboard/billboard_explosion.c").read_text()

table = [int(v) for v in re.search(
    r"RNDTABLE\[256\] = \{(.*?)\};", DOOM_RANDOM_C, re.S).group(1).split(",")]
assert len(table) == 256
assert table[:8] == [0, 8, 109, 220, 222, 241, 149, 107] and table[-4:] == [120, 163, 236, 249]
assert sum(table) == 32986, sum(table)  # m_random.c's rndtable, byte for byte
assert "doom_random_reset();" in MAIN_C  # M_ClearRandom at every level start


def spread_q12(d, shift):
    """doom_random.c doom_random_spread_q12 for a given P_Random() - P_Random()."""
    x = (d * 804) >> (7 + (20 - shift))
    x2 = (x * x) >> 12
    x3 = (x2 * x) >> 12
    x5 = (x3 * x2) >> 12
    return x + c_div(x3, 3) + c_div(2 * x5, 15)


for token in (
    "const s32 x = (d * 804) >> (7 + (20 - shift));",
    "return (s16)(x + (x3 / 3) + ((2 * x5) / 15));",
):
    assert token in DOOM_RANDOM_C, token
# (P - P) << shift of a 2^32 turn, as tan in Q12, for both of Doom's spreads.
for shift in (18, 20):
    for d in range(-255, 256):
        want = math.tan(d * 2 * math.pi / (1 << (32 - shift))) * 4096
        got = spread_q12(d, shift)
        assert abs(got - want) <= 2 + 0.002 * abs(want), (shift, d, got, want)
assert "return accurate ? 0 : doom_random_spread_q12(18);" in WEAPONS_C

# --- damage rolls ------------------------------------------------------------------
assert "return (u16)(2 * ((doom_random() % 10) + 1));" in WEAPONS_C      # A_Punch, A_Saw
assert "return (u16)(5 * ((doom_random() % 3) + 1));" in WEAPONS_C       # P_GunShot
assert "damage = (u16)(((doom_random() % 8) + 1) * 3);" in ENEMY_C       # A_TroopAttack
assert "const u16 pellet = (u16)(((doom_random() % 5) + 1) * 3);" in ENEMY_C  # A_PosAttack
assert "const u8 pellets = object->shotgun_guy ? 3 : 1;" in ENEMY_C      # A_SPosAttack
assert "const s16 k = doom_random_spread_q12(20);" in ENEMY_C
# The monsters' flat 20-point auto-hit is gone.
assert "PLAYER_HIT_DAMAGE" not in MAIN_C


# --- monster aim: Doom's hit chance, not a certainty -------------------------------
def c_trace_crosses_box(dir_x, dir_y, dx, dy, radius):
    """billboard_combat.c billboard_trace_crosses_box."""
    positive = (dir_x ^ dir_y) >= 0
    den = (dir_x + dir_y) if positive else (dir_x - dir_y)
    extent = abs(dir_x) + abs(dir_y)
    if abs(dir_x * dy - dir_y * dx) >= radius * extent:
        return False
    num = (dx + dy) if positive else (dx - dy)
    return num != 0 and ((num < 0) == (den < 0))


def c_enemy_bullet_hits(dx, dy, k):
    """billboard_enemy.c enemy_attack's aim for one bullet with tilt k (Q12)."""
    ax, ay = dx, dy
    while ax > 16383 or ax < -16383 or ay > 16383 or ay < -16383:
        ax >>= 1
        ay >>= 1
    while (ax or ay) and -8192 < ax < 8192 and -8192 < ay < 8192:
        ax <<= 1
        ay <<= 1
    dir_x = ax - ((k * ay) >> 12)
    dir_y = ay + ((k * ax) >> 12)
    return c_trace_crosses_box(dir_x, dir_y, dx, dy, 16)


def ref_bullet_hits(dx, dy, d):
    """Doom, in floats: face the player exactly, turn by d << 20, and cross the
    player's box diagonal (the one facing the trace) ahead of the shooter."""
    a = math.atan2(dy, dx) + d * 2 * math.pi / (1 << 12)
    ux, uy = math.cos(a), math.sin(a)
    r = 16
    if (ux >= 0) == (uy >= 0):
        x1, y1, x2, y2 = dx - r, dy + r, dx + r, dy - r
    else:
        x1, y1, x2, y2 = dx - r, dy - r, dx + r, dy + r
    s1 = ux * y1 - uy * x1
    s2 = ux * y2 - uy * x2
    if (s1 > 0) == (s2 > 0):
        return False
    ex, ey = x2 - x1, y2 - y1
    det = ux * (-ey) - uy * (-ex)
    t = (x1 * (-ey) - y1 * (-ex)) / det
    return t > 0


# Over every (P_Random, P_Random) pair Doom can roll, the hit share at each
# range and bearing must match Doom's geometry.
pairs = [(a - b) for a in range(256) for b in range(256)]
d_counts = {}
for d in pairs:
    d_counts[d] = d_counts.get(d, 0) + 1
for dist in (48, 96, 192, 400):
    for bearing in (0.0, 0.4, 0.785398, 2.2, 3.9):
        dx = int(round(dist * math.cos(bearing)))
        dy = int(round(dist * math.sin(bearing)))
        c_hits = sum(n for d, n in d_counts.items() if c_enemy_bullet_hits(dx, dy, spread_q12(d, 20)))
        r_hits = sum(n for d, n in d_counts.items() if ref_bullet_hits(dx, dy, d))
        assert abs(c_hits - r_hits) <= 0.01 * len(pairs), (dist, bearing, c_hits, r_hits)
# The share itself, pinned: nearly every shot at 48 units, well under half at
# 192 (this engine's attack range), a small minority at 400.
share = lambda dist: sum(n for d, n in d_counts.items()
                         if c_enemy_bullet_hits(dist, 0, spread_q12(d, 20))) / len(pairs)
assert share(48) > 0.75, share(48)
assert 0.25 < share(192) < 0.5, share(192)
assert share(400) < 0.25, share(400)
# Negative control: the old rule, every attack connects, is far from Doom.
assert abs(1.0 - share(192)) > 0.4

# --- the player's side of P_DamageMobj ----------------------------------------------
for token in (
    # "I'm too young to die" halves damage.
    "if (skill == DOOM_SKILL_IM_TOO_YOUNG_TO_DIE) {",
    "damage >>= 1;",
    # Armour class: green absorbs a third, blue half, until it runs out.
    "u16 saved = (g_player_armor_type == 1) ? (u16)(damage / 3) : (u16)(damage / 2);",
    "if (*player_armor <= saved) {",
    "g_player_armor_type = 0;",
    # P_GiveArmor / the armour bonus.
    "} else if (player_armor < pickup.amount) {",
    "g_player_armor_type = (u8)(pickup.amount / 100);",
    "if (g_player_armor_type == 0) g_player_armor_type = 1;",
    # Knockback rides the player's momentum, through the walking collision.
    "player_controller_add_thrust(thrust_x, thrust_y);",
):
    assert token in MAIN_C, token
# Doom has no invulnerability window after a hit.
assert "g_player_invuln" not in MAIN_C and "PLAYER_INVULN" not in MAIN_C
# Thrust is damage * (FRACUNIT >> 3) * 100 / mass (100) along inflictor -> target.
assert "const s32 thrust = (s32)damage * 8192;" in COMBAT_C
assert "*thrust_x = (thrust / 303) * fx_cos(angle);" in COMBAT_C
assert "billboard_damage_thrust(object->x, object->y, player->x, player->y, damage," in ENEMY_C

# --- the monsters' side ------------------------------------------------------------------
for token in (
    "const u8 painchance = object->shotgun_guy ? 170 : (is_demon ? 180 : 200);",  # info.c
    "if (doom_random() >= painchance) {",
    "const u8 pain_tics = short_pain ? 4 : 6;",   # POSS/SPOS 3+3, TROO/SARG 2+2
    "const bool short_pain = is_demon || (object->visual_id == BILLBOARD_VISUAL_IMP);",
    "state->attack_cooldown = 0;",                             # MF_JUSTHIT
    "#define DOOM_MONSTER_SLIDE_NUM 4",                        # damage * 4/3 units
    "#define DOOM_MONSTER_SLIDE_DEN 3",
    "#define MONSTER_SLIDE_STEP 16",
):
    assert token in COMBAT_C, token
# 8192 * 32/3 / 65536 = 4/3: Doom's thrust over ground friction's geometric sum.
assert abs(8192 * (32 / 3) / 65536 - 4 / 3) < 1e-9
# Pain only after surviving; the chainsaw pushes nothing.
damage_path = COMBAT_C[COMBAT_C.index("    if (best_object->hp > damage) {"):]
assert damage_path.index("roll_dummy_pain(best_object);") < damage_path.index("return result;")
assert "(bool)(weapon != &WEAPON_DEFS[WEAPON_CHAINSAW]));" in MAIN_C
assert "DUMMY_HIT_PUSH_STEP" not in INTERNAL_H and "DUMMY_HIT_STUN_FRAMES" not in INTERNAL_H

# --- radii and HP ------------------------------------------------------------------------
for token in ("#define DOOM_RADIUS_MONSTER 20", "#define DOOM_RADIUS_BARREL 10",
              "#define DOOM_RADIUS_PLAYER 16", "#define BARREL_EXPLOSION_RADIUS 128"):
    assert token in INTERNAL_H, token
assert "const s32 radius = billboard_doom_radius(object);" in COMBAT_C
for token in ("if (object->type_id == BILLBOARD_TYPE_BARREL) return DOOM_RADIUS_BARREL;",
              "return (object->visual_id == BILLBOARD_VISUAL_DEMON) ? DOOM_RADIUS_DEMON"):
    assert token in INTERNAL_H, token
assert "return (u16)(BARREL_EXPLOSION_DAMAGE - distance);" in EXPLOSION_C
assert "#define DOOM_IMP_HEALTH 60" in BILLBOARD_C
assert "#define DOOM_SHOTGUN_GUY_HEALTH 30" in BILLBOARD_C
assert "object->shotgun_guy = 1;" in BILLBOARD_C
assert re.search(r"\{BILLBOARD_VISUAL_BARREL,\s+BILLBOARD_EFFECT_NONE,\s+20,", BILLBOARD_C)
assert re.search(r"\{BILLBOARD_VISUAL_DUMMY,\s+BILLBOARD_EFFECT_NONE,\s+20,", BILLBOARD_C)

# --- the player's hitscan source pins -----------------------------------------------------
assert "BillboardFireResult billboard_fire_hitscan(" in COMBAT_C
assert "aim_col < (measure.center_col - measure.half_w)" not in COMBAT_C
assert "POINT_BLANK" not in COMBAT_C and "POINT_BLANK" not in INTERNAL_H
for token in (
    "const s16 dir_x = (s16)(cos_a - (s16)(((s32)spread_q12 * sin_a) >> 12));",
    "const s16 dir_y = (s16)(sin_a + (s16)(((s32)spread_q12 * cos_a) >> 12));",
    "const s32 dir_depth = (((s32)cos_a * dir_x) + ((s32)sin_a * dir_y)) >> 8;",
    "const s32 den = trace_positive ? ((s32)dir_x + dir_y) : ((s32)dir_x - dir_y);",
    "if (lateral >= radius * dir_extent) {",
    "const s32 num = trace_positive ? (dx + dy) : (dx - dy);",
    "if ((num == 0) || ((num < 0) != (den < 0))) {",
    "const s32 depth = ((s32)(s16)num * (s16)dir_depth) / den;",
    "if (wall_depth <= range_depth) {",  # no puff from a swing into open air
    "#define WORLD_TO_VIEW_DEPTH(units) ((u16)(((u32)(units) * 303u) >> 8))",
    # the shared helper the monsters use is the same test
    "const bool positive = (bool)((dir_x ^ dir_y) >= 0);",
    "if (lateral >= (s32)radius * extent) {",
):
    assert token in COMBAT_C, token

# --- weapon timing: Doom's psprite states, simulated tic by tic ----------------------------
# Reference: the state lists of linuxdoom-1.10 info.c, run the way P_MovePsprites
# and A_WeaponReady / A_ReFire run them. Each state is (tics, action).
DOOM_STATES = {
    "FIST": [(4, None), (4, "fire"), (5, None), (4, None), (5, "refire")],
    "CHAINSAW": [(4, "fire"), (4, "fire"), (0, "refire")],
    "PISTOL": [(4, None), (6, "fire"), (4, None), (5, "refire")],
    "SHOTGUN": [(3, None), (7, "fire"), (5, None), (5, None), (4, None),
                (5, None), (5, None), (3, None), (7, "refire")],
    "CHAINGUN": [(4, "fire"), (4, "fire"), (0, "refire")],
}


def doom_fire_tics(states, held_at, total):
    """Tics at which Doom's weapon fires, and whether each shot is refire == 0."""
    shots = []
    state = None   # None = ready
    tics = 0
    refire = 0

    def enter(i, t):
        nonlocal state, tics, refire
        while True:
            state = i
            n, action = states[i]
            tics = n
            if action == "fire":
                shots.append((t, refire == 0))
            elif action == "refire":
                if held_at(t):
                    refire += 1
                    i = 0
                    continue
                refire = 0
            if tics == 0:
                i += 1
                if i >= len(states):
                    state = None
                    return
                continue
            return

    for t in range(total):
        if state is None:
            if held_at(t):
                enter(0, t)
            continue
        tics -= 1
        if tics <= 0:
            nxt = state + 1
            if nxt >= len(states):
                state = None
                if held_at(t):
                    enter(0, t)
            else:
                enter(nxt, t)
    return shots


rows = dict(re.findall(r"^    \[WEAPON_(\w+)\] = \{\n(.*?)\n    \},$", WEAPONS_C, re.S | re.M))


def weapon_row(name):
    f = [x.strip() for x in rows[name].replace("\n", " ").split(",")]
    return {"pellets": int(f[2]), "accurate_first": f[3] == "TRUE", "melee": int(f[4]),
            "windup": int(f[5]), "shots": int(f[6]), "gap": int(f[7]),
            "tail": int(f[8]), "release": int(f[9])}


MAX_SHOTS_PER_ITERATION = int(
    re.search(r"#define MAX_SHOTS_PER_ITERATION (\d+)", MAIN_C).group(1))
MAX_TIC_CARRY = int(
    re.search(r"#define WEAPON_MAX_TIC_CARRY (\d+)", MAIN_C).group(1))


def c_fire_tics(w, held_at, total, batch=1, variant=None, latch_age=None):
    """main.c weapon_state_step, driven one main-loop iteration per `batch`
    tics with the button sampled once per iteration.

    `latch_age` is how many tics had already elapsed since the trigger edge when
    the iteration was processed -- what the V-Int stamp measures. None means no
    stamp was available, i.e. the conservative end-of-window assumption the code
    falls back to. At batch=1 every choice collapses to 0, which is why the Doom
    parity comparisons above are unaffected by the real-time weapon clock.
    """
    READY, WINDUP, GAP, TAIL, RELEASE = range(5)
    phase, timer, fired, refire = READY, 0, 0, 0
    shots = []
    t = 0
    carry = 0
    while t < total:
        held = held_at(t)
        trigger = held
        tics = batch + carry
        carry = 0
        guard = 0
        fired_this_iteration = 0
        while guard < 16 and fired_this_iteration < MAX_SHOTS_PER_ITERATION:
            guard += 1
            if phase == READY:
                if not trigger:
                    break
                trigger = held
                tics = 0 if latch_age is None else min(tics, latch_age)
                phase, timer, fired = WINDUP, w["windup"], 0
                continue
            if timer > tics:
                timer -= tics
                tics = 0
                break
            tics -= timer
            timer = 0
            if phase in (WINDUP, GAP):
                shots.append((t + batch - 1 - tics, w["accurate_first"] and refire == 0))
                fired += 1
                fired_this_iteration += 1
                if fired < w["shots"]:
                    phase, timer = GAP, w["gap"]
                else:
                    phase, timer = TAIL, w["tail"]
                continue
            if phase == TAIL:
                if held:
                    refire += 1
                    phase, timer, fired = WINDUP, w["windup"], 0
                elif variant == "no-release":
                    refire, phase = 0, READY
                else:
                    refire, phase, timer = 0, RELEASE, w["release"]
                continue
            phase = READY
        # Tics the shot cap left unspent wait for the next iteration, as main.c
        # carries them in weapon_tic_carry.
        carry = min(tics, MAX_TIC_CARRY)
        t += batch
    return shots


PATTERNS = {
    "held": lambda t: True,
    "tap": lambda t: t == 0,
    "burst-then-repress": lambda t: t < 6 or 20 <= t < 70,
    "quick-repress": lambda t: t < 2 or 16 <= t < 18,
}
for name in DOOM_STATES:
    w = weapon_row(name)
    for pattern, held_at in PATTERNS.items():
        doom = doom_fire_tics(DOOM_STATES[name], held_at, 200)
        doom_acc = [(t, acc and w["accurate_first"]) for t, acc in doom]
        got = c_fire_tics(w, held_at, 200)
        assert got == doom_acc, (name, pattern, got[:6], doom_acc[:6])
# The headline numbers, straight from the simulation:
pistol = weapon_row("PISTOL")
held_shots = [t for t, _ in c_fire_tics(pistol, PATTERNS["held"], 200)]
assert held_shots[0] == 4                                   # the first-shot delay
assert {b - a for a, b in zip(held_shots, held_shots[1:])} == {14}
assert [t for t, _ in c_fire_tics(weapon_row("CHAINGUN"), PATTERNS["tap"], 50)] == [0, 4]
shotgun_held = [t for t, _ in c_fire_tics(weapon_row("SHOTGUN"), PATTERNS["held"], 200)]
assert shotgun_held[0] == 3 and {b - a for a, b in zip(shotgun_held, shotgun_held[1:])} == {37}
# Accuracy: the pistol's first held shot only; the chaingun's first two.
assert [acc for _, acc in c_fire_tics(pistol, PATTERNS["held"], 60)] == [True, False, False, False]
assert [acc for _, acc in c_fire_tics(weapon_row("CHAINGUN"), PATTERNS["held"], 20)][:4] == \
    [True, True, False, False]
# Negative control: skipping A_ReFire's release state lets a quick re-press
# fire sooner than Doom does.
assert c_fire_tics(pistol, PATTERNS["quick-repress"], 200, variant="no-release") != \
    [(t, a) for t, a in doom_fire_tics(DOOM_STATES["PISTOL"], PATTERNS["quick-repress"], 200)]
# Batched iterations must not change how many shots a hold fires, only when they
# resolve. The weapon clock credits REAL elapsed vblanks (not the movement
# clamp's ~3 tics), so one iteration on a heavy frame carries far more than it
# used to -- and the invariant has to hold wherever inside the window the
# trigger edge landed, because the V-Int stamp can report any age in [0, batch).
#
# "How many" cannot be counted from t=0: a big window delays the FIRST shot by
# up to one window (the press is only known at the iteration boundary), and over
# a fixed 210 tics that startup shift alone drops whole cycles. The property that
# actually matters is the SUSTAINED one -- once firing starts, the cadence must
# be bit-identical to the unbatched run, which is what proves the shot cap never
# eats a shot (main.c carries unspent tics in weapon_tic_carry).
MAX_WEAPON_TICS = (int(re.search(
    r"#define WEAPON_MAX_CREDITED_VBLANKS (\d+)", MAIN_C).group(1)) * 35) // 60
for name in DOOM_STATES:
    w = weapon_row(name)
    exact = [t for t, _ in c_fire_tics(w, PATTERNS["held"], 400)]
    for batch in (2, 3, 6, 10, 14, MAX_WEAPON_TICS):
        for age in {None, 0, batch // 2, batch - 1}:
            got = [t for t, _ in c_fire_tics(w, PATTERNS["held"], 400,
                                             batch=batch, latch_age=age)]
            assert got, (name, batch, age)
            # Start-up: the trigger is only observed at an iteration boundary.
            assert 0 <= got[0] - exact[0] <= batch, (name, batch, age, got[0])
            # Cadence: every later shot lands the same distance from the first.
            span = min(len(got), len(exact))
            assert [t - got[0] for t in got[:span]] == \
                   [t - exact[0] for t in exact[:span]], (name, batch, age)
            # And nothing is dropped mid-stream: the same number of shots fit in
            # the time that remained after the first one.
            assert len(got) >= len([t for t in exact if t <= 400 - got[0]]) - 1, \
                (name, batch, age, len(got), len(exact))
for token in (
    "static bool weapon_state_step(const WeaponDef *weapon, u16 *tics, bool *trigger,",
    "*trigger = held;  // a tap starts one attack, not one per step",
    "*accurate = (bool)(weapon->accurate_first && (st->refire == 0));",
    "if (held && has_ammo) {",
    "st->timer = weapon->release_tics;",
    "const u16 fire_latch_tics = player_controller_consume_fire_latch_tics();",
    "weapon_tic_accumulator = (u16)(weapon_tic_accumulator +",
    "turn_to_melee_target(arsenal.current, &hit)",
    "#define SAW_TURN_STEP 3",
    "} else if (fire_result.pain) {",
):
    assert token in MAIN_C, token
assert "automatic" not in WEAPONS_H and "spread_cols" not in WEAPONS_H
assert "cooldown_vblanks" not in WEAPONS_H

# --- the demon: MT_SERGEANT, and MT_SHADOWS (the spectre), which is the same monster -----
# info.c: spawnhealth 150, radius 30, mass 400, painchance 180. S_SARG_ATK1..3 are
# 8 tics each and A_SargAttack is ATK3's action; S_SARG_PAIN is 2 + 2 tics.
SARG_ATTACK = [(8, "A_FaceTarget"), (8, "A_FaceTarget"), (8, "A_SargAttack")]
SARG_PAIN = [2, 2]
attack_tics = sum(tics for tics, _ in SARG_ATTACK)
bite_after = sum(tics for tics, _ in SARG_ATTACK[:2])
assert f"#define DEMON_ATTACK_TICS {attack_tics}" in ENEMY_C
assert f"#define DEMON_BITE_AT {attack_tics - bite_after}" in ENEMY_C
assert sum(SARG_PAIN) == 4 and "is_demon || (object->visual_id == BILLBOARD_VISUAL_IMP)" in COMBAT_C
assert "#define DOOM_RADIUS_DEMON 30" in INTERNAL_H
assert "#define DOOM_DEMON_HEALTH 150" in BILLBOARD_C
assert "case 3002: case 58: *visual = BILLBOARD_VISUAL_DEMON; return BILLBOARD_TYPE_DUMMY;" in BILLBOARD_C
assert "#define DOOM_DEMON_MASS_RATIO 4" in COMBAT_C          # mass 400 against 100
assert "damage = (u16)(((doom_random() % 10) + 1) * 4);" in ENEMY_C
assert {((p % 10) + 1) * 4 for p in range(256)} == set(range(4, 41, 4))
# A pain cancels the pending bite (P_SetMobjState leaves the attack states).
pain = COMBAT_C[COMBAT_C.index("static void roll_dummy_pain"):]
assert pain.index("object->bite_pending = 0;") < pain.index("const u8 pain_tics")
# The bite re-checks range and sight when it lands, not when the attack began.
demon_block = ENEMY_C[ENEMY_C.index("    if (is_demon) {"):]
assert (demon_block.index("if (object->bite_pending && (state->attack_anim <= DEMON_BITE_AT)) {") <
        demon_block.index("if (visible && demon_in_melee_range(player_dx, player_dy)) {") <
        demon_block.index("enemy_attack(object, player, update);"))

# P_CheckMeleeRange: P_AproxDistance in 16.16 against MELEERANGE - 20 + player radius.
assert "#define DEMON_MELEE_RANGE (64 - 20 + DOOM_RADIUS_PLAYER)" in ENEMY_C
for token in ("const s32 approx2 = 2 * (dx + dy) - ((dx < dy) ? dx : dy);",
              "return approx2 < 2 * DEMON_MELEE_RANGE;"):
    assert token in ENEMY_C, token


def doom_in_melee(dx, dy):
    fr = 1 << 16
    ax, ay = abs(dx) * fr, abs(dy) * fr
    dist = ax + ay - (min(ax, ay) >> 1)
    return not dist >= (64 - 20 + 16) * fr


def c_in_melee(dx, dy, variant=None):
    ax, ay = abs(dx), abs(dy)
    if variant == "floor-half":     # halving the integer instead of the 16.16
        return ax + ay - (min(ax, ay) >> 1) < 60
    if variant == "no-minus-20":    # MELEERANGE + player radius
        return 2 * (ax + ay) - min(ax, ay) < 2 * 80
    return 2 * (ax + ay) - min(ax, ay) < 2 * 60


melee_grid = [(x, y) for x in range(-100, 101) for y in range(-100, 101)]
assert all(c_in_melee(x, y) == doom_in_melee(x, y) for x, y in melee_grid)
for variant in ("floor-half", "no-minus-20"):
    wrong = sum(c_in_melee(x, y, variant) != doom_in_melee(x, y) for x, y in melee_grid)
    assert wrong > 0, f"negative control {variant!r} was not caught"

# The approach (demon_step_toward, x then y, each against the box as it stands):
# from anywhere around the player it must end inside the bite range without
# ever entering the player's box -- the player at the origin, standing still.
DEMON_GAP = 16 + 24
DEMON_STEP = 32
for token in ("#define DEMON_PLAYER_GAP (DOOM_RADIUS_PLAYER + ENEMY_RADIUS)",
              "if (magnitude > DUMMY_MOVE_STEP) magnitude = DUMMY_MOVE_STEP;",
              "if (abs_other < DEMON_PLAYER_GAP) {",
              "const s32 room = abs_delta - DEMON_PLAYER_GAP;",
              "step_x = demon_step_toward(player_dx, player_dy);",
              "step_y = demon_step_toward(player->y - object->y, player->x - object->x);"):
    assert token in ENEMY_C, token


def demon_step(delta, other, variant=None):
    magnitude = min(abs(delta), DEMON_STEP)
    if variant == "full-step":      # get_step_toward: always a whole step
        magnitude = DEMON_STEP if delta else 0
    if variant not in ("full-step", "no-box") and abs(other) < DEMON_GAP:
        room = abs(delta) - DEMON_GAP
        if room <= 0:
            return 0
        magnitude = min(magnitude, room)
    return magnitude if delta > 0 else -magnitude


def approach(x, y, variant=None):
    """(bites, entered_box). 'full-step' refuses a step into the box instead."""
    for _ in range(64):
        step_x = demon_step(-x, -y, variant)
        if variant == "full-step" and abs(x + step_x) < DEMON_GAP and abs(y) < DEMON_GAP:
            step_x = 0
        x += step_x
        step_y = demon_step(-y, -x, variant)
        if variant == "full-step" and abs(x) < DEMON_GAP and abs(y + step_y) < DEMON_GAP:
            step_y = 0
        y += step_y
        if abs(x) < DEMON_GAP and abs(y) < DEMON_GAP:
            return c_in_melee(x, y), True
        if step_x == 0 and step_y == 0:
            break
    return c_in_melee(x, y), False


starts = [(x, y) for x in range(-400, 401, 12) for y in range(-400, 401, 12)
          if not (abs(x) < DEMON_GAP and abs(y) < DEMON_GAP)]
for x, y in starts:
    bites, entered = approach(x, y)
    assert bites and not entered, (x, y)
for variant in ("full-step", "no-box"):
    broken = sum(1 for x, y in starts if approach(x, y, variant) != (True, False))
    assert broken > 0, f"negative control {variant!r} was not caught"

print(f"ok    hitscan: Doom box crossing matches a float reference on {compared} traces; "
      "monster aim matches Doom's hit share; weapon timelines match info.c tic for tic; "
      f"demon melee matches P_CheckMeleeRange on {len(melee_grid)} offsets and closes in "
      f"from {len(starts)} starts; 9 negative controls caught; P_DamageMobj, radii, HP and "
      "P_Random pinned")
