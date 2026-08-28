#!/usr/bin/env python3
"""Contracts for Doom-style weapon bob (player_controller.c update_weapon_bob).

The bob is a *visual* derivative of the same 35 Hz momentum that drives
gameplay, so this test rebuilds that momentum the way the controller does
(thrust -> clamp -> friction/stop) and then rebuilds the bob offset the way
update_weapon_bob does, and asserts the behaviour the implementation plan
requires: stationary -> (0, 0), direction symmetry, hard clamp, run >= walk,
friction-decay tail after input release, and the 2:1 vertical:horizontal
frequency relationship.

It also pins the source tokens so the feature cannot be silently rewired to
read D-pad state or advance phase off the render frame.
"""

from pathlib import Path
import math
import re

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/player_controller.c").read_text()
HEADER = (ROOT / "src/player_controller.h").read_text()

# --- Constants, mirrored from the C --------------------------------------
FX_SHIFT = 8
ANGLE_STEPS = 256
ANGLE_MASK = ANGLE_STEPS - 1
ANGLE_90 = ANGLE_STEPS // 4

THRUST_X8 = 8            # s_momentum_* += thrust * 8
MAX_MOVE = 30 << 16
STOP_SPEED = 0x1000
DOOM_FORWARD_WALK = 25
DOOM_FORWARD_RUN = 50


def friction(v: int) -> int:
    return v - ((v * 3) >> 5)


def cdefine(name: str) -> int:
    m = re.search(rf"#define {name}\s+(\(?[^\n/]+?\)?)\s*(?://|$)", SOURCE, re.M)
    assert m, f"{name} missing from player_controller.c"
    return eval(m.group(1), {"ANGLE_STEPS": ANGLE_STEPS})


BOB_MOMENTUM_SHIFT = cdefine("BOB_MOMENTUM_SHIFT")
BOB_MAG_MAX = cdefine("BOB_MAG_MAX")
BOB_X_NUM = cdefine("BOB_X_NUM")
BOB_Y_NUM = cdefine("BOB_Y_NUM")
BOB_TRIG_SHIFT = cdefine("BOB_TRIG_SHIFT")
BOB_MAX_X = cdefine("BOB_MAX_X")
BOB_MAX_Y = cdefine("BOB_MAX_Y")
BOB_TICS_PER_CYCLE = cdefine("BOB_TICS_PER_CYCLE")
BOB_PHASE_STEP = (ANGLE_STEPS + BOB_TICS_PER_CYCLE // 2) // BOB_TICS_PER_CYCLE


# --- fx_sin / fx_cos, mirrored from src/fixed_math.c --------------------
def _sin_quarter_q8(angle: int) -> int:
    a = angle & (ANGLE_90 - 1)
    x = (a * (1 << FX_SHIFT)) // ANGLE_90
    x2 = (x * x) >> FX_SHIFT
    x3 = (x2 * x) >> FX_SHIFT
    x5 = (x3 * x2) >> FX_SHIFT
    return (479 * x - 196 * x3 + 24 * x5) >> FX_SHIFT


def _build_sin_table():
    table = []
    for i in range(ANGLE_STEPS):
        quadrant = i // ANGLE_90
        local = i & (ANGLE_90 - 1)
        if quadrant == 0:
            v = _sin_quarter_q8(local)
        elif quadrant == 1:
            v = _sin_quarter_q8(ANGLE_90 - 1 - local)
        elif quadrant == 2:
            v = -_sin_quarter_q8(local)
        else:
            v = -_sin_quarter_q8(ANGLE_90 - 1 - local)
        table.append(v)
    return table


_SIN = _build_sin_table()
fx_sin = lambda angle: _SIN[angle & ANGLE_MASK]
fx_cos = lambda angle: _SIN[(angle + ANGLE_90) & ANGLE_MASK]


# --- update_weapon_bob, mirrored from src/player_controller.c ----------
BOB_MOVE_GRACE_TICS = cdefine("BOB_MOVE_GRACE_TICS")


class Bob:
    def __init__(self):
        self.phase = 0
        self.x = 0
        self.y = 0
        self.grace = 0

    def update(self, momentum_x: int, momentum_y: int, moved: bool = True):
        if moved:
            self.grace = BOB_MOVE_GRACE_TICS
        elif self.grace > 0:
            self.grace -= 1

        mx = abs(momentum_x >> BOB_MOMENTUM_SHIFT)
        my = abs(momentum_y >> BOB_MOMENTUM_SHIFT)
        hi, lo = max(mx, my), min(mx, my)
        mag = hi + (lo >> 1)
        if mag > BOB_MAG_MAX:
            mag = BOB_MAG_MAX

        if mag == 0 or self.grace == 0:
            # Ease back to neutral 1px/tic (stopped, or shoving into a wall).
            if self.x > 0:
                self.x -= 1
            elif self.x < 0:
                self.x += 1
            if self.y > 0:
                self.y -= 1
            if self.x == 0 and self.y == 0:
                self.phase = 0
            return

        self.phase = (self.phase + BOB_PHASE_STEP) & ANGLE_MASK

        bob_x = (mag * fx_cos(self.phase) * BOB_X_NUM) >> BOB_TRIG_SHIFT
        bob_x = max(-BOB_MAX_X, min(BOB_MAX_X, bob_x))

        vwave = abs(fx_sin(self.phase & (ANGLE_STEPS // 2 - 1)))
        bob_y = (mag * vwave * BOB_Y_NUM) >> BOB_TRIG_SHIFT
        bob_y = min(BOB_MAX_Y, bob_y)

        self.x, self.y = bob_x, bob_y


def clamp_move(v: int) -> int:
    return max(-MAX_MOVE, min(MAX_MOVE, v))


# fx_cos(0) / fx_sin(ANGLE_90) reproduce the controller's dir_x/dir_y for a
# player facing along +x / +y. Forward-only movement then puts all the thrust on
# one axis, exactly like simulate_doom_movement_tic.
DIR = fx_cos(0)


def to_s16(v: int) -> int:
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def simulate(tics, forward, heading_x=True, release_at=None):
    """Return per-tic (bob_x, bob_y, momentum_abs) for `forward` command applied
    along one axis, optionally released (command -> 0) after `release_at` tics."""
    mx = my = 0
    rem_x = rem_y = 0
    bob = Bob()
    out = []

    def consume(total):
        whole = (total >> 16) if total >= 0 else -((-total) >> 16)
        rem = (total & 0xFFFF) if total >= 0 else -((-total) & 0xFFFF)
        return whole, rem

    for t in range(tics):
        cmd = forward if (release_at is None or t < release_at) else 0
        thrust = to_s16(DIR * cmd)
        if heading_x:
            mx = clamp_move(mx + thrust * THRUST_X8)
        else:
            my = clamp_move(my + thrust * THRUST_X8)
        # Position consume runs on the post-thrust momentum, before friction, and
        # decides `moved` for the bob (no walls in this sim -> free movement).
        whole_x, rem_x = consume(mx + rem_x)
        whole_y, rem_y = consume(my + rem_y)
        moved = (whole_x != 0) or (whole_y != 0)
        if cmd == 0 and abs(mx) < STOP_SPEED and abs(my) < STOP_SPEED:
            mx = my = 0
            rem_x = rem_y = 0
        else:
            mx, my = friction(mx), friction(my)
        bob.update(mx, my, moved)
        out.append((bob.x, bob.y, abs(mx) + abs(my)))
    return out


def main():
    # --- Source wiring: momentum-driven, tic-driven, exposed read-only ----
    assert "s16 player_controller_weapon_bob_x(void);" in HEADER
    assert "s16 player_controller_weapon_bob_y(void);" in HEADER

    tic_fn = re.search(r"static bool simulate_doom_movement_tic\(.*?\n\}", SOURCE, re.S).group(0)
    assert "update_weapon_bob(moved);" in tic_fn, "bob must advance on the 35 Hz tic"
    assert re.search(r"moved\s*=\s*\(player->x != old_x\)\s*\|\|\s*\(player->y != old_y\)",
                     tic_fn), "bob must be told whether the player actually translated"

    bob_fn = re.search(r"static void update_weapon_bob\(bool moved\) \{.*?\n\}", SOURCE, re.S).group(0)
    assert "s_momentum_x" in bob_fn and "s_momentum_y" in bob_fn
    assert "joy" not in bob_fn and "forward" not in bob_fn, "bob must not read input"
    assert "s_weapon_bob_move_grace" in bob_fn, "bob must ease off when not translating"
    assert "fx_cos(s_weapon_bob_phase)" in bob_fn
    # No 64-bit helper: the products stay word-sized via player_muls_word.
    assert "player_muls_word" in bob_fn

    reset_fn = re.search(r"void player_controller_reset\(void\) \{.*?\n\}", SOURCE, re.S).group(0)
    assert "s_weapon_bob_phase = 0;" in reset_fn

    # --- Phase 2 renderer wiring: BG_A plane-scroll bob, window-plane HUD ----
    # The controller signals "bob moved but the player may not have crossed a
    # whole world pixel" so a cheap weapon-overlay frame still re-applies scroll.
    assert "#define PLAYER_CONTROL_WEAPON_BOB" in HEADER
    upd_fn = re.search(r"u16 player_controller_update\(.*?\n\}", SOURCE, re.S).group(0)
    assert "PLAYER_CONTROL_WEAPON_BOB" in upd_fn
    assert ("s_weapon_bob_x != bob_x_before" in upd_fn
            and "s_weapon_bob_y != bob_y_before" in upd_fn)

    scene = (ROOT / "src/renderer/renderer_scene.c").read_text()
    scene_flat = re.sub(r"\s+", " ", scene)
    assert ("renderer_apply_weapon_bob(player_controller_weapon_bob_x()" in scene_flat
            and "player_controller_weapon_bob_y()" in scene_flat), \
        "scene must push the controller's bob offset each frame"

    overlay = (ROOT / "src/renderer/renderer_frame_overlay.c").read_text()
    bob_apply = re.search(r"void renderer_apply_weapon_bob\(.*?\n\}", overlay, re.S).group(0)
    assert "VDP_setHorizontalScroll(BG_A" in bob_apply
    assert "VDP_setVerticalScroll(BG_A" in bob_apply

    hud = (ROOT / "src/renderer/renderer_hud.c").read_text()
    num_fn = re.search(r"static void draw_hud_number_tilemap\(void\) \{.*?\n\}", hud, re.S).group(0)
    assert "VDP_setTileMapXY(WINDOW," in num_fn, "status numbers must ride the window plane"
    assert "VDP_setTileMapXY(BG_A," not in num_fn, "status numbers must not stay on BG_A"
    assert "VDP_setWindowOnBottom(HUD_PANEL_H)" in hud

    mainc = (ROOT / "src/main.c").read_text()
    assert "renderer_hud_window_suspend();" in mainc, "pause must drop the window plane"
    assert "PLAYER_CONTROL_WEAPON_BOB" in mainc

    # --- Stationary -> (0, 0), forever -----------------------------------
    assert all(s[:2] == (0, 0) for s in simulate(180, 0))

    # --- Walking: bounded, periodic, non-trivial ------------------------
    walk = simulate(960, DOOM_FORWARD_WALK)
    wx = [s[0] for s in walk]
    wy = [s[1] for s in walk]
    assert all(-BOB_MAX_X <= v <= BOB_MAX_X for v in wx)
    assert all(0 <= v <= BOB_MAX_Y for v in wy)
    assert max(wx) > 0 and min(wx) < 0, "horizontal bob must swing both ways"
    # The vertical dip is downward-only (Doom's positive lobe): >= 0 always, and
    # it does move the gun. The renderer parks the 3D view flush against the
    # status bar so the dip runs into the WINDOW/HUD region (plane A suppressed),
    # never past a visible frame edge.
    assert min(wy) >= 0, "vertical dip must never lift the gun (would float it)"
    assert max(wy) > 0, "vertical bob must dip the weapon"
    view_y = int(re.search(r"#define VIEW_TILEMAP_Y (\d+)",
                           (ROOT / "src/renderer/renderer_internal.h").read_text()).group(1))
    hud_h = int(re.search(r"#define FREEDOOM_HUD_TILE_H (\d+)",
                          (ROOT / "src/renderer/generated_hud_assets.h").read_text()).group(1))
    ray_h = int(re.search(r"#define RAY_VIEW_TILE_H (\d+)",
                          (ROOT / "src/raycast.h").read_text()).group(1))
    assert view_y + ray_h == 28 - hud_h, "3D view must sit flush on the status bar"

    # Periodicity: with settled momentum the phase steps by BOB_PHASE_STEP mod
    # ANGLE_STEPS each tic, so the exact repeat period is ANGLE_STEPS/gcd.
    steady = [s[:2] for s in walk[300:]]
    period = ANGLE_STEPS // math.gcd(BOB_PHASE_STEP, ANGLE_STEPS)
    assert len(steady) > 2 * period
    assert all(steady[i] == steady[i + period]
               for i in range(len(steady) - period)), "bob cycle must repeat"
    # No shorter period (the bob genuinely uses the whole cycle).
    assert not any(all(steady[i] == steady[i + p] for i in range(period))
                   for p in range(1, period) if period % p == 0)

    # Vertical frequency is twice the horizontal. Measure on the raw trig of the
    # phase (before pixel quantisation, which staircases the small-integer
    # output): count horizontal sign changes vs vertical zero-touches over one
    # full phase period.
    raw_cos, raw_fold = [], []
    ph = 0
    for _ in range(period):
        ph = (ph + BOB_PHASE_STEP) & ANGLE_MASK
        raw_cos.append(fx_cos(ph))
        raw_fold.append(fx_sin(ph & (ANGLE_STEPS // 2 - 1)))
    def maxima(seq, floor):
        n = len(seq)
        return sum(1 for i in range(n)
                   if seq[i] > floor and seq[i] >= seq[(i - 1) % n]
                   and seq[i] > seq[(i + 1) % n])
    h_humps = maxima(raw_cos, 100)
    v_humps = maxima(raw_fold, 100)
    assert h_humps == 13, h_humps                 # 13 cosine cycles over the period
    assert v_humps == 2 * h_humps, (v_humps, h_humps)
    assert min(raw_fold) >= 0, "vertical wave must stay on the positive lobe"

    # --- Direction symmetry --------------------------------------------
    walk_x_axis = simulate(240, DOOM_FORWARD_WALK, heading_x=True)
    walk_y_axis = simulate(240, DOOM_FORWARD_WALK, heading_x=False)
    assert (max(abs(s[0]) for s in walk_x_axis) == max(abs(s[0]) for s in walk_y_axis))
    assert (max(s[1] for s in walk_x_axis) == max(s[1] for s in walk_y_axis))

    # --- Running amplitude >= walking --------------------------------
    run = simulate(240, DOOM_FORWARD_RUN)
    assert max(abs(s[0]) for s in run) >= max(abs(s[0]) for s in walk)
    assert max(s[1] for s in run) >= max(s[1] for s in walk)
    # ... and strictly bigger somewhere, not just an equal clamp.
    assert max(s[1] for s in run) > max(s[1] for s in walk) or \
           max(abs(s[0]) for s in run) > max(abs(s[0]) for s in walk)

    # --- Hard clamp at the maximum allowed momentum -----------------
    bob = Bob()
    worst = 0
    for _ in range(64):
        bob.update(MAX_MOVE, MAX_MOVE, moved=True)
        worst = max(worst, abs(bob.x), bob.y)
    assert worst <= max(BOB_MAX_X, BOB_MAX_Y)
    assert all(abs(bob.x) <= BOB_MAX_X and 0 <= bob.y <= BOB_MAX_Y for bob in [bob])

    # --- Shoving into a wall: momentum stays pinned but the player stops
    #     translating, so the bob must ease off instead of racing on the spot ---
    bob = Bob()
    for _ in range(30):                       # walk up to the wall
        bob.update(MAX_MOVE, 0, moved=True)
    assert abs(bob.x) > 0
    seen = []
    for _ in range(40):                       # now blocked: momentum high, moved=False
        bob.update(MAX_MOVE, 0, moved=False)
        seen.append((bob.x, bob.y))
    assert seen[-1] == (0, 0), "bob must settle to neutral while pinned on a wall"
    # Settles quickly: at most the grace window plus a 1px/tic ease from the clamp.
    settle = next(i for i, v in enumerate(seen) if v == (0, 0) and all(
        s == (0, 0) for s in seen[i:]))
    assert settle <= BOB_MOVE_GRACE_TICS + max(BOB_MAX_X, BOB_MAX_Y) + 1, settle
    # Once the grace window closes the ease is strictly toward neutral.
    xs = [abs(x) for x, _ in seen[BOB_MOVE_GRACE_TICS:]]
    assert all(xs[i] >= xs[i + 1] for i in range(len(xs) - 1)), xs

    # --- Friction decay: releasing input does not snap the bob to zero --
    released = simulate(400, DOOM_FORWARD_RUN, release_at=120)
    at_release = max(abs(v[0]) for v in released[110:120])
    assert at_release > 0
    # The bob keeps moving for several tics after release while momentum bleeds.
    post = released[120:]
    moving_after = sum(1 for v in post[:20] if (v[0], v[1]) != (0, 0))
    assert moving_after >= 5, moving_after
    # ... and it does reach neutral once momentum is fully gone.
    assert released[-1][:2] == (0, 0)
    # Amplitude envelope is monotonically non-increasing after release (friction
    # only ever removes momentum).
    env = [max(abs(v[0]) for v in post[i:i + period]) for i in range(0, len(post) - period, period)]
    assert all(env[i] >= env[i + 1] for i in range(len(env) - 1)), env

    print(f"ok    weapon bob: stationary->0, clamp +/-{BOB_MAX_X}/{BOB_MAX_Y}px, "
          f"2:1 v:h, friction tail, phase {BOB_PHASE_STEP}u/tic")


if __name__ == "__main__":
    main()
