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

# --- source pins -----------------------------------------------------------------
assert "#define DOOM_SHOOT_RADIUS_MONSTER 20" in COMBAT_C
assert "#define DOOM_SHOOT_RADIUS_BARREL 10" in COMBAT_C
assert "BillboardFireResult billboard_fire_hitscan(" in COMBAT_C
# The old sprite-width test and the direction-blind barrel rescue are gone.
assert "aim_col < (measure.center_col - measure.half_w)" not in COMBAT_C
assert "POINT_BLANK" not in COMBAT_C and "POINT_BLANK" not in INTERNAL_H
# Mirrored expressions: the Python above must be what the C computes.
for token in (
    "const s16 dir_x = (s16)(cos_a - (s16)(((s32)spread_q12 * sin_a) >> 12));",
    "const s16 dir_y = (s16)(sin_a + (s16)(((s32)spread_q12 * cos_a) >> 12));",
    "const s32 dir_depth = (((s32)cos_a * dir_x) + ((s32)sin_a * dir_y)) >> 8;",
    "const s32 den = trace_positive ? ((s32)dir_x + dir_y) : ((s32)dir_x - dir_y);",
    "if (lateral >= radius * dir_extent) {",
    "const s32 num = trace_positive ? (dx + dy) : (dx - dy);",
    "if ((num == 0) || ((num < 0) != (den < 0))) {",
    "const s32 depth = ((s32)(s16)num * (s16)dir_depth) / den;",
    # A melee swing into open air leaves no puff.
    "if (wall_depth <= range_depth) {",
    "#define WORLD_TO_VIEW_DEPTH(units) ((u16)(((u32)(units) * 303u) >> 8))",
):
    assert token in COMBAT_C, token

# Doom spawnhealth (info.c): zombieman 20, shotgun guy 30, imp 60, barrel 20.
assert "#define DOOM_IMP_HEALTH 60" in BILLBOARD_C
assert "#define DOOM_SHOTGUN_GUY_HEALTH 30" in BILLBOARD_C
assert "if (bsp_things[i].type == 3001) {" in BILLBOARD_C
assert "} else if (bsp_things[i].type == 9) {" in BILLBOARD_C
assert re.search(r"\{BILLBOARD_VISUAL_BARREL,\s+BILLBOARD_EFFECT_NONE,\s+20,", BILLBOARD_C)
assert re.search(r"\{BILLBOARD_VISUAL_DUMMY,\s+BILLBOARD_EFFECT_NONE,\s+20,", BILLBOARD_C)

# Refire cycles: Doom's held-trigger loop in tics (info.c states, A_ReFire's
# own state skipped), converted at 12/7 vblanks per tic.
DOOM_CYCLE_TICS = {
    "FIST": 4 + 4 + 5 + 4,
    "CHAINSAW": 4,
    "PISTOL": 4 + 6 + 4,
    "SHOTGUN": 3 + 7 + 5 + 5 + 4 + 5 + 5 + 3,
    "CHAINGUN": 4,
}
rows = dict(re.findall(r"^    \[WEAPON_(\w+)\] = \{\n(.*?)\n    \},$", WEAPONS_C, re.S | re.M))
for name, tics in DOOM_CYCLE_TICS.items():
    fields = [f.strip() for f in rows[name].replace("\n", " ").split(",")]
    _ammo, _per, pellets, accurate, melee, cooldown, _flash = fields[:7]
    assert int(cooldown) == round(tics * 12 / 7), (name, cooldown, tics)
    # Doom's refire == 0 shots: the pistol's first, the chaingun's first two.
    assert int(accurate) == {"PISTOL": 1, "CHAINGUN": 2}.get(name, 0), (name, accurate)
    assert int(melee) == {"FIST": 64, "CHAINSAW": 65}.get(name, 0), (name, melee)
    assert int(pellets) == (7 if name == "SHOTGUN" else 1), (name, pellets)
assert "automatic" not in WEAPONS_H and "spread_cols" not in WEAPONS_H

# Damage and spread come from Doom's rndtable with Doom's formulas.
table = [int(v) for v in re.search(
    r"RNDTABLE\[256\] = \{(.*?)\};", WEAPONS_C, re.S).group(1).split(",")]
assert len(table) == 256
assert table[:8] == [0, 8, 109, 220, 222, 241, 149, 107] and table[-4:] == [120, 163, 236, 249]
assert sum(table) == 32986, sum(table)  # m_random.c's rndtable checksum
assert "return (u16)(2 * ((weapon_rng_next() % 10) + 1));" in WEAPONS_C
assert "return (u16)(5 * ((weapon_rng_next() % 3) + 1));" in WEAPONS_C
assert "return (s16)(((s32)d * 201) / 128);" in WEAPONS_C
# (P - P) << 18 in BAM is d * 2*pi / 2^14 rad; 201/128 in Q12 must match it.
for d in (1, 100, 255):
    want = math.tan(d * 2 * math.pi / (1 << 14)) * 4096
    got = (d * 201) / 128
    assert abs(got - want) / want < 0.006, (d, got, want)
# Deterministic: the index restarts every level, like M_ClearRandom.
assert "weapon_rng_reset();" in MAIN_C
assert "rand(" not in WEAPONS_C

# Main-loop wiring: held-trigger refire, overrun carry, melee turn-to-face.
for token in (
    "(bool)(burst_shots < weapon->accurate_shots)",
    "shot_overrun = (u16)(elapsed_vblanks - shot_cooldown);",
    "#define MAX_SHOTS_PER_ITERATION 2",
    "turn_to_melee_target(arsenal.current, &hit)",
    "#define SAW_TURN_STEP 3",
    "} else if (fire_result.pain) {",
):
    assert token in MAIN_C, token

print(f"ok    hitscan: Doom box crossing matches a float reference on {compared} traces; "
      "3 negative controls caught; radii, HP, refire and P_Random pinned")
