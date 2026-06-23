# Rewrite checkpoints

This file tracks human validation gates for the clean raycaster rewrite.

## Baseline

- Baseline Git commit before rewrite execution: `a2d7a1a`
- Old prototype entry point: `src/main.c`
- Status: frozen as broken prototype reference
- Rule: do not copy renderer, DDA, BMP path, tables or game loop structure from the old prototype into the new raycaster.

## Gate 0: external sources and baseline

Implementation status: accepted by human validation.

Completed:

- `.extenals/` is ignored by Git.
- `tools/sync-externals.ps1` creates/updates local external sources.
- Freedoom was cloned into `.extenals/freedoom`.
- `docs/external-assets.md` records the exact Freedoom commit.
- The old renderer path is documented as frozen and excluded from the rewrite base.

Human validation:

- Confirm that Gate 0 is accepted.
- After acceptance, the next implementation step is Gate 1: clean SGDK skeleton.

## Gate 1: clean SGDK skeleton

Implementation status: ready for human validation.

Scope:

- Replace the old entry point with a minimal SGDK program.
- Initialize VDP, CRAM, joypad and the main VSync loop.
- Render only a black screen with static text and a small frame counter.
- Keep BMP and raycast completely out of this gate.

Expected visual result:

- Black screen.
- Text: `MEGALDOOM REWRITE GATE 1`.
- Text: `NO BMP  NO RAYCAST`.
- Hex frame counter changing near the center.

Build validation:

- `.\tools\build-windows.ps1`
- Result: passed with zero compiler warnings.
- Build input now compiles only `src/main.c`, boot code and empty resources; old prototype modules were moved out of `src/`.

Human validation:

- Run in BlastEm and ares.
- Leave it running long enough to confirm there is no periodic corruption.

## Gate 2: dynamic tile renderer

Implementation status: ready for human validation.

Scope:

- Keep the program free of BMP and raycast.
- Allocate a small 16x12 tile viewport in RAM.
- Generate deterministic tile patterns every frame.
- Upload 192 tiles to a fixed VRAM range using SGDK tile DMA queue.
- Fill BG_B tilemap once with the dynamic tile indices.
- Keep debug text on BG_A.

Expected visual result:

- Text: `MEGALDOOM REWRITE GATE 2`.
- A centered 128x96 px animated tile viewport.
- The viewport should show stable bands/checker-like color blocks.
- Frame counter continues changing.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: passed with zero compiler warnings.

Human validation:

- Run in BlastEm and ares.
- Leave it running for at least 60 seconds in each emulator.
- If stable, Gate 3 starts with a fake 3D window using this tile path.

## Gate 3: fake 3D tile window

Implementation status: accepted by human validation.

Scope:

- Keep the program free of BMP, DDA, raycast and map data.
- Preload 16 solid color tiles once.
- Draw sky, floor and synthetic wall columns into a 16x12 tilemap once.
- Upload only the 16 solid tiles once.
- Upload the 16x12 tilemap once on CPU.
- Keep per-frame work limited to joypad, sparse debug counter update and VSync.

Expected visual result:

- Text: `MEGALDOOM REWRITE GATE 3`.
- Text: `FAKE 3D TILE WINDOW`.
- A centered 128x96 px fake 3D view.
- Blue-ish upper half, green-ish floor, blocky vertical wall columns.
- Frame counter continues changing.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: passed with zero compiler warnings.
- First pixel-framebuffer attempt was rejected because it did not show the viewport and dropped to 16 VPS in ares.
- Corrected version uses tilemap animation only for this gate.
- Second attempt displayed the viewport but still dropped to about 13 VPS in ares.
- Current revision updates the tilemap once every 8 frames, moves the counter out of the viewport, and updates text once every 16 frames.
- DMA_QUEUE tilemap update made the viewport disappear in ares.
- Current accepted candidate is static: tilemap is uploaded once via CPU.

Human validation:

- Run in BlastEm and ares.
- Leave it running for at least 60 seconds in each emulator.
- If stable, Gate 4 starts with isolated fixed-point math and visual angle/vector checks.
- Accepted in ares with visible viewport and 60 VPS.

## Gate 4: fixed-point vector test

Implementation status: accepted by human validation.

Scope:

- Add a new fixed-point math module.
- Generate a 256-step Q8 sine table at startup.
- Provide `fx_sin()` and `fx_cos()`.
- Keep the fake 3D viewport static from Gate 3.
- Add a small tile compass on the left side that updates every 16 frames.

Expected visual result:

- Text: `MEGALDOOM REWRITE GATE 4`.
- Text: `FIXED POINT VECTOR TEST`.
- Static fake 3D viewport remains visible.
- A small compass-like 5x5 block appears to the left.
- A colored dot moves around the compass smoothly.
- Frame counter continues changing.
- VPS remains near 60.

Build validation:

- `.\tools\build-windows.ps1`
- Result: passed with zero compiler warnings.

Human validation:

- Run in BlastEm and ares.
- Leave it running for at least 60 seconds in each emulator.
- If stable, Gate 5 starts DDA untextured.
- Accepted by human validation.

## Gate 5: untextured DDA raycast

Implementation status: accepted by human validation.

Scope:

- Add a new raycast module.
- Use a fixed 8x8 test map.
- Use a fixed player position.
- Cast 16 rays using grid DDA traversal.
- Render one tile column per ray.
- Use flat wall colors only, with side shading.
- Keep BMP, textures, sprites, doors and movement out of this gate.
- Rotate camera automatically at a low update rate.
- Apply fish-eye correction before projecting wall height.
- Update raycast every 4 frames with smaller angle increments for smoother rotation.

Expected visual result:

- Text: `MEGALDOOM REWRITE GATE 5`.
- Text: `UNTEXTURED RAYCAST`.
- Viewport shows blocky flat-color walls over sky/floor.
- Walls change shape slowly as the camera rotates.
- Rotation should feel less jumpy than the first Gate 5 attempt.
- Compass dot still rotates.
- Frame counter continues changing.
- VPS remains near 60.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: passed with zero compiler warnings.
- First human validation said the rotation worked but did not feel natural; current revision smooths the update cadence and adds fish-eye correction.

Human validation:

- Run in BlastEm and ares.
- Leave it running for at least 60 seconds in each emulator.
- If stable, Gate 6 can add controlled player movement and collision.
- Accepted by human validation after smoothing rotation and adding fish-eye correction.

## Gate 6: controlled movement and collision

Implementation status: accepted by human validation.

Scope:

- Add `PlayerState` with fixed-point position and angle.
- Move the DDA raycast from fixed position to current player state.
- Add simple grid collision with a small player radius.
- Use D-pad up/down for forward/backward movement.
- Use D-pad left/right for turning.
- Use A/C for strafe left/right.
- Redraw only when input changes the player state.
- Keep BMP, textures, sprites and doors out of this gate.

Expected visual result:

- Text: `MEGALDOOM REWRITE GATE 6`.
- Text: `MOVE AND COLLISION`.
- D-pad moves/turns the camera.
- A/C strafes.
- Walls should block the player.
- View remains flat-color and blocky.
- VPS remains near 60.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: passed with zero compiler warnings.

Human validation:

- Run in BlastEm and ares.
- Test turning, forward/backward movement and strafe.
- Push into walls from several angles; the player should not pass through.
- If stable, Gate 7 can begin minimal wall texturing.
- Accepted by human validation.

## Gate 7: minimal wall texturing

Implementation status: accepted by human validation.

Scope:

- Keep the existing 16-column tile renderer.
- Keep movement and collision from Gate 6.
- Add wall hit `tex_x` calculation in DDA.
- Add tiny 8x8 procedural wall and door textures.
- Render wall tiles by sampling one texture color per tile cell.
- Keep BMP, sprites, doors interaction and imported external assets out of this gate.

Expected visual result:

- Text: `MEGALDOOM REWRITE GATE 7`.
- Text: `MINIMAL WALL TEXTURE`.
- Walls should no longer be solid flat blocks.
- The visible wall pattern should stay attached to walls while moving/turning.
- Movement and collision should behave like Gate 6.
- VPS should remain acceptable; near 60 is ideal but this gate is now measuring a more expensive feature.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: passed with zero compiler warnings.

Human validation:

- Run in BlastEm and ares.
- Move, turn and strafe near walls.
- Check whether texture appears stable or slides/tears badly.
- If stable, the next step is optimizing texture sampling or adding a real asset conversion gate.
- Accepted by human validation.

## Gate 8: real asset conversion

Implementation status: accepted by human validation.

Scope:

- Add a small asset conversion script.
- Read one PNG from the local non-versioned Freedoom checkout.
- Downsample it to an 8x8 palette-index texture.
- Generate a versioned C header used by the renderer.
- Replace the procedural wall texture with the generated Freedoom texture.
- Keep the door texture procedural for now.

Converted asset:

- Source: `.extenals/freedoom/patches/stonew1.png`
- Output: `src/generated_assets.h`
- Symbol: `FREEDOOM_WALL_TEXTURE`

Expected visual result:

- Text: `MEGALDOOM REWRITE GATE 8`.
- Text: `FREEDOOM ASSET TEXTURE`.
- Walls should use a noisier stone-like palette pattern from Freedoom.
- Movement and collision should behave like Gate 7.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: passed with zero compiler warnings.

Human validation:

- Run in BlastEm and ares.
- Confirm walls still render and texture remains stable while moving/turning.
- Accepted by human validation.

## Gate 9: texture sampling table

Implementation status: ready for human validation.

Scope:

- Keep Gate 8 visuals and behavior.
- Precompute vertical texture sampling for each possible wall height.
- Replace per-wall-tile vertical division with table lookup.
- Add no new gameplay feature.
- Refactor `main.c` for SRP: move video setup, tile generation, tilemap rendering, compass and frame counter drawing into `renderer.c`.
- Move joypad-to-player-state behavior into `player_controller.c`.
- Keep `main.c` responsible only for startup sequencing, high-level frame loop and orchestration.

Expected visual result:

- Text: `MEGALDOOM REWRITE GATE 9`.
- Text: `TEXTURE SAMPLING TABLE`.
- Visual result should match Gate 8 closely.
- Movement and collision should behave like Gate 8.
- VPS should be no worse than Gate 8.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: passed with zero compiler warnings.
- Rebuilt after SRP refactor; result still passed with zero compiler warnings.
- Rebuilt after extracting `player_controller.c`; result still passed with zero compiler warnings.

Human validation:

- Run in BlastEm and ares.
- Confirm texture still appears stable while moving/turning.
- Confirm there is no new visual artifact compared to Gate 8.
