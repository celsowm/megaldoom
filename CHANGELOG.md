# Changelog

## Unreleased

- Added Doom-style weapon bob. `player_controller.c` (`update_weapon_bob`,
  exposed via `player_controller_weapon_bob_x/y`) derives a momentum-based,
  35 Hz-tic-driven pixel offset (octagonal-norm magnitude, no 64-bit math) with
  a half-circle-folded 2x vertical wave; it keeps bobbing through the friction
  tail after input release and re-centres on an exact stop.
- Wired that offset to the weapon as a BG_A hardware-scroll (two VDP register
  writes/frame, zero tile DMA): the status-bar numbers moved to the WINDOW plane
  (bottom `HUD_PANEL_H` rows) so BG_A now carries nothing but the weapon and can
  be whole-plane scrolled without dragging the HUD. Replaces the sprite-weapon
  route in `docs/DOOM_WEAPON_BOB_IMPLEMENTATION_PLAN.md` (see its Implementation
  Note); no asset-generator or VRAM-map changes. `PLAYER_CONTROL_WEAPON_BOB`
  asks for a cheap weapon-overlay frame when the bob moves without a whole-pixel
  world step. Contracts in `tools/test-weapon-bob.py`.
- Tuned the bob after play tests: swing widened to `+/-10 px` horizontal,
  `0..10 px` vertical dip (downward only, Doom's positive lobe). To give the
  vertical dip somewhere to go, the 3D view was dropped 4 tile-rows to sit flush
  against the top of the status bar (`VIEW_TILEMAP_Y 5 -> 9`, freed space becomes
  a taller black border above the view). The weapon now bottom-anchors to the
  HUD edge, and a downward bob dips its base into the WINDOW/HUD region where
  plane A is suppressed, so the gun's cut-off bottom edge is never visible.
  Death prompt moved with the view (`DEATH_PROMPT_Y 8 -> 12`).
- Weapon bob now eases back to neutral when the player stops translating even
  though momentum stays high -- i.e. shoving into a wall no longer races the walk
  cadence on the spot. `update_weapon_bob` takes the tic's real "did the player
  move a whole pixel" result; a short grace window (`BOB_MOVE_GRACE_TICS`) bridges
  a slow walk's sub-pixel gaps, and past it the offset decays 1px/tic (phase
  held) instead of advancing.
- Generated raw E1M1 THINGS into ROM and spawn a curated set of 102 gameplay
  entities: pickups, enemies, candles/lamp-style props, columns and barrels.
- Added a generated 15-sprite billboard world atlas, functional health/armor/
  pistol-ammo pickups, real HUD ammo/armor values, and blocking static props.
- Capped imported billboard rendering to the nearest 12 visible objects and
  added active/cull/draw/prop-collision DEBUG_PERF counters.
- Added an offline-generated spatial blockmap for exact local collision and LOS
  queries, replacing repeated full E1M1 segment scans.
- Made BSP near/far ordering lazy per visited node and clipped near-plane child
  boxes instead of conservatively expanding them to the full view.
- Reduced player movement collision calls and removed per-substep divisions.
- Expanded `DEBUG_PERF` with player/enemy collision, LOS, candidate-count and
  BSP side-cache measurements.
- Added deterministic blockmap validation across 5,638 route, boundary, wall-end,
  cardinal, diagonal and zero-length query cases.

## Unreleased

- Optimized the BSP renderer's bounding-box frustum test with a division-free half-plane precheck (`LEFT_REJECT_SCALE` / `RIGHT_REJECT_SCALE`) that rejects out-of-FOV child boxes before paying for four perspective divisions, and derived the right view axis from the forward basis (`g_rx = -g_fwy; g_ry = g_fwx`) to drop two trig lookups per frame.
- Replaced billboard software divisions with bounded 68000 `DIVS.W` / `DIVU.W` (shared view basis, `divu32_16_exact` two-stage divider for Q16 texture stepping) and converted the billboard renderer's texture-step divisions to `divu` / `divu32_16_exact`, removing roughly 14-28 slow 32-bit division-helper calls per turning frame.
- Replaced the stride-4 strip packer with a direct pre-shaded tile packer that writes each output u32 once (no 120-entry intermediate strips, ~19 KB less intermediate traffic per base frame) and folded `commit_base_tile`'s per-row comparison into one difference accumulator.
- Generated weapon clear masks as immutable ROM arrays (`MEGALDOOM_WEAPON_CLEAR_IDLE` / `_FIRE`) so `draw_weapon_overlay` is a single table-lookup read-modify-write per op instead of reconstructing each mask at runtime.
- Added `DEBUG_PERF`-guarded BSP traversal instrumentation (nodes visited, cheap frustum rejects, boxes projected, near-plane fallbacks) to the perf overlay.
- Added Doom sound effects through the XGM2 PCM driver: pistol gunshot, enemy pain/death, player pain/death, door move and item pickup.
- Added `tools/extract-sfx.py` to extract Doom `DS*` DMX sound lumps from `DOOM1.WAD` into `res/sound/*.wav` (mirrors the existing MUS music extractor).
- Declared XGM2 PCM SFX resources in `res/resources.res`; triggered with `XGM2_playPCM(..)` on PCM channels 2/3 so music (channel 1) is never interrupted.

## 0.0.4

- Renamed the project to MegalDoom.
- Updated include guards from `MEGADOOMGEO_*` to `MEGALDOOM_*`.
- Updated runtime version string to `MegalDoom 0.0.4`.
- Added `.gitignore` with SGDK build outputs, local toolchain and emulator folders.
- Added `.env.example` for local overrides.
- Added Windows SGDK setup script: `tools/setup-sgdk-windows.ps1`.
- Added Linux SGDK setup script: `tools/setup-sgdk-linux.sh`.
- Added Windows BlastEm downloader: `tools/download-emulator-windows.ps1`.
- Added Linux BlastEm downloader: `tools/download-emulator-linux.sh`.
- Reworked build scripts to auto-detect `.toolchain/sgdk`.
- Added run scripts that build and launch the ROM in BlastEm.
- Added clean scripts for Windows and Linux.

## 0.0.3

- Removed redundant full-frame `BMP_clear()`.
- Reordered frame rendering to draw HUD before weapon.
- Added per-column ray relative-angle and fish-eye correction tables.
- Added precomputed inverse sine/cosine tables for DDA.
- Replaced per-scanline wall texture division with fixed-point texture stepping.
- Replaced clipped `draw_hline()` wall spans with a direct 4-pixel column writer.
- Capped billboard enemy screen height.
- Added two-pixel horizontal actor draw step.
- Added `ENABLE_MINIMAP`, `ACTOR_MAX_SCREEN_H` and `ACTOR_X_STEP` compile-time switches.

## 0.0.2

- Replaced simple ray march with DDA grid raycasting.
- Added side-hit information for wall shading.
- Added fake 64x64 wall patterning.
- Added mutable map state.
- Added doors toggled with B.
- Added placeholder billboard enemy.
- Added per-column depth buffer for actor occlusion.
- Improved minimap and placeholder weapon.
- Split map code into `map.c` / `map.h`.

## 0.0.1

- Initial SGDK project.
- Software bitmap raycaster.
- 16x16 map.
- Movement, rotation, strafing, collision.
- HUD, minimap, placeholder weapon.
