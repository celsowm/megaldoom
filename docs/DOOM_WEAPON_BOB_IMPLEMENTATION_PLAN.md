# Doom-Style Weapon Bob Implementation Plan

## Status

Proposed implementation plan for reproducing the original Doom weapon/hand bob behavior in MegaLDOOM while respecting Mega Drive VDP constraints and the project's existing 35 Hz movement simulation.

## Implementation Note (2026-08-27)

Phase 1 (bob simulation state in `player_controller.c`) shipped as written.

Phases 2-6 (hardware-sprite weapon renderer) were **not** taken. Investigation
found: (a) gameplay uses zero hardware sprites, so the SAT is free, but (b) the
weapon VRAM window is only 72 tiles (`WEAPON_TILE_BASE 1368` ->
`HUD_VRAM_SAFE_TILE_LIMIT 1440`) and the largest weapon already needs 69 with the
tilemap path's per-cell transparent indirection and idle/fire dedup, neither of
which a sprite-piece layout keeps — the big weapons would not fit without
cropping the art or dropping fire-frame sprites.

Instead the bob is applied as a **whole-plane BG_A hardware scroll**. The plan's
only stated objection to "moving the tilemap" was 8-pixel snapping, which does
not apply to the scroll registers (pixel precision, two VDP writes, no DMA). The
one structural change: BG_A also held the four status-bar number fields, so those
moved to the WINDOW plane (pinned over the bottom `HUD_PANEL_H` rows) and BG_A
now carries nothing but the weapon. See `renderer_frame_overlay.c`
(`renderer_apply_weapon_bob`), `renderer_hud.c` (`renderer_hud_window_setup`) and
`PLAYER_CONTROL_WEAPON_BOB` in `player_controller.c` / `main.c`.

The Definition of Done still holds except "the weapon no longer depends on a
BG_A tilemap rectangle" — it does, it is just scrolled rather than repositioned
cell-by-cell.

## Goal

Implement Doom-style first-person weapon bob in MegaLDOOM so that the weapon and the Doomguy's hands move naturally while the player walks or runs, with motion driven by the same momentum used by gameplay physics rather than by raw input state.

The implementation should preserve the characteristic behavior of Doom:

- bob amplitude derives from player momentum;
- stopping input does not instantly stop the weapon because momentum decays through friction;
- horizontal motion follows a periodic left/right arc;
- vertical motion runs at twice the horizontal frequency, creating the familiar Doom weapon path;
- bob timing follows Doom's 35 Hz game-tic simulation rather than the renderer frame rate;
- the weapon returns to its neutral position when momentum reaches zero.

## Non-Goals

This plan does not initially include:

- camera/view-height bob;
- recoil camera shake;
- per-weapon custom bob curves;
- weapon sway caused only by turning;
- interpolation of world physics beyond the current MegaLDOOM timing model;
- changing weapon damage, fire timing, ammo behavior, or gameplay physics.

Camera bob may be implemented later as a separate feature after weapon bob is stable.

---

# 1. Current Architecture

## 1.1 Movement already follows Doom-style timing

`src/player_controller.c` already maintains persistent fixed-point momentum:

- `s_momentum_x`
- `s_momentum_y`

Movement is simulated at Doom's 35 game tics per second using `s_doom_tic_accumulator`, independently of the visible frame cadence.

The controller also already implements Doom-style thrust, momentum clamping, stop speed, and friction.

This is the correct source of truth for weapon bob. The feature must not derive its amplitude directly from D-pad input.

## 1.2 Weapon rendering is currently tilemap-based

The weapon is currently rendered by `draw_weapon_overlay()` in:

`src/renderer/renderer_frame_overlay.c`

The implementation writes a transparent rectangular tilemap into `BG_A` with `VDP_setTileMapDataRect()`.

The current weapon renderer is efficient because it only updates the tilemap when the weapon frame changes between idle and firing states.

However, this architecture is unsuitable for smooth Doom-style bob because a BG tilemap rectangle can only be repositioned naturally in 8x8 tile increments without additional rendering tricks.

A Doom-like 1-3 pixel motion would become visibly coarse if implemented by moving the tilemap rectangle.

## 1.3 Renderer responsibilities

The current high-level composition is effectively:

```text
BG_B    -> 3D world view
BG_A    -> HUD + weapon overlay
Sprites -> actors/other sprite-based content as currently allocated
```

The weapon should move to the Mega Drive sprite system so its position can be updated at pixel precision without rewriting weapon pixels every frame.

The target composition becomes:

```text
BG_B    -> 3D world view
BG_A    -> HUD and other static overlays
Sprites -> weapon/hands + existing sprite consumers
```

---

# 2. Target Behavior

The visual model should reproduce the original Doom relationship between momentum, phase, and weapon position.

Conceptually:

```text
player momentum
    |
    v
bob amplitude
    |
    +----> horizontal offset = amplitude * cos(phase)
    |
    +----> vertical offset   = amplitude * periodic vertical curve
```

The important behavioral requirement is not bit-for-bit reuse of the DOS renderer implementation, but preservation of its observable motion and timing while remaining efficient on the 68000 and VDP.

## 2.1 Momentum-based amplitude

Doom derives bob magnitude from squared horizontal momentum.

MegaLDOOM should use the existing fixed-point momentum and compute a bounded visual bob amplitude.

The implementation must avoid expensive 64-bit arithmetic on the 68000.

Recommended approach:

1. scale `s_momentum_x` and `s_momentum_y` down before squaring;
2. square using values that safely fit 16-bit multiplication;
3. sum the squared components;
4. clamp to a visual maximum;
5. map the result into a small pixel amplitude.

The final visible range should initially target approximately:

```text
horizontal: -3 .. +3 pixels
vertical:    0 .. +4 pixels
```

These values are tuning targets, not hard architectural constants.

## 2.2 Phase

Bob phase must advance on Doom simulation tics, not renderer frames.

Add a game-tic phase counter or equivalent state to `player_controller.c` and update it only when a simulated Doom movement tic is processed.

This gives the feature the following timing model:

```text
Mega Drive VDP       60 Hz
renderer target      30 Hz currently
movement simulation  35 Hz
weapon bob phase     35 Hz
```

The renderer consumes the latest bob position produced by the simulation.

## 2.3 Horizontal and vertical frequencies

Horizontal motion should complete one left/right cycle while vertical motion completes two vertical lobes over the same horizontal period.

Conceptually:

```text
x = A * cos(t)
y = B * vertical_wave(2t)
```

The exact implementation may use `fx_sin()` / `fx_cos()` from MegaLDOOM's existing fixed-math layer.

The implementation should avoid a new trigonometric lookup table unless profiling proves the existing one unsuitable.

---

# 3. Architecture

## 3.1 Keep bob state in the controller/simulation layer

Bob amplitude and phase are derived from gameplay momentum, so their authoritative state belongs with movement simulation rather than the renderer.

Recommended internal state:

```c
static u16 s_weapon_bob_phase;
static s16 s_weapon_bob_x;
static s16 s_weapon_bob_y;
```

The exact names may differ, but the renderer must not directly access `s_momentum_x` or `s_momentum_y`.

## 3.2 Expose a small read-only interface

Add accessors in `player_controller.h` such as:

```c
s16 player_controller_weapon_bob_x(void);
s16 player_controller_weapon_bob_y(void);
```

An alternative small struct is acceptable if it produces cleaner call sites:

```c
typedef struct {
    s16 x;
    s16 y;
} WeaponBobOffset;

WeaponBobOffset player_controller_weapon_bob(void);
```

Prefer the representation that generates the smallest and clearest 68000 code after compilation.

## 3.3 Renderer owns presentation only

The renderer should receive already-computed pixel offsets and apply them to the weapon presentation.

It must not:

- calculate player speed;
- inspect input buttons;
- emulate friction;
- advance bob phase independently;
- duplicate gameplay timing constants.

---

# 4. Replace the BG_A Weapon Tilemap with Hardware Sprites

This is the central renderer change.

## 4.1 Why hardware sprites

The current `BG_A` implementation is optimized for a stationary weapon. Doom-style bob requires pixel-level movement every rendered frame.

Using hardware sprites allows MegaLDOOM to change weapon position by updating sprite coordinates instead of rebuilding or moving a BG tilemap rectangle.

Expected advantages:

- 1-pixel positioning;
- no weapon graphics DMA on normal bob frames;
- no modification of the dynamic 3D framebuffer;
- no 8-pixel snapping;
- simple integration with firing frames;
- low CPU cost for position updates.

## 4.2 Preserve weapon tile residency behavior where possible

The current renderer keeps only one weapon's graphics resident at a time because the complete weapon set does not fit safely in the existing VRAM window.

That policy should remain unless the sprite conversion requires a different layout.

Weapon switching may continue to upload the selected weapon graphics to the shared weapon VRAM region.

Bob itself must not trigger graphics uploads.

## 4.3 Convert generated weapon metadata

Update the renderer asset-generation pipeline so each weapon frame includes enough metadata to build the hardware-sprite representation.

The generated data should include, as needed:

- weapon frame dimensions;
- tile indices;
- sprite-piece layout;
- per-piece X/Y offsets from a common weapon origin;
- idle and firing variants;
- optional bounding information for clipping/placement.

Do not hand-maintain data that can be generated from the source weapon images.

Likely affected files:

```text
tools/generate-renderer-assets.ps1
src/renderer/generated_renderer_assets.h
```

Generated files should remain generated rather than manually edited.

## 4.4 Sprite decomposition

Mega Drive hardware sprites have dimension limits, so a weapon larger than one hardware sprite must be decomposed into multiple sprite pieces.

The generator should prefer larger legal sprite pieces to minimize SAT entries.

The plan must account for:

- the maximum number of total sprites;
- the maximum number of sprites per scanline;
- overlap with actor sprites;
- firing-frame dimensions that may differ from idle frames;
- weapons wider than the pistol.

Before implementation is considered complete, calculate the worst-case SAT and scanline usage for every weapon.

## 4.5 Coordinate model

Define one stable screen-space origin per weapon.

For example:

```c
screen_x = base_x + piece_x + bob_x;
screen_y = base_y + piece_y + bob_y;
```

`base_x` and `base_y` represent the neutral Doom weapon position.

`piece_x` and `piece_y` come from generated sprite decomposition.

`bob_x` and `bob_y` come from the controller.

The firing frame must use the same logical origin so firing does not cause the whole weapon to jump unexpectedly.

---

# 5. Simulation Implementation

## Phase 1 - Introduce bob state without changing rendering

Add bob calculation to `player_controller.c` while leaving the weapon visually stationary.

Tasks:

1. add bob phase state;
2. reset it in `player_controller_reset()`;
3. derive a safe squared-speed measure from momentum;
4. clamp amplitude;
5. compute pixel X/Y offsets;
6. expose read-only getters;
7. add host-side tests for the bob calculation if practical.

Acceptance criteria:

- build remains unchanged visually;
- stationary player produces `(0, 0)`;
- walking produces bounded periodic values;
- running reaches a larger amplitude than walking;
- releasing movement decays naturally according to momentum/friction;
- values remain safe for all allowed momentum values;
- no 64-bit runtime helper is introduced in the hot movement loop.

## Phase 2 - Add sprite weapon renderer behind a build-time switch

Implement a sprite-based weapon path without immediately deleting the old BG_A path.

Recommended temporary switch:

```c
#define RENDERER_SPRITE_WEAPON 1
```

This flag is temporary and should be removed after the new implementation is validated.

Tasks:

1. generate sprite-piece metadata;
2. allocate/manage the weapon sprite entries;
3. render idle weapon at its current neutral location;
4. support the existing firing variant;
5. support weapon switching;
6. restore the weapon correctly after pause/menu transitions;
7. verify palettes and priority.

Acceptance criteria:

- with bob offsets forced to zero, every weapon visually matches its current placement;
- weapon switching works;
- firing frames work;
- pause/menu restore works;
- no stale pieces from the previous weapon remain visible;
- sprite count remains inside hardware limits.

## Phase 3 - Connect bob offsets to sprite positions

Pass the controller's X/Y offsets into the weapon renderer.

Recommended flow:

```text
player_controller_update()
        |
        v
35 Hz Doom movement tics
        |
        v
weapon bob state
        |
        v
renderer_render_scene()
        |
        v
draw weapon sprites at base + bob
```

Acceptance criteria:

- weapon remains still while stationary;
- weapon starts bobbing when momentum develops;
- movement continues briefly while momentum decays;
- bob amplitude is visibly stronger while running;
- no 8-pixel stepping is visible;
- weapon motion remains stable at the current renderer cadence;
- firing does not reset or desynchronize the bob phase.

## Phase 4 - Remove legacy weapon tilemap path

After the sprite implementation passes visual and performance validation:

1. remove `BG_A` weapon tilemap composition;
2. remove obsolete tilemap cache state such as weapon-only `g_last_weapon_variant` behavior if replaced by sprite-frame state;
3. remove the temporary build flag;
4. simplify VRAM constants and comments;
5. update renderer documentation and tests.

The HUD must remain on `BG_A`.

---

# 6. Renderer API Changes

The final API should keep simulation and presentation separated.

A possible renderer call is:

```c
void draw_weapon_overlay(bool flash, s16 bob_x, s16 bob_y);
```

However, if the sprite system updates position independently from weapon-frame selection, a cleaner design may be:

```c
void renderer_set_weapon_bob(s16 x, s16 y);
void draw_weapon_overlay(bool flash);
```

Prefer whichever avoids duplicated SAT updates and keeps the current render path simple.

Do not expose controller globals to renderer files.

---

# 7. Firing Interaction

Weapon bob and firing animation are independent state dimensions.

Conceptually:

```text
weapon frame = idle/fire state
weapon position = neutral origin + bob offset
```

Changing the firing frame must not:

- reset bob phase;
- force bob to zero;
- change the neutral origin unless the source art itself requires an explicit generated offset;
- upload graphics every bob frame.

Automatic weapons must continue to work with the existing held-fire behavior.

---

# 8. Weapon Switching

When switching weapons:

1. upload the selected weapon's graphics as currently required;
2. rebuild or rebind the weapon sprite pieces;
3. preserve current bob phase;
4. apply the current bob offset immediately;
5. remove or hide unused pieces left by a larger previous weapon.

The new weapon should not visually snap to an unrelated phase simply because the player changed weapons while moving.

---

# 9. Pause and Frontend Restore

`renderer_restore_after_menu()` currently reloads HUD and weapon tile data and clears `BG_A`.

The sprite implementation must update this flow.

Required behavior after returning from a pause/menu screen:

- selected weapon remains selected;
- correct weapon graphics are resident;
- all weapon sprite pieces are restored;
- current bob position is applied;
- HUD is restored independently on `BG_A`;
- no duplicate or stale sprite pieces remain.

---

# 10. Performance Constraints

The feature must be designed for Mega Drive hardware rather than as a desktop-style animation system.

## 10.1 68000 CPU

Avoid:

- 64-bit multiplication/division;
- floating point;
- per-frame dynamic allocation;
- per-pixel weapon composition;
- unnecessary trigonometric work in the renderer;
- rebuilding weapon graphics for position-only changes.

Prefer:

- existing fixed-point math;
- word-sized multiplies where possible;
- small bounded state;
- generated static metadata;
- SAT coordinate updates.

## 10.2 VRAM

The implementation must preserve or improve the current VRAM safety guarantees.

Document the final ranges used by:

- dynamic world tile banks;
- HUD tiles;
- font;
- static atlases when enabled;
- weapon tiles.

No weapon frame may overlap another reserved VRAM region.

## 10.3 Sprite limits

Before merging the final renderer conversion, record for each weapon:

```text
weapon
idle sprite pieces
fire sprite pieces
maximum sprites on one scanline
VRAM tile count
```

Also measure worst-case interaction with world sprites/billboards.

If the weapon threatens the per-scanline sprite limit, adjust sprite decomposition rather than silently accepting flicker.

---

# 11. Testing Plan

## 11.1 Bob math tests

Extend `tools/test-weapons.py` or add a focused test file.

Test at minimum:

### Stationary

```text
momx = 0
momy = 0
=> x = 0, y = 0
```

### Symmetry

Equivalent speed in different movement directions should produce equivalent amplitude.

### Clamp

Maximum allowed momentum must never generate offsets outside the configured visual limit.

### Walking vs running

Running must produce a larger or equal bob amplitude than walking.

### Friction decay

After movement input stops, bob amplitude must decay as momentum decays instead of dropping directly to zero.

### Periodicity

The bob cycle must repeat predictably and vertical frequency must be twice the horizontal frequency.

## 11.2 Renderer tests

Validate:

- all weapon IDs;
- idle frame;
- fire frame;
- switching from smallest to largest weapon;
- switching from largest to smallest weapon;
- pause/restore;
- damage overlay and low-health overlay coexistence;
- HUD correctness;
- weapon clipping at maximum bob offsets.

## 11.3 Hardware/emulator visual tests

Capture movement sequences for:

- walk forward;
- run forward;
- backward movement;
- strafing left/right;
- diagonal movement if supported by the input state;
- release after running;
- firing while running;
- switching weapons while running.

Compare the rhythm and path against original Doom footage or a reference source port using the original bob behavior.

---

# 12. Profiling and Regression Checks

Measure before and after the sprite conversion.

Track at minimum:

- renderer frame cadence;
- movement/controller time if instrumentation exists;
- weapon rendering subticks;
- VBlank budget;
- DMA bytes caused by weapon updates;
- sprite/SAT usage;
- ROM size change;
- RAM usage change.

Expected result:

- bob-only frames should not upload weapon graphics;
- changing weapon position should be substantially cheaper than rebuilding BG tilemap data;
- no measurable regression in the 3D view renderer should occur.

---

# 13. Tuning Strategy

Do not tune the motion by changing physics constants.

Gameplay momentum must remain authoritative and unchanged.

Only visual conversion constants may be tuned, for example:

```text
momentum-to-amplitude scale
maximum horizontal pixels
maximum vertical pixels
phase scale
neutral weapon origin
```

Tuning order:

1. match cycle timing;
2. match horizontal amplitude;
3. match vertical amplitude;
4. validate walking;
5. validate running;
6. validate friction decay;
7. validate all weapon sizes.

---

# 14. Optional Follow-Up: Camera Bob

Camera bob should be a separate follow-up after weapon bob is complete.

The original Doom also modifies view height based on player bob state. MegaLDOOM can reproduce that later, but camera bob is more invasive because it affects the world presentation rather than only a first-person overlay.

A future camera-bob implementation should reuse the same underlying movement/bob magnitude rather than create another speed calculation.

Do not couple camera bob to the initial weapon-sprite migration.

---

# 15. Implementation Order

Recommended commit sequence:

## Commit 1 - Bob simulation state

- add momentum-derived bob calculation;
- add phase state;
- add getters;
- add math tests;
- no visual changes.

## Commit 2 - Generated sprite weapon metadata

- extend asset generator;
- generate weapon sprite-piece layouts;
- add compile-time bounds checks where practical.

## Commit 3 - Sprite weapon renderer

- implement hardware-sprite weapon composition;
- preserve idle/fire behavior;
- preserve weapon switching;
- keep bob at zero initially.

## Commit 4 - Enable Doom weapon bob

- connect controller offsets to weapon sprite coordinates;
- tune visual amplitude and timing;
- add integration tests.

## Commit 5 - Remove legacy BG_A weapon path

- delete obsolete tilemap-specific code;
- clean renderer state;
- update comments/documentation;
- run full regression suite.

## Commit 6 - Performance and hardware validation

- record profiling results;
- verify sprite limits;
- verify VRAM map;
- perform emulator and real-hardware smoke tests when available.

---

# 16. Definition of Done

The feature is complete when all of the following are true:

- weapon bob is derived from existing Doom-style momentum;
- bob phase advances with the 35 Hz simulation;
- horizontal and vertical movement reproduce the characteristic Doom rhythm;
- stationary weapons return to the neutral position;
- movement decays naturally after input release;
- walking and running have visibly appropriate amplitudes;
- weapon movement has pixel precision;
- the weapon no longer depends on moving a BG_A tilemap rectangle;
- all weapons and firing frames work;
- weapon switching while moving works;
- pause/menu restoration works;
- HUD rendering is unaffected;
- no weapon graphics DMA occurs solely because bob position changed;
- no new 64-bit or floating-point hot-path dependency is introduced;
- VRAM ranges remain safe;
- Mega Drive sprite and scanline limits are validated;
- automated weapon/bob tests pass;
- current gameplay movement constants remain unchanged;
- renderer performance remains within the existing frame budget.

---

# 17. Final Target

The desired result is a first-person weapon presentation that feels like Doom because it is driven by the same underlying movement state, not because a canned animation starts when a button is pressed.

The final data flow should remain simple:

```text
Doom-style thrust
      |
      v
player momentum
      |
      v
35 Hz bob calculation
      |
      v
pixel X/Y offset
      |
      v
Mega Drive hardware weapon sprites
      |
      v
smooth Doom-style hand/weapon motion
```

This approach preserves MegaLDOOM's existing movement model, keeps the renderer efficient, and maps the original Doom visual behavior naturally onto Mega Drive hardware.