# Doom-Style Weapon Bob

## Status

**Shipped** (initial implementation 2026-08-27, tuned through 2026-09). This
document started as a plan to move the weapon onto hardware sprites. That route
was not taken. The weapon stays a BG_A tilemap and the bob is a **whole-plane
BG_A hardware scroll**. The document now records the design as built. The sprite
route is kept in [section 9](#9-rejected-alternative-hardware-sprite-weapon) as a
rejected alternative, with the reasons.

Code map:

| Concern | Location |
| --- | --- |
| Bob simulation (35 Hz) | `update_weapon_bob()` in `src/player_controller.c`; tuning `#define BOB_*` at the top of the file |
| Read-only interface | `player_controller_weapon_bob_x/y()` in `src/player_controller.h` |
| Redraw trigger | `PLAYER_CONTROL_WEAPON_BOB` (`player_controller.h`), consumed in `src/main.c` |
| Presentation | `renderer_apply_weapon_bob()` in `src/renderer/renderer_frame_overlay.c`, called from `renderer_scene.c` |
| HUD numbers / keys on WINDOW plane | `renderer_hud_window_setup()` in `src/renderer/renderer_hud.c` |
| Neutral reset | `frame_overlay_reset()`, `renderer_automap_weapon_visibility()` |
| Contracts | `tools/test-weapon-bob.py`, `tools/test-hud-layout.py`, `tools/test-weapons.py` |

## Goal

The weapon and hands move like Doom's while the player walks or runs. The motion
comes from the momentum the physics already uses, not from raw input:

- the amplitude comes from player momentum;
- after the player releases input, the bob continues through the friction tail
  and does not stop at once;
- the horizontal motion is a periodic left/right arc;
- the vertical motion runs at twice the horizontal frequency;
- the phase advances on Doom's 35 Hz game tics, not on renderer frames;
- the weapon returns to neutral when the player stops translating.

## Non-Goals

- camera/view-height bob (see [section 10](#10-follow-up-camera-bob));
- recoil camera shake;
- per-weapon bob curves;
- sway from turning alone;
- any change to damage, fire timing, ammo, or movement physics constants.

---

# 1. Timing Model

`src/player_controller.c` keeps fixed-point momentum (`s_momentum_x`,
`s_momentum_y`) and runs Doom movement tics at 35 Hz through
`s_doom_tic_accumulator`. This is separate from the frame cadence. The code
applies Doom-style thrust, momentum clamping, and friction: `DOOM_FRICTION` is
29/32, computed without a 64-bit helper.

```text
Mega Drive VDP       60 Hz
renderer             variable (idle 30 fps, motion ~10 vblanks/frame)
movement simulation  35 Hz
weapon bob phase     35 Hz (update_weapon_bob runs once per movement tic)
```

The renderer only reads the latest offset. It never looks at momentum, input,
or timing constants.

# 2. Simulation (`update_weapon_bob`)

State, all reset in `player_controller_reset()`:

```c
static u16 s_weapon_bob_phase;
static s16 s_weapon_bob_x;
static s16 s_weapon_bob_y;
static u8  s_weapon_bob_move_grace;
```

Each movement tic calls `update_weapon_bob(moved)`. The flag `moved` says
whether that tic moved the player by at least one whole world pixel.

## 2.1 Magnitude

Doom squares the momentum. That would need 32-bit multiplies here, so the code
uses an **octagonal norm** on shifted-down momentum instead:

```text
mx, my = |momentum| >> BOB_MOMENTUM_SHIFT (12)
mag    = max(mx, my) + min(mx, my) / 2,  clamped to BOB_MAG_MAX (360, ~running speed)
```

The norm is close to isotropic, so strafing and diagonal movement bob about as
much as walking forward. The whole calculation stays in word-sized operations.

## 2.2 Movement grace and settling

If the player pushes into a wall, momentum stays high but the player does not
move. A pure momentum-driven bob would keep cycling in place. To prevent this:

- a tic that moves the player refills `s_weapon_bob_move_grace` to
  `BOB_MOVE_GRACE_TICS` (4). The grace bridges the sub-pixel gaps of a slow
  walk;
- if `mag == 0` or the grace runs out, the phase stops advancing. `x` and `y`
  move 1 px per tic toward zero instead. When both reach zero, the phase resets
  to 0.

On release, momentum decays through friction. The bob keeps cycling for as long
as that tail still moves the player whole pixels, then eases back to neutral.

## 2.3 Waveform

```text
phase += BOB_PHASE_STEP                  (one cycle per BOB_TICS_PER_CYCLE = 20 tics, ~0.57 s)
x = clamp((mag * fx_cos(phase)            * BOB_X_NUM) >> BOB_TRIG_SHIFT, ±BOB_MAX_X)
y = clamp((mag * |fx_sin(phase mod π)|    * BOB_Y_NUM) >> BOB_TRIG_SHIFT, 0..BOB_MAX_Y)
```

- Folding the phase onto a half circle gives the positive sine lobe twice per
  horizontal cycle. That is the 2× vertical frequency, and `y` is always ≥ 0.
- `mag * fx_*` fits `muls.w` (`player_muls_word`). The small `*_NUM`
  multiplier becomes shifts and adds.
- The code uses the existing `fx_sin`/`fx_cos` tables. No new lookup table was
  added.
- Current limits: `BOB_MAX_X = 10` (±10 px), `BOB_MAX_Y = 10` (0..10 px, a
  downward dip only). These were widened from the original ±3 / 0..4 targets
  after play tests.

## 2.4 Redraw trigger

On a tic that moves the player less than a whole pixel, the bob can still
change, for example while settling. `player_controller_update()` compares the
offset before and after the tic loop. If it changed, it sets
`PLAYER_CONTROL_WEAPON_BOB`. `main.c` turns that into a
`RENDERER_REDRAW_WEAPON` overlay request, which never runs a cast. The request
is skipped while the automap is open.

# 3. Presentation: BG_A Plane Scroll

## 3.1 Plane layout

```text
BG_B    -> 3D view, HUD backdrop/face (unchanged)
BG_A    -> weapon tilemap ONLY, whole-plane scrolled by the bob
WINDOW  -> status-bar number fields + key-card icons, plus the black gutter under the view
Sprites -> unused during gameplay
```

Before the bob, BG_A also held the four status-bar number fields. Scrolling the
plane would have moved them too, so they moved to the WINDOW plane. The key-card
icons were added there later.

## 3.2 `renderer_apply_weapon_bob(x, y)`

```c
VDP_setHorizontalScroll(BG_A, bob_x);     // +H moves the plane right
VDP_setVerticalScroll(BG_A, -bob_y);      // -V moves the plane down (dip)
```

- Each call is two register writes: pixel-precise, no tile DMA, no tilemap
  rewrite.
- A last-applied cache (`s_bob_applied_x/y`) skips the writes when the offset
  has not changed.
- `renderer_render_scene()` calls it every scene frame, so it is idempotent.
- `frame_overlay_reset()` writes `0, 0` without checking the cache and primes
  the cache, because a level load, scene invalidation, or menu return may have
  left BG_A scrolled.
- `renderer_automap_weapon_visibility()` sets the offset to neutral before it
  hides or restores the weapon.

The idle/fire tilemap path (`draw_weapon_overlay`) is unchanged. It still
rewrites the 8×5-tile rectangle only when the variant changes. Firing does not
reset or desynchronise the phase, because the frame and the position are
independent.

## 3.3 Why the dip is never cut off visibly

The weapon tilemap sits at the bottom of the 3D view
(`WEAPON_TILEMAP_Y = VIEW_TILEMAP_Y + VIEW_TILE_H - WEAPON_TILE_H`). The WINDOW
plane is pinned from `VIEW_WINDOW_TOP_Y`, the row right below the view, down to
the bottom of the screen (`VDP_setWindowOnBottom(VIEW_WINDOW_TILE_H)`). Plane A
is hidden over the whole window region, whatever the tile content. When the gun
dips, its bottom rows slide under that region and are clipped exactly at the
view's bottom edge. The gutter cells of the window are transparent tile 0, so
the letterbox still looks plain black.

This is why `bob_y` is never negative. Lifting the gun would leave its base
floating over the floor.

This rule once required the view to sit flush on the status bar
(`VIEW_TILEMAP_Y 9`). Pinning the window from `VIEW_WINDOW_TOP_Y` removed that
requirement, and the view is centred again.

## 3.4 Other BG_A consumers

Anything drawn on BG_A moves with the bob. The `DEBUG_PERF` overlay, including
`VDP_showFPS`/`VDP_showCPULoad`, which draw on the text plane (= BG_A), was
moved off BG_A for this reason (see `LOG.md`, 2026-08-29). Do not add new
gameplay-time content to BG_A. Put static overlays on the WINDOW plane or BG_B.

Pause and menu screens draw full-screen BG_A images. `renderer_hud_window_suspend()`
turns the window off while they are shown. When the game resumes,
`renderer_restore_after_menu()` restores everything:

- `init_hud_tiles()` → `reload_weapon_tiles()` restores the weapon tiles;
- `renderer_invalidate_scene()` → `frame_overlay_reset()` resets the scroll;
- `renderer_draw_static_screen()` → `renderer_hud_window_setup()` turns the
  window back on.

# 4. Weapon Switching

`renderer_set_weapon()` DMAs the new weapon's tileset into the shared weapon
VRAM window and clears the variant cache. The bob state lives in the controller,
so switching weapons does not affect the phase or the offset. The new weapon
appears at the current bob position on the next frame.

# 5. VRAM

The layout is unchanged by the bob. The weapon streaming window starts at
`WEAPON_TILE_BASE` and must end before `HUD_VRAM_SAFE_TILE_LIMIT` (1440, the SGDK
font).

`#error` in `renderer.c` and `tools/test-weapons.py` enforce this. With the
current defines, `WEAPON_TILE_BASE` = 16 + 704 (view banks at 22×16 × 2) + 256
(pairs) + 160 (HUD) + 16 (face window) + 78 (numbers) + 4 (keys) = **1234**. That
leaves a 206-tile window. The largest weapon uses 69 tiles
(`MEGALDOOM_WEAPON_TILE_COUNTS = 42, 48, 48, 45, 69`).

Treat these numbers as a snapshot. The defines are authoritative.

# 6. Performance

- Simulation: a few shifts, two `muls.w`, and two table lookups per 35 Hz tic.
  No 64-bit math, division, or floating point.
- Presentation: at most two VDP register writes per frame, and none when the
  offset has not changed.
- A bob-only frame (`PLAYER_CONTROL_WEAPON_BOB` without
  `PLAYER_CONTROL_CHANGED`) is an overlay frame. It does not recast the view.
- The weapon uses no graphics DMA because of the bob. DMA happens only on idle/fire
  changes and weapon switches, the same as before.

# 7. Tests

`tools/test-weapon-bob.py` mirrors `update_weapon_bob` in Python and checks:

- a stationary player gives `(0, 0)`;
- equal speeds in different directions give equal amplitude;
- the clamp holds at maximum momentum;
- running produces at least as much bob as walking;
- the bob continues through the friction decay after input is released;
- the pattern is periodic, with the vertical at 2× the horizontal frequency;
- the grace/settle path works when the player is pinned against a wall;
- the window mask is deeper than `BOB_MAX_Y` at every view size.

The script also checks the wiring contracts: the getters exist, the bob runs
inside the tic loop, the phase resets, the `PLAYER_CONTROL_WEAPON_BOB`
comparison is present, and the scene calls `renderer_apply_weapon_bob` with the
controller getters. Because the Python copy is a mirror, **update it whenever
`update_weapon_bob` changes**.

`tools/test-hud-layout.py` covers the WINDOW-plane pinning.
`tools/test-weapons.py` covers the VRAM window.

The shape of the motion was judged in play tests. Static screenshots do not show
it. Any retuning needs someone to look at real motion in an emulator.

# 8. Tuning

Only the visual conversion constants in `player_controller.c` may be tuned.
Never change physics to change the bob.

| Constant | Effect |
| --- | --- |
| `BOB_MOMENTUM_SHIFT` | momentum → magnitude scale |
| `BOB_MAG_MAX` | speed where the amplitude saturates |
| `BOB_X_NUM` / `BOB_Y_NUM` / `BOB_TRIG_SHIFT` | amplitude per unit of magnitude |
| `BOB_MAX_X` / `BOB_MAX_Y` | hard pixel limits |
| `BOB_TICS_PER_CYCLE` | cycle length (drives `BOB_PHASE_STEP`) |
| `BOB_MOVE_GRACE_TICS` | how long a sub-pixel walk keeps bobbing |

Before raising `BOB_MAX_Y`, make sure the dip stays inside the window-clipped
region (section 3.3). `tools/test-weapon-bob.py` checks this for every view
size in the OPTIONS menu. Before raising `BOB_MAX_X`, make sure the weapon's
side edges stay inside the 3D view at the smallest view size. BG_A scrolls as a
whole plane, and no mask clips it horizontally.

---

# 9. Rejected Alternative: Hardware-Sprite Weapon

The original plan was to move the weapon to hardware sprites and position the
pieces at base + bob. This would have required generated sprite-piece metadata,
SAT management, and a build flag before removing the tilemap path. Its stated
reason was that moving a BG tilemap rectangle snaps to 8 px. That applies to
rewriting the rectangle at a new cell position. It does not apply to the plane
scroll registers.

When the plan was written, it was rejected for these reasons:

1. **VRAM.** The weapon window was 72 tiles at the time (`WEAPON_TILE_BASE`
   1368). The largest weapon used 69 tiles, and only because the tilemap path
   uses per-cell transparent indirection (`0xFFFF` → tile 0) and shares tiles
   between the idle and fire frames. A sprite-piece layout keeps neither
   saving, so the large weapons would not have fitted without cropping the art
   or dropping fire frames.
2. **Cost and risk.** It needed a new generator stage, SAT and scanline budgeting,
   and new pause/restore and switching paths. The scroll route needed two
   register writes and moving the HUD numbers to WINDOW.

Reason 1 no longer holds as strongly. The face streaming window and resized view
banks moved `WEAPON_TILE_BASE` to 1234, so the window is now 206 tiles (section 5).
Reason 2 still holds. The scroll route also clips the dip for free, which
sprites could not do without masking. Reconsider sprites only if a feature needs
the weapon to move independently of the rest of BG_A. One example is
per-weapon offsets that differ from the bob.

# 10. Follow-Up: Camera Bob

This is not implemented. Doom also changes the view height using the same bob
state. It is more invasive because it changes the world projection rather than
an overlay. A future implementation should reuse the `mag` and phase from
`update_weapon_bob` rather than compute a second speed value.

# 11. Definition of Done — Status

- [x] bob derived from existing Doom-style momentum
- [x] phase advances with the 35 Hz simulation
- [x] horizontal arc with 2× vertical frequency
- [x] returns to neutral when stopped, and when pinned against a wall
- [x] continues through the friction decay after release
- [x] walking and running amplitudes differ visibly (play-tested)
- [x] pixel-precise movement (BG_A scroll, no 8 px snapping)
- [x] all weapons and fire frames work; switching while moving keeps the phase
- [x] pause/menu and automap restore to neutral
- [x] HUD unaffected (numbers and keys on WINDOW)
- [x] no weapon graphics DMA from bob position changes
- [x] no 64-bit or floating-point hot-path dependency
- [x] VRAM ranges guarded at compile time and by test
- [x] automated bob contracts pass (`tools/test-weapon-bob.py`)
- [x] movement physics constants unchanged
- [ ] ~~weapon no longer on a BG_A tilemap~~ — superseded: it is still a BG_A
      tilemap, scrolled as a whole plane (section 9)
- [ ] ~~sprite and scanline limits validated~~ — not applicable (no weapon sprites)
