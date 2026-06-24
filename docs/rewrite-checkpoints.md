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

## Gate 10: packed 32-column renderer

Implementation status: accepted by human validation.

Scope:

- Increase ray count from 16 to 32.
- Keep the viewport at 16x12 tiles.
- Pack two 4px-wide ray columns into each 8px tile.
- Precompute 256 pair-color tiles once: left color 0-15 and right color 0-15.
- Update tilemap entries only; do not generate or upload wall tiles every frame.
- Keep the existing 8x8 texture sampling and movement/collision behavior.

Reasoning:

- The earlier dynamic-tile attempt failed because it generated/uploaded many tiles every frame.
- This gate improves horizontal resolution while keeping per-frame VDP work bounded.
- CPU work increases from 16 rays to 32 rays, so performance must be validated before adding any new gameplay feature.

Expected visual result:

- Text: `MEGALDOOM REWRITE GATE 10`.
- Text: `PACKED 32 COLUMN VIEW`.
- Walls should look less chunky horizontally than Gate 9.
- Movement and collision should behave like Gate 9.
- Texture should remain attached to walls while moving/turning.
- VPS should remain acceptable; any large drop stops the gate.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: passed with zero compiler warnings.

Human validation:

- Run in BlastEm and ares.
- Move, turn and strafe near walls.
- Confirm the 32-column packed renderer is visually cleaner than Gate 9.
- Confirm texture does not detach, smear badly or corrupt.
- Confirm VPS does not collapse; a large drop stops this gate.
- Accepted by human validation.

## Gate 11: ray table precompute

Implementation status: accepted by human validation.

Scope:

- Keep Gate 10 visuals and behavior.
- Precompute per-column ray angle offsets.
- Precompute per-column fish-eye correction.
- Remove per-column offset division and correction cosine lookup from the frame cast loop.
- Add no new gameplay feature.

Expected visual result:

- Same visible behavior as Gate 10.
- Same 32-column packed renderer.
- Same movement and collision.
- VPS should be no worse than Gate 10.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: passed with zero compiler warnings.

Human validation:

- Run in BlastEm and ares.
- Confirm visual behavior matches Gate 10.
- Confirm movement/collision still work.
- Confirm VPS is no worse than Gate 10.
- Accepted by human validation.

## Gate 12: DDA reciprocal precompute

Implementation status: accepted by human validation.

Scope:

- Keep Gate 11 visuals and behavior.
- Precompute ray direction X/Y for all 256 angles.
- Precompute DDA delta distance X/Y for all 256 angles.
- Remove per-ray direction sine/cosine lookup and reciprocal division from `cast_single_ray()`.
- Add no new gameplay feature.

Expected visual result:

- Same visible behavior as Gate 11.
- Same 32-column packed renderer.
- Same movement and collision.
- VPS should be no worse than Gate 11.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: passed with zero compiler warnings.

Human validation:

- Run in BlastEm and ares.
- Confirm visual behavior matches Gate 11.
- Confirm movement/collision still work.
- Confirm VPS is no worse than Gate 11.
- Accepted by human validation.

## Gate 13: world map module split

Implementation status: accepted by human validation.

Scope:

- Keep Gate 12 visuals and behavior.
- Move map data and tile queries out of `raycast.c`.
- Add `world_map.c/.h` as the owner of map dimensions, cell size, tile IDs and solidity.
- Keep DDA/projection in `raycast.c`.
- Add no new gameplay feature.

Reasoning:

- Doors, larger maps and asset-driven levels should not be bolted directly into the raycast module.
- This gate reduces responsibility overlap before adding interactive world features.

Expected visual result:

- Same visible behavior as Gate 12.
- Same movement and collision.
- Same texture behavior.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: passed with zero compiler warnings.

Human validation:

- Run in BlastEm and ares.
- Confirm visual behavior matches Gate 12.
- Confirm movement/collision still work.
- Confirm texture remains stable.
- Accepted by human validation.

## Gate 14: instant door interaction

Implementation status: accepted by human validation.

Scope:

- Keep Gate 13 renderer, raycast and movement behavior.
- Make `world_map` hold mutable runtime map state.
- Add map initialization from immutable initial data.
- Add instant door toggle in front of the player.
- Add B-button edge detection in the player controller.
- Keep doors non-animated for this gate.

Expected visual result:

- Same 32-column textured view as Gate 13.
- Door cells render with the door color/texture while closed.
- Pressing B near a door opens it instantly.
- Open door no longer blocks movement or raycast.
- Pressing B again closes the same door.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: passed with zero compiler warnings.

Human validation:

- Run in BlastEm and ares.
- Move to a visible door and press B.
- Confirm the door disappears from raycast/collision when opened.
- Confirm pressing B again closes it.
- Confirm holding B does not repeatedly toggle every frame.
- Accepted by human validation.

## Gate 15: per-column depth buffer

Implementation status: accepted by human validation.

Scope:

- Keep Gate 14 visuals and behavior.
- Extend raycast output with corrected per-column depth.
- Preserve the existing renderer output.
- Add no sprite rendering yet.

Reasoning:

- Sprites and pickups need wall-depth occlusion.
- Adding sprites before validating depth data would mix rendering bugs with projection bugs.

Expected visual result:

- Same visible behavior as Gate 14.
- Same instant door interaction.
- Same movement and collision.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: passed with zero compiler warnings.

Human validation:

- Run in BlastEm and ares.
- Confirm visual behavior matches Gate 14.
- Confirm instant doors still work.
- Confirm movement/collision still work.
- Accepted by human validation.

## Gate 16: occluded billboard placeholder

Implementation status: accepted by human validation.

Scope:

- Keep Gate 15 movement, doors and wall renderer.
- Add one fixed billboard object in world space.
- Project the billboard into the 32-column view.
- Draw the billboard as tilemap color blocks, not hardware sprites yet.
- Use per-column wall depth to hide billboard columns behind walls.
- Add no AI, animation, pickup logic or hardware sprite system.

Reasoning:

- This validates projection and wall occlusion before spending complexity on VDP sprites.
- If oclusion is wrong here, hardware sprites would only make the bug harder to isolate.

Expected visual result:

- Same textured scene as Gate 15.
- A small colored placeholder object appears in the world.
- It grows/shrinks with distance and moves on screen while turning.
- Far distance should reduce it to a single coarse column/tile before it disappears.
- It disappears behind walls/doors when occluded.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: passed with zero compiler warnings.
- First validation showed far-distance scale staying too large; current revision allows a 1-column/1-tile minimum and hides the billboard past 6 cells.

Human validation:

- Run in BlastEm and ares.
- Move and turn around the placeholder object.
- Confirm it appears in front of walls when closer.
- Confirm it disappears behind walls/closed doors when occluded.
- Confirm doors and movement still work.
- Accepted by human validation after far-distance scale correction.

## Gate 17: billboard projection module split

Implementation status: accepted by human validation.

Scope:

- Keep Gate 16 visuals and behavior.
- Move billboard projection and occlusion decisions out of `renderer.c`.
- Add `billboard.c/.h` for world-space billboard projection.
- Keep actual tilemap drawing in `renderer.c`.
- Add no new sprites, AI, animation or hardware sprite system.

Reasoning:

- Renderer should draw projected spans, not own world-space billboard math.
- This prepares multiple objects without turning `renderer.c` into a gameplay/entity module.

Expected visual result:

- Same visible behavior as Gate 16.
- Same billboard distance scaling and wall/door occlusion.
- Same movement, doors and collision.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: passed with zero compiler warnings.

Human validation:

- Run in BlastEm and ares.
- Confirm visual behavior matches Gate 16.
- Confirm billboard scale and occlusion still work.
- Confirm movement, doors and collision still work.
- Accepted by human validation.

## Gate 18: static billboard list

Implementation status: accepted by human validation.

Scope:

- Keep Gate 17 movement, doors and renderer.
- Replace the single hardcoded billboard with a small static billboard list.
- Project all billboards into a fixed span buffer.
- Draw the projected spans through the existing renderer overlay path.
- Keep objects static: no AI, animation, pickups or hardware sprite system.
- Avoid dynamic allocation.

Expected visual result:

- Multiple colored placeholder objects can be visible in the map.
- Existing billboard scale and wall/door occlusion behavior remains.
- Doors, movement and collision still work.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: passed with zero compiler warnings.

Human validation:

- Run in BlastEm and ares.
- Move and turn around the map until more than one colored billboard appears.
- Confirm each billboard still shrinks with distance.
- Confirm walls and closed doors still occlude billboard columns.
- Confirm movement, doors and collision still work.
- Object-object depth sorting is intentionally not part of this gate; overlapping billboards may draw in list order.
- Accepted by human validation.

## Gate 19: billboard depth sort

Implementation status: accepted by human validation.

Scope:

- Keep Gate 18 movement, doors, static billboard list and renderer.
- Sort projected billboard spans from far to near before drawing.
- Preserve wall and closed-door occlusion through the existing per-column wall depth check.
- Add no animation, AI, pickups, hardware sprite system or dynamic allocation.

Expected visual result:

- Overlapping billboard columns draw nearer objects over farther objects.
- Existing distance scaling and wall/door occlusion behavior remains.
- Doors, movement and collision still work.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: passed with zero compiler warnings.

Human validation:

- Run in BlastEm and ares.
- Move and turn until two billboard placeholders overlap or nearly overlap.
- Confirm the nearer billboard appears over the farther one.
- Confirm walls and closed doors still occlude billboard columns.
- Confirm movement, doors and collision still work.
- Accepted by human validation.

## Gate 20: textured billboard placeholder

Implementation status: accepted by human validation.

Scope:

- Keep Gate 19 movement, doors, static billboard list and sorted draw order.
- Add horizontal texture coordinate output to each projected billboard span.
- Replace solid-color billboard blocks with a tiny 8x8 masked placeholder texture.
- Preserve transparent texels, per-object body color, wall occlusion and depth sorting.
- Add no animation, AI, pickups, hardware sprite system, asset conversion or dynamic allocation.

Expected visual result:

- Billboards look like simple shaped placeholders instead of solid rectangles.
- Transparent texels let the wall/floor behind show through.
- Existing distance scaling, wall/door occlusion and near-over-far drawing remain.
- Doors, movement and collision still work.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: passed with zero compiler warnings.

Human validation:

- Run in BlastEm and ares.
- Move and turn around the static billboards.
- Confirm the objects now have a simple masked shape, not a solid rectangle.
- Confirm transparent pixels reveal the wall/floor behind them.
- Confirm distance scaling, wall/door occlusion and near-over-far draw order still work.
- Confirm movement, doors and collision still work.
- Accepted by human validation.

## Gate 21: converted Freedoom billboard asset

Implementation status: accepted by human validation.

Scope:

- Keep Gate 20 movement, doors, static billboard list, depth sorting and wall occlusion.
- Extend the Freedoom conversion script with a billboard-output path.
- Convert one real Freedoom sprite into an 8x8 palette-index billboard texture.
- Replace the procedural billboard mask with the converted asset.
- Keep the object list static and keep rendering through the existing tilemap overlay.
- Add no animation, AI, pickups, hardware sprite system or dynamic allocation.

Expected visual result:

- Billboards use the converted Freedoom bonus sprite instead of the procedural diamond mask.
- Palette index 0 remains transparent.
- Existing distance scaling, wall/door occlusion and near-over-far drawing remain.
- Doors, movement and collision still work.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\convert-freedoom-assets.ps1 -BillboardOnly`
- Result: generated `src/generated_billboard_assets.h`.
- `.\tools\convert-freedoom-assets.ps1`
- Result: completed with a warning because `src/generated_assets.h` was locked by another process; the existing wall texture header was left unchanged and the billboard header was generated.
- `.\tools\build-windows.ps1`
- Result: passed with zero compiler warnings.

Human validation:

- Run in BlastEm and ares.
- Move and turn around the static billboards.
- Confirm the billboard shape changed from the procedural placeholder to the converted Freedoom bonus sprite.
- Confirm palette index 0 remains transparent.
- Confirm distance scaling, wall/door occlusion and near-over-far draw order still work.
- Confirm movement, doors and collision still work.
- Accepted by human validation.

## Gate 22: billboard type definitions

Implementation status: accepted by human validation.

Scope:

- Keep Gate 21 visuals, movement, doors, static billboard list, depth sorting and wall occlusion.
- Split billboard instances from billboard type definitions.
- Store position plus `type_id` in the static object list.
- Store visual id, scale and max depth in a small type table.
- Pass visual id through projected spans so the renderer can choose the texture.
- Add no new asset, animation, AI, pickup behavior, hardware sprite system or dynamic allocation.

Expected visual result:

- Visual behavior should match Gate 21.
- Billboards still use the converted Freedoom bonus sprite.
- Existing distance scaling, wall/door occlusion and near-over-far drawing remain.
- Doors, movement and collision still work.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: passed with zero compiler warnings.

Human validation:

- Run in BlastEm and ares.
- Confirm the screen shows `MEGALDOOM REWRITE GATE 22`.
- Confirm visual behavior matches Gate 21.
- Confirm billboards still use the converted Freedoom bonus sprite.
- Confirm distance scaling, wall/door occlusion and near-over-far draw order still work.
- Confirm movement, doors and collision still work.
- Accepted by human validation.

## Gate 23: multiple billboard visuals

Implementation status: accepted by human validation.

Scope:

- Keep Gate 22 movement, doors, static billboard list, type table, depth sorting and wall occlusion.
- Convert a second Freedoom sprite into the generated billboard asset header.
- Add a second billboard visual id and type.
- Place at least one static object using the second type.
- Keep all objects static and non-interactive.
- Add no animation, AI, pickup behavior, hardware sprite system or dynamic allocation.

Expected visual result:

- At least two different billboard appearances exist in the map.
- Existing distance scaling, transparency, wall/door occlusion and near-over-far drawing remain.
- Doors, movement and collision still work.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\convert-freedoom-assets.ps1 -BillboardOnly`
- Result: generated `src/generated_billboard_assets.h` with bonus and key textures.
- `.\tools\build-windows.ps1`
- Result: passed with zero compiler warnings.

Human validation:

- Run in BlastEm and ares.
- Confirm the screen shows `MEGALDOOM REWRITE GATE 23`.
- Move and turn around the static billboards until both visual types are visible.
- Confirm one object still uses the bonus sprite and another uses the key sprite.
- Confirm distance scaling, transparency, wall/door occlusion and near-over-far draw order still work.
- Confirm movement, doors and collision still work.
- Accepted by human validation.

## Gate 24: runtime billboard state

Implementation status: accepted by human validation.

Scope:

- Keep Gate 23 visuals, movement, doors, type table, depth sorting and wall occlusion.
- Split immutable initial billboard data from mutable runtime billboard state.
- Add `billboard_init()` and call it during startup.
- Add an `active` flag to runtime billboard objects.
- Ignore inactive objects during projection.
- Keep all objects active for this gate.
- Add no pickup behavior, animation, AI, hardware sprite system or dynamic allocation.

Expected visual result:

- Visual behavior should match Gate 23 except for the title text.
- Both bonus and key billboard visuals remain visible somewhere in the map.
- Existing distance scaling, transparency, wall/door occlusion and near-over-far drawing remain.
- Doors, movement and collision still work.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: passed with zero compiler warnings.

Human validation:

- Run in BlastEm and ares.
- Confirm the screen shows `MEGALDOOM REWRITE GATE 24`.
- Confirm visual behavior matches Gate 23.
- Confirm both bonus and key billboard visuals remain visible somewhere in the map.
- Confirm distance scaling, transparency, wall/door occlusion and near-over-far draw order still work.
- Confirm movement, doors and collision still work.
- Accepted by human validation.

## Gate 25: pickup collection by proximity

Implementation status: accepted by human validation.

Scope:

- Keep Gate 24 movement, doors, runtime billboard state, type table, depth sorting and wall occlusion.
- Add proximity collection for active billboard objects.
- Deactivate the first active object within a small radius of the player.
- Trigger redraw when collection happens.
- Keep collection silent: no score, sound, HUD, inventory or item-specific behavior yet.
- Add no animation, AI, hardware sprite system or dynamic allocation.

Expected visual result:

- Walking close to a bonus/key billboard makes it disappear.
- Other active billboards remain visible.
- Existing distance scaling, transparency, wall/door occlusion and near-over-far drawing remain.
- Doors, movement and collision still work.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: passed with zero compiler warnings.

Human validation:

- Run in BlastEm and ares.
- Confirm the screen shows `MEGALDOOM REWRITE GATE 25`.
- Walk directly into a bonus/key billboard.
- Confirm the collected billboard disappears.
- Confirm other active billboards remain visible.
- Confirm distance scaling, transparency, wall/door occlusion and near-over-far draw order still work.
- Confirm movement, doors and collision still work.
- Accepted by human validation.

## Gate 26: pickup counter

Implementation status: accepted by human validation.

Scope:

- Keep Gate 25 movement, doors, runtime billboard state, pickup collection, depth sorting and wall occlusion.
- Count collected billboard objects.
- Show the collected count as a small debug HUD text.
- Update the counter when collection happens.
- Keep item behavior generic: no inventory, score categories, sound or item-specific effects yet.
- Add no animation, AI, hardware sprite system or dynamic allocation.

Expected visual result:

- The HUD/debug text shows `PICKUPS 00` at startup.
- Walking into a bonus/key billboard makes it disappear and increments the counter.
- Other active billboards remain visible.
- Existing distance scaling, transparency, wall/door occlusion and near-over-far drawing remain.
- Doors, movement and collision still work.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: passed with zero compiler warnings.

Human validation:

- Run in BlastEm and ares.
- Confirm the screen shows `MEGALDOOM REWRITE GATE 26`.
- Confirm the HUD/debug text shows `PICKUPS 00` at startup.
- Walk into a bonus/key billboard.
- Confirm the billboard disappears and the counter increments.
- Confirm collecting another billboard increments again.
- Confirm distance scaling, transparency, wall/door occlusion and near-over-far draw order still work.
- Confirm movement, doors and collision still work.
- Accepted by human validation.

## Gate 27: collectible billboard types

Implementation status: accepted by human validation.

Scope:

- Keep Gate 26 movement, doors, pickup counter, runtime billboard state, depth sorting and wall occlusion.
- Add a `collectible` property to billboard type definitions.
- Make proximity collection ignore active objects whose type is not collectible.
- Keep the current bonus and key types collectible for this gate.
- Add no new visual, inventory behavior, score category, animation, AI, hardware sprite system or dynamic allocation.

Expected visual result:

- Visual behavior should match Gate 26 except for the title text.
- The HUD/debug text still shows `PICKUPS 00` at startup.
- Walking into bonus/key billboards still makes them disappear and increments the counter.
- Existing distance scaling, transparency, wall/door occlusion and near-over-far drawing remain.
- Doors, movement and collision still work.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: passed with zero compiler warnings.

Human validation:

- Run in BlastEm and ares.
- Confirm the screen shows `MEGALDOOM REWRITE GATE 27`.
- Confirm the HUD/debug text still shows `PICKUPS 00` at startup.
- Walk into bonus/key billboards.
- Confirm they still disappear and increment the counter.
- Confirm distance scaling, transparency, wall/door occlusion and near-over-far draw order still work.
- Confirm movement, doors and collision still work.
- Accepted by human validation.

## Gate 28: non-collectible decor billboard

Implementation status: accepted by human validation.

Scope:

- Keep Gate 27 movement, doors, pickup counter, runtime billboard state, collectible type flag, depth sorting and wall occlusion.
- Add a third billboard type with `collectible = FALSE`.
- Place one static decorative instance in the map.
- Reuse an existing visual for this gate; add no new asset.
- Keep bonus and key types collectible.
- Add no inventory behavior, score category, animation, AI, hardware sprite system or dynamic allocation.

Expected visual result:

- A decorative billboard appears and renders like a normal billboard.
- Walking into the decorative billboard does not remove it and does not increment `PICKUPS`.
- Walking into bonus/key billboards still removes them and increments `PICKUPS`.
- Existing distance scaling, transparency, wall/door occlusion and near-over-far drawing remain.
- Doors, movement and collision still work.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: passed with zero compiler warnings.

Human validation:

- Run in BlastEm and ares.
- Confirm the screen shows `MEGALDOOM REWRITE GATE 28`.
- Walk into the decorative billboard.
- Confirm it does not disappear and does not increment `PICKUPS`.
- Walk into bonus/key billboards.
- Confirm they still disappear and increment `PICKUPS`.
- Confirm distance scaling, transparency, wall/door occlusion and near-over-far draw order still work.
- Confirm movement, doors and collision still work.
- Accepted by human validation.

## Gate 29: converted decor billboard asset

Implementation status: accepted by human validation.

Scope:

- Keep Gate 28 movement, doors, pickup counter, non-collectible decor type, depth sorting and wall occlusion.
- Convert one Freedoom decorative sprite into the generated billboard asset header.
- Add a decor visual id and map the non-collectible type to it.
- Keep bonus and key assets unchanged.
- Keep pickup rules unchanged.
- Add no inventory behavior, score category, animation, AI, hardware sprite system or dynamic allocation.

Expected visual result:

- The decorative billboard uses its own converted sprite instead of reusing the bonus visual.
- Walking into the decorative billboard still does not remove it and does not increment `PICKUPS`.
- Walking into bonus/key billboards still removes them and increments `PICKUPS`.
- Existing distance scaling, transparency, wall/door occlusion and near-over-far drawing remain.
- Doors, movement and collision still work.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\convert-freedoom-assets.ps1 -BillboardOnly`
- Result: generated `src/generated_billboard_assets.h` with bonus, key and decor textures.
- `.\tools\build-windows.ps1`
- Result: passed with zero compiler warnings.

Human validation:

- Run in BlastEm and ares.
- Confirm the screen shows `MEGALDOOM REWRITE GATE 29`.
- Confirm the decorative billboard no longer looks like the bonus pickup.
- Walk into the decorative billboard.
- Confirm it does not disappear and does not increment `PICKUPS`.
- Walk into bonus/key billboards.
- Confirm they still disappear and increment `PICKUPS`.
- Confirm distance scaling, transparency, wall/door occlusion and near-over-far draw order still work.
- Confirm movement, doors and collision still work.
- Accepted by human validation.

## Gate 30: pickup counters by type

Implementation status: accepted by human validation.

Scope:

- Keep Gate 29 movement, doors, decor asset, pickup counter, collectible rules, depth sorting and wall occlusion.
- Track collected bonus and key pickups separately.
- Replace the single pickup counter text with per-type debug counters.
- Keep decorative billboards excluded from the counters.
- Add no inventory behavior, score category beyond type counts, animation, AI, hardware sprite system or dynamic allocation.

Expected visual result:

- HUD/debug text shows `BONUS 00 KEY 00` at startup.
- Collecting a bonus increments only the bonus counter.
- Collecting a key increments only the key counter.
- Walking into the decorative billboard still does not remove it and does not change either counter.
- Existing distance scaling, transparency, wall/door occlusion and near-over-far drawing remain.
- Doors, movement and collision still work.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: passed with zero compiler warnings.

Human validation:

- Run in BlastEm and ares.
- Confirm the screen shows `MEGALDOOM REWRITE GATE 30`.
- Confirm the HUD/debug text shows `BONUS 00 KEY 00` at startup.
- Collect a bonus pickup and confirm only `BONUS` increments.
- Collect a key pickup and confirm only `KEY` increments.
- Walk into the decorative billboard and confirm neither counter changes.
- Confirm distance scaling, transparency, wall/door occlusion and near-over-far draw order still work.
- Confirm movement, doors and collision still work.
- Accepted by human validation.

## Gate 31: last collected pickup type

Implementation status: accepted by human validation.

Scope:

- Keep Gate 30 movement, doors, decor asset, per-type counters, collectible rules, depth sorting and wall occlusion.
- Track the last collectible pickup type acquired.
- Show the last pickup type in a small debug HUD text.
- Keep decorative billboards excluded from the last-pickup state.
- Add no inventory behavior, key usage, animation, AI, hardware sprite system or dynamic allocation.

Expected visual result:

- HUD/debug text shows `LAST NONE` at startup.
- Collecting a bonus changes the label to `LAST BONUS`.
- Collecting a key changes the label to `LAST KEY`.
- Walking into the decorative billboard does not change the last-pickup label.
- Existing per-type counters, distance scaling, transparency, wall/door occlusion and near-over-far drawing remain.
- Doors, movement and collision still work.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: passed with zero compiler warnings.

Human validation:

- Run in BlastEm and ares.
- Confirm the screen shows `MEGALDOOM REWRITE GATE 31`.
- Confirm the HUD/debug text shows `LAST NONE` at startup.
- Collect a bonus pickup and confirm the label changes to `LAST BONUS`.
- Collect a key pickup and confirm the label changes to `LAST KEY`.
- Walk into the decorative billboard and confirm the label does not change.
- Confirm per-type counters, distance scaling, transparency, wall/door occlusion and near-over-far draw order still work.
- Confirm movement, doors and collision still work.
- Accepted by human validation.

## Gate 32: locked door requires key

Implementation status: accepted by human validation.

Scope:

- Keep Gate 31 movement, billboard pickups, last-pickup HUD, decor asset, per-type counters, collectible rules, depth sorting and wall occlusion.
- Add a locked door tile type distinct from the normal door tile.
- Require at least one collected key before a locked door can open or close.
- Keep normal doors working without any key.
- Do not consume the key yet.
- Add a distinct debug-facing locked-door wall texture.
- Add no inventory menu, sound, animation, enemy logic, hardware sprite system or dynamic allocation.

Expected visual result:

- Screen title shows `MEGALDOOM REWRITE GATE 32`.
- Subtitle shows `LOCKED DOOR`.
- One door in the map uses a different texture from the normal door.
- Pressing use on the locked door without a collected key does nothing.
- After collecting a key, pressing use on the locked door opens and closes it.
- Normal doors still open and close without requiring any key.
- Last-pickup label, bonus/key counters, billboard scaling, transparency, wall/door occlusion and near-over-far draw order remain stable.
- Movement and collision still work.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: pending.

Human validation:

- Run in BlastEm and ares.
- Confirm the screen shows `MEGALDOOM REWRITE GATE 32`.
- Find the locked door with the alternate texture.
- Before collecting a key, face the locked door and press use; confirm it does not open.
- Confirm a normal door still opens without any key.
- Collect a key pickup and confirm `KEY` increments and `LAST KEY` appears.
- Return to the locked door and confirm use now opens and closes it.
- Confirm the key count is not consumed yet.
- Confirm bonus/key counters, last-pickup label, billboard scaling, transparency, wall/door occlusion and near-over-far draw order still work.
- Confirm movement and collision still work.
- Accepted by human validation.

## Gate 33: key is consumed when unlocking

Implementation status: accepted by human validation.

Scope:

- Keep Gate 32 movement, locked door requirement, billboard pickups, last-pickup HUD, decor asset, per-type counters, collectible rules, depth sorting and wall occlusion.
- Consume one held key when the player unlocks a locked door for the first time.
- After unlock, convert that door to normal door behavior so later open/close does not consume another key.
- Keep normal doors working without any key.
- Keep bonus count and last-pickup label behavior unchanged.
- Add no inventory menu, sound, animation, enemy logic, hardware sprite system or dynamic allocation.

Expected visual result:

- Screen title shows `MEGALDOOM REWRITE GATE 33`.
- Subtitle shows `KEY UNLOCK`.
- Before collecting a key, the locked door still does nothing.
- After collecting one key, unlocking the locked door spends that key immediately.
- The `KEY` HUD value decreases by one after unlock.
- Once unlocked, that same door behaves like a normal door and can open/close again without another key.
- Normal doors still open and close without any key.
- Bonus counter, last-pickup label, billboard scaling, transparency, wall/door occlusion and near-over-far draw order remain stable.
- Movement and collision still work.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: pending.

Human validation:

- Run in BlastEm and ares.
- Confirm the screen shows `MEGALDOOM REWRITE GATE 33`.
- Confirm the subtitle shows `KEY UNLOCK`.
- Find the locked door with the alternate texture.
- Before collecting a key, face the locked door and press use; confirm it does not open.
- Collect one key pickup and confirm `KEY` increments and `LAST KEY` appears.
- Return to the locked door and press use; confirm the door unlocks/opens and `KEY` decreases by one.
- Press use on that same door again and confirm it now behaves like a normal door without spending another key.
- Confirm a normal non-locked door still opens without any key.
- Confirm bonus counter, last-pickup label, billboard scaling, transparency, wall/door occlusion and near-over-far draw order still work.
- Confirm movement and collision still work.
- Accepted by human validation.

## Gate 34: use feedback HUD

Implementation status: accepted by human validation.

Scope:

- Keep Gate 33 movement, locked-door unlock/consumption, billboard pickups, last-pickup HUD, decor asset, per-type counters, collectible rules, depth sorting and wall occlusion.
- Change door interaction to return an explicit action result instead of a plain boolean.
- Show a small debug HUD feedback line for the last use action.
- Distinguish at least normal door toggle, locked-without-key, and successful key unlock.
- Keep the feedback persistent; no timeout logic in this gate.
- Add no inventory menu, sound, animation, enemy logic, hardware sprite system or dynamic allocation.

Expected visual result:

- Screen title shows `MEGALDOOM REWRITE GATE 34`.
- Subtitle shows `USE FEEDBACK`.
- HUD shows `USE ----` at startup.
- Using a normal door changes the feedback to `USE DOOR`.
- Trying a locked door without key changes the feedback to `USE LOCK`.
- Unlocking a locked door with a key changes the feedback to `USE KEY!`.
- Key consumption from Gate 33 still works.
- Bonus counter, last-pickup label, billboard scaling, transparency, wall/door occlusion and near-over-far draw order remain stable.
- Movement and collision still work.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: pending.

Human validation:

- Run in BlastEm and ares.
- Confirm the screen shows `MEGALDOOM REWRITE GATE 34`.
- Confirm the subtitle shows `USE FEEDBACK`.
- Confirm the HUD shows `USE ----` at startup.
- Use a normal door and confirm the HUD changes to `USE DOOR`.
- Face a locked door without key and confirm the HUD changes to `USE LOCK`.
- Collect a key, unlock the locked door, and confirm the HUD changes to `USE KEY!`.
- Confirm the `KEY` counter still decreases by one on unlock.
- Confirm that unlocked door still behaves like a normal door afterward.
- Confirm bonus counter, last-pickup label, billboard scaling, transparency, wall/door occlusion and near-over-far draw order still work.
- Confirm movement and collision still work.
- Accepted by human validation.

## Gate 35: exit switch objective

Implementation status: accepted by human validation.

Scope:

- Keep Gate 34 movement, locked-door unlock/consumption, billboard pickups, use feedback HUD, decor asset, per-type counters, collectible rules, depth sorting and wall occlusion.
- Add a visible exit switch tile in the map.
- Trigger a simple level-clear state when the player uses the exit switch.
- Show a small goal-status HUD line with seek/clear state.
- Freeze further movement after level clear for unambiguous validation.
- Add no next-level transition, inventory menu, sound, animation, enemy logic, hardware sprite system or dynamic allocation.

Expected visual result:

- Screen title shows `MEGALDOOM REWRITE GATE 35`.
- Subtitle shows `EXIT SWITCH`.
- HUD shows `GOAL SEEK` at startup.
- A distinct switch-like wall tile is visible in the map.
- Using the switch changes the use HUD to `USE EXIT`.
- Using the switch changes the goal HUD to `GOAL CLEAR`.
- After clear, movement input no longer changes the scene.
- Existing key unlock flow still works.
- Bonus counter, last-pickup label, billboard scaling, transparency, wall/door occlusion and near-over-far draw order remain stable.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: pending.

Human validation:

- Run in BlastEm and ares.
- Confirm the screen shows `MEGALDOOM REWRITE GATE 35`.
- Confirm the subtitle shows `EXIT SWITCH`.
- Confirm the HUD shows `GOAL SEEK` at startup.
- Find the distinct exit switch tile in the map.
- Use the locked-door path as before if needed to reach the switch.
- Face the switch and press use; confirm the HUD changes to `USE EXIT`.
- Confirm the goal line changes to `GOAL CLEAR`.
- Confirm movement input no longer changes the scene after clear.
- Confirm the key/door flow from earlier gates still works before clear.
- Confirm bonus counter, last-pickup label, billboard scaling, transparency, wall/door occlusion and near-over-far draw order still work.
- Accepted by human validation.

## Gate 36: hitscan target shot

Implementation status: accepted by human validation.

Scope:

- Keep Gate 35 movement, exit switch objective, locked-door unlock/consumption, billboard pickups, use feedback HUD, decor asset, per-type counters, collectible rules, depth sorting and wall occlusion.
- Split `use` and `fire` onto separate buttons.
- Add a minimal hitscan shot using the center ray depth as wall occlusion.
- Allow the shot to remove only the decorative target billboard for this gate.
- Show a small debug HUD line for the last shot result.
- Add no weapon sprite, ammo, sound, animation, enemy logic, damage system, hardware sprite system or dynamic allocation.

Expected visual result:

- Screen title shows `MEGALDOOM REWRITE GATE 36`.
- Subtitle shows `HITSCAN TARGET`.
- HUD shows `SHOT ----` at startup.
- Controls text shows `START USE  B FIRE`.
- Pressing `B` with no decor target in the crosshair changes the HUD to `SHOT MISS`.
- Pressing `B` with the decor billboard centered and not occluded changes the HUD to `SHOT HIT`.
- On hit, the decor billboard disappears.
- Pickups are not removed by shooting in this gate.
- Existing key/door/exit flow still works, now using `START` for use.
- Bonus counter, last-pickup label, billboard scaling, transparency, wall/door occlusion and near-over-far draw order remain stable.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: pending.

Human validation:

- Run in BlastEm and ares.
- Confirm the screen shows `MEGALDOOM REWRITE GATE 36`.
- Confirm the subtitle shows `HITSCAN TARGET`.
- Confirm the HUD shows `SHOT ----` at startup.
- Confirm the controls text shows `START USE  B FIRE`.
- Press `B` while not aiming at the decor billboard and confirm the HUD changes to `SHOT MISS`.
- Aim at the decor billboard and press `B`; confirm the HUD changes to `SHOT HIT`.
- Confirm the decor billboard disappears after a hit.
- Confirm bonus and key pickups are still collected by proximity, not removed by shooting.
- Confirm locked door and exit switch still work through `START`.
- Confirm bonus counter, last-pickup label, billboard scaling, transparency, wall/door occlusion and near-over-far draw order still work.
- Accepted by human validation.

## Gate 37: shot cooldown HUD

Implementation status: accepted by human validation.

Scope:

- Keep Gate 36 movement, exit switch objective, locked-door unlock/consumption, billboard pickups, hitscan shot, use feedback HUD, decor asset, per-type counters, collectible rules, depth sorting and wall occlusion.
- Add a fixed cooldown after each shot.
- Block repeated firing while cooldown is active.
- Show the cooldown value in a small debug HUD line.
- Keep the current hit/miss shot status line.
- Add no weapon sprite, ammo, sound, animation, enemy logic, damage system, hardware sprite system or dynamic allocation.

Expected visual result:

- Screen title shows `MEGALDOOM REWRITE GATE 37`.
- Subtitle shows `SHOT COOLDOWN`.
- HUD shows `COOL 00` at startup.
- Pressing `B` fires once and changes `COOL` to a non-zero value.
- The cooldown value counts back down to `00`.
- Pressing `B` repeatedly during cooldown does not remove a second target.
- Once `COOL 00` returns, firing works again.
- Existing `SHOT HIT` and `SHOT MISS` feedback still works.
- Existing key/door/exit flow still works through `START`.
- Bonus counter, last-pickup label, billboard scaling, transparency, wall/door occlusion and near-over-far draw order remain stable.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: pending.

Human validation:

- Run in BlastEm and ares.
- Confirm the screen shows `MEGALDOOM REWRITE GATE 37`.
- Confirm the subtitle shows `SHOT COOLDOWN`.
- Confirm the HUD shows `COOL 00` at startup.
- Press `B` once and confirm `COOL` jumps above `00`.
- Confirm the cooldown number counts back down to `00`.
- While cooldown is above `00`, press `B` repeatedly and confirm no second shot effect happens.
- At `COOL 00`, shoot again and confirm firing works normally.
- Confirm `SHOT HIT` and `SHOT MISS` still behave as before.
- Confirm locked door and exit switch still work through `START`.
- Confirm bonus counter, last-pickup label, billboard scaling, transparency, wall/door occlusion and near-over-far draw order still work.
- Accepted by human validation.

## Gate 38: visible weapon flash

Implementation status: accepted by human validation.

Scope:

- Keep Gate 37 movement, exit switch objective, locked-door unlock/consumption, billboard pickups, hitscan shot, shot cooldown HUD, use feedback HUD, decor asset, per-type counters, collectible rules, depth sorting and wall occlusion.
- Add a simple screen-space weapon overlay at the bottom of the view.
- Add a short muzzle flash when a shot is actually fired.
- Keep the weapon visual procedural; add no sprite asset pipeline in this gate.
- Add no ammo, sound, animation beyond the brief flash timer, enemy logic, damage system, hardware sprite system or dynamic allocation.

Expected visual result:

- Screen title shows `MEGALDOOM REWRITE GATE 38`.
- Subtitle shows `WEAPON FLASH`.
- A simple weapon shape is visible at the bottom center of the 3D view.
- Pressing `B` when cooldown is `00` briefly brightens the weapon muzzle area.
- The flash disappears automatically after a short moment.
- Pressing `B` during cooldown does not retrigger the flash.
- Existing cooldown HUD, `SHOT HIT`/`SHOT MISS`, key/door/exit flow and pickups still work.
- Bonus counter, last-pickup label, billboard scaling, transparency, wall/door occlusion and near-over-far draw order remain stable.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: pending.

Human validation:

- Run in BlastEm and ares.
- Confirm the screen shows `MEGALDOOM REWRITE GATE 38`.
- Confirm the subtitle shows `WEAPON FLASH`.
- Confirm a weapon shape is visible at the bottom center of the view.
- Press `B` at `COOL 00` and confirm the muzzle area flashes briefly.
- Confirm the flash disappears automatically.
- Press `B` during cooldown and confirm the flash does not retrigger.
- Confirm `SHOT HIT` and `SHOT MISS` still behave as before.
- Confirm locked door and exit switch still work through `START`.
- Confirm bonus counter, last-pickup label, billboard scaling, transparency, wall/door occlusion and near-over-far draw order still work.
- Accepted by human validation.

## Gate 39: two-hit target

Implementation status: accepted by human validation.

Scope:

- Keep Gate 38 movement, exit switch objective, locked-door unlock/consumption, billboard pickups, hitscan shot, shot cooldown HUD, visible weapon flash, use feedback HUD, decor asset, per-type counters, collectible rules, depth sorting and wall occlusion.
- Give the decor target two hit points.
- Keep the target present after the first hit.
- Change the target visual on the damaged state.
- Show the target HP in a small debug HUD line.
- Add no ammo, sound, enemy logic, damage to player, hardware sprite system or dynamic allocation.

Expected visual result:

- Screen title shows `MEGALDOOM REWRITE GATE 39`.
- Subtitle shows `TARGET 2 HIT`.
- HUD shows `TARGET 02` at startup.
- First valid shot on the target changes the shot label to `SHOT DMG`.
- After the first hit, `TARGET` drops to `01` and the target remains visible with a damaged-looking recolor.
- Second valid shot changes the shot label to `SHOT KILL`.
- After the second hit, `TARGET` drops to `00` and the target disappears.
- Existing weapon flash, cooldown HUD, key/door/exit flow and pickups still work.
- Bonus counter, last-pickup label, billboard scaling, transparency, wall/door occlusion and near-over-far draw order remain stable.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: pending.

Human validation:

- Run in BlastEm and ares.
- Confirm the screen shows `MEGALDOOM REWRITE GATE 39`.
- Confirm the subtitle shows `TARGET 2 HIT`.
- Confirm the HUD shows `TARGET 02` at startup.
- Hit the decor target once and confirm `SHOT DMG`.
- Confirm `TARGET` changes to `01`.
- Confirm the target remains visible after the first hit and appears recolored/damaged.
- Wait for cooldown and hit the same target again; confirm `SHOT KILL`.
- Confirm `TARGET` changes to `00`.
- Confirm the target disappears after the second hit.
- Confirm locked door and exit switch still work through `START`.
- Confirm bonus counter, last-pickup label, billboard scaling, transparency, wall/door occlusion and near-over-far draw order still work.
- Accepted by human validation.

## Gate 40: multiple targets

Implementation status: accepted by human validation.

Scope:

- Keep Gate 39 movement, exit switch objective, locked-door unlock/consumption, billboard pickups, two-hit target logic, hitscan shot, shot cooldown HUD, visible weapon flash, use feedback HUD, per-type counters, collectible rules, depth sorting and wall occlusion.
- Add a second destructible decor target.
- Track remaining target count in the HUD.
- Keep per-target HP behavior from Gate 39.
- Add no ammo, sound, enemy AI, damage to player, hardware sprite system or dynamic allocation.

Expected visual result:

- Screen title shows `MEGALDOOM REWRITE GATE 40`.
- Subtitle shows `MULTI TARGET`.
- HUD shows `TGT 02 HP 02` at startup.
- Killing one target changes the target count to `TGT 01`.
- The remaining target still has its own HP flow and can show `HP 02` then `HP 01`.
- After killing both targets, the HUD reaches `TGT 00 HP 00`.
- Existing weapon flash, cooldown HUD, key/door/exit flow and pickups still work.
- Bonus counter, last-pickup label, billboard scaling, transparency, wall/door occlusion and near-over-far draw order remain stable.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: pending.

Human validation:

- Run in BlastEm and ares.
- Confirm the screen shows `MEGALDOOM REWRITE GATE 40`.
- Confirm the subtitle shows `MULTI TARGET`.
- Confirm the HUD shows `TGT 02 HP 02` at startup.
- Kill one target fully and confirm the HUD changes to `TGT 01`.
- Confirm the remaining target can still be damaged and killed in two shots.
- Confirm the HUD eventually reaches `TGT 00 HP 00` after both kills.
- Confirm `SHOT DMG` and `SHOT KILL` still behave correctly.
- Confirm locked door and exit switch still work through `START`.
- Confirm bonus counter, last-pickup label, billboard scaling, transparency, wall/door occlusion and near-over-far draw order still work.
- Accepted by human validation.

## Gate 41: exit requires all targets

Implementation status: accepted by human validation.

Scope:

- Keep Gate 40 movement, multiple destructible targets, exit switch objective, locked-door unlock/consumption, billboard pickups, hitscan shot, shot cooldown HUD, visible weapon flash, use feedback HUD, per-type counters, collectible rules, depth sorting and wall occlusion.
- Require all destructible targets to be cleared before the exit switch can finish the level.
- Reuse the existing target-count HUD to signal progress.
- Add a distinct use-feedback state when the player tries the exit too early.
- Add no ammo, sound, enemy AI, damage to player, hardware sprite system or dynamic allocation.

Expected visual result:

- Screen title shows `MEGALDOOM REWRITE GATE 41`.
- Subtitle shows `CLEAR EXIT`.
- Before all targets are dead, using the exit switch changes the use HUD to `USE HUNT`.
- Before all targets are dead, the goal does not change to `CLEAR`.
- After all targets are dead and the HUD shows `TGT 00`, using the exit switch changes the use HUD to `USE EXIT`.
- After all targets are dead, the goal changes to `GOAL CLEAR`.
- Existing weapon flash, cooldown HUD, key/door flow and pickups still work.
- Bonus counter, last-pickup label, billboard scaling, transparency, wall/door occlusion and near-over-far draw order remain stable.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: pending.

Human validation:

- Run in BlastEm and ares.
- Confirm the screen shows `MEGALDOOM REWRITE GATE 41`.
- Confirm the subtitle shows `CLEAR EXIT`.
- Before killing all targets, use the exit switch and confirm the use HUD changes to `USE HUNT`.
- Confirm the goal does not change to `GOAL CLEAR` yet.
- Kill all targets until the HUD shows `TGT 00 HP 00`.
- Use the exit switch again and confirm the use HUD changes to `USE EXIT`.
- Confirm the goal changes to `GOAL CLEAR`.
- Confirm locked door still works through `START`.
- Confirm bonus counter, last-pickup label, billboard scaling, transparency, wall/door occlusion and near-over-far draw order still work.
- Accepted by human validation.

## Gate 42: restart level after clear

Implementation status: accepted by human validation.

Scope:

- Keep Gate 41 movement, clear-gated exit, multiple destructible targets, locked-door unlock/consumption, billboard pickups, hitscan shot, shot cooldown HUD, visible weapon flash, use feedback HUD, per-type counters, collectible rules, depth sorting and wall occlusion.
- Allow the player to restart the current level after reaching `GOAL CLEAR`.
- Reset player position, doors, targets, pickups, HUD counters and temporary combat state.
- Keep restart input on `START`, which already becomes free once the level is cleared and movement is frozen.
- Add no phase selection, save system, enemy AI, hardware sprite system or dynamic allocation.

Expected visual result:

- Screen title shows `MEGALDOOM REWRITE GATE 42`.
- Subtitle shows `LEVEL RESET`.
- After clearing the level, pressing `START` resets the whole stage.
- After reset, the HUD returns to startup values such as `GOAL SEEK`, `TGT 02 HP 02`, `COOL 00`, `SHOT MISS`/default and pickup counters at zero.
- The player view returns to the initial spawn position.
- Locked door, targets and pickups reappear in their initial states.
- Existing key/door/shot/cooldown/exit flow still works after the reset.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: pending.

Human validation:

- Run in BlastEm and ares.
- Confirm the screen shows `MEGALDOOM REWRITE GATE 42`.
- Confirm the subtitle shows `LEVEL RESET`.
- Finish the level until the HUD shows `GOAL CLEAR`.
- Press `START` after clear and confirm the whole stage resets.
- Confirm the HUD returns to `GOAL SEEK` and `TGT 02 HP 02`.
- Confirm the player is back at the initial spawn position.
- Confirm the locked door, targets and pickups are restored.
- Confirm the level can be played and cleared again from the reset state.
- Accepted by human validation.

## Gate 43: phase loop

Implementation status: accepted by human validation.

Scope:

- Keep Gate 42 movement, reset-after-clear flow, clear-gated exit, multiple destructible targets, locked-door unlock/consumption, billboard pickups, hitscan shot, shot cooldown HUD, visible weapon flash, use feedback HUD, per-type counters, collectible rules, depth sorting and wall occlusion.
- Add a second playable phase with its own map layout, spawn point and object placement.
- Show the current phase in a small debug HUD line.
- After clearing a phase, pressing `START` advances to the next phase instead of reloading the same one.
- Loop back to phase 1 after clearing phase 2.
- Add no enemy AI, save system, hardware sprite system or dynamic allocation.

Expected visual result:

- Screen title shows `MEGALDOOM REWRITE GATE 43`.
- Subtitle shows `PHASE LOOP`.
- HUD shows `PH 01` at startup.
- After clearing phase 1 and pressing `START`, the game loads a visibly different phase and the HUD shows `PH 02`.
- Phase 2 starts from a different spawn position and uses a different map/object arrangement.
- After clearing phase 2 and pressing `START`, the game loops back to phase 1 and the HUD shows `PH 01`.
- Existing weapon flash, cooldown HUD, key/door flow, target gating and pickups still work in both phases.
- Bonus counter, last-pickup label, billboard scaling, transparency, wall/door occlusion and near-over-far draw order remain stable.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: pending.

Human validation:

- Run in BlastEm and ares.
- Confirm the screen shows `MEGALDOOM REWRITE GATE 43`.
- Confirm the subtitle shows `PHASE LOOP`.
- Confirm the HUD shows `PH 01` at startup.
- Clear phase 1 and press `START`; confirm the HUD changes to `PH 02`.
- Confirm phase 2 has a different spawn position and visibly different target/map arrangement.
- Clear phase 2 and press `START`; confirm the HUD returns to `PH 01`.
- Confirm the key/door/exit/shot/target loop still works in both phases.
- Accepted by human validation.

## Gate 44: dummy enemy

Implementation status: ready for human validation.

Scope:

- Keep Gate 43 movement, phase loop, clear-gated exit, multiple destructible targets, locked-door unlock/consumption, billboard pickups, hitscan shot, shot cooldown HUD, visible weapon flash, use feedback HUD, per-type counters, collectible rules, depth sorting and wall occlusion.
- Add one static dummy enemy per phase.
- Give the dummy its own billboard visual recolor and a larger HP pool than decor targets.
- Show dummy count in a small debug HUD line.
- Count the dummy toward the existing target-clear requirement.
- Add no enemy movement, return fire, pathfinding, sound, hardware sprite system or dynamic allocation.

Expected visual result:

- Screen title shows `MEGALDOOM REWRITE GATE 44`.
- Subtitle shows `DUMMY ENEMY`.
- HUD shows `ENMY 01` at startup.
- A dummy enemy billboard appears with a distinct color treatment from the decor targets.
- Hitting the dummy damages it over multiple shots instead of removing it immediately.
- Killing the dummy changes the HUD to `ENMY 00`.
- Because the dummy is targetable, the exit still requires killing it before the level can be cleared.
- Existing weapon flash, cooldown HUD, key/door flow, phase loop and pickups still work.
- Bonus counter, last-pickup label, billboard scaling, transparency, wall/door occlusion and near-over-far draw order remain stable.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: pending.

Human validation:

- Run in BlastEm and ares.
- Confirm the screen shows `MEGALDOOM REWRITE GATE 44`.
- Confirm the subtitle shows `DUMMY ENEMY`.
- Confirm the HUD shows `ENMY 01` at startup.
- Find the dummy enemy billboard and confirm it looks distinct from decor targets.
- Shoot the dummy and confirm it survives more than one hit.
- Kill the dummy and confirm the HUD changes to `ENMY 00`.
- Confirm the exit still requires killing all targets including the dummy.
- Confirm the phase loop still works after clearing the level.

## Gate 45: dummy chase and contact attack

Implementation status: ready for human validation.

Scope:

- Keep Gate 44 phase loop, clear-gated exit, multiple destructible targets, locked-door unlock/consumption, billboard pickups, hitscan shot, shot cooldown HUD, visible weapon flash, use feedback HUD, per-type counters, collectible rules, depth sorting and wall occlusion.
- Give the dummy a simple chase step toward the player when inside a short range.
- Add a simple contact attack with a fixed cooldown.
- Show player HP in a small debug HUD line.
- Reset the current phase when player HP reaches zero.
- Keep the dummy logic grid-simple: no pathfinding, no projectile attack, no sound, no hardware sprite system and no dynamic allocation.

Expected visual result:

- Screen title shows `MEGALDOOM REWRITE GATE 45`.
- Subtitle shows `DUMMY CHASE`.
- HUD shows `ENMY 01` and `HP 03` at startup.
- When the player comes near the dummy, it starts moving in simple steps toward the player.
- When the dummy reaches close contact, player HP drops with a visible cooldown between hits.
- If the dummy drains all HP, the current phase resets to its initial state.
- Existing weapon flash, cooldown HUD, key/door flow, target gating and pickups still work.
- Bonus counter, last-pickup label, billboard scaling, transparency, wall/door occlusion and near-over-far draw order remain stable.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: pending.

Human validation:

- Run in BlastEm and ares.
- Confirm the screen shows `MEGALDOOM REWRITE GATE 45`.
- Confirm the subtitle shows `DUMMY CHASE`.
- Confirm the HUD shows `HP 03` at startup.
- Approach the dummy and confirm it starts moving toward the player.
- Let the dummy touch the player and confirm HP drops, but not every single frame.
- Let HP reach zero and confirm the current phase resets.
- Confirm the dummy can still be shot and killed normally.
- Confirm the exit still requires killing all targets including the dummy.

## Gate 46: player damage flash

Implementation status: ready for human validation.

Scope:

- Keep Gate 45 dummy chase, contact attack, phase loop, clear-gated exit, locked-door unlock/consumption, billboard pickups, hitscan shot, shot cooldown HUD, visible weapon flash, use feedback HUD, per-type counters, collectible rules, depth sorting and wall occlusion.
- Add a brief visual hit flash on the 3D view when the player takes damage.
- Reuse the existing player HP flow; add no armor, healing, sound, screen shake, projectile attack, hardware sprite system or dynamic allocation.

Expected visual result:

- Screen title shows `MEGALDOOM REWRITE GATE 46`.
- Subtitle shows `PLAYER HIT FLASH`.
- HUD still shows `HP 03` at startup.
- When the dummy lands a hit, the 3D view briefly flashes with a visible red damage frame.
- The flash fades automatically after a short moment.
- HP drain, death reset, shooting, pickups, keys, exit gating and phase loop still work.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: pending.

Human validation:

- Run in BlastEm and ares.
- Confirm the screen shows `MEGALDOOM REWRITE GATE 46`.
- Confirm the subtitle shows `PLAYER HIT FLASH`.
- Let the dummy hit the player and confirm the red flash appears on the 3D view.
- Confirm the flash disappears automatically after a short time.
- Confirm repeated hits retrigger the flash.
- Confirm HP still drops correctly and the phase still resets at zero HP.

## Gate 47: dummy hit pushback

Implementation status: ready for human validation.

Scope:

- Keep Gate 46 player damage flash, dummy chase/contact attack, phase loop, clear-gated exit, locked-door unlock/consumption, billboard pickups, hitscan shot, shot cooldown HUD, visible weapon flash, use feedback HUD, per-type counters, collectible rules, depth sorting and wall occlusion.
- When the dummy takes a shot, apply a simple pushback away from the player if the space is free.
- Briefly pause dummy chase immediately after a hit so the pushback is readable.
- Add no ragdoll, animation set, pathfinding, projectile logic, sound, hardware sprite system or dynamic allocation.

Expected visual result:

- Screen title shows `MEGALDOOM REWRITE GATE 47`.
- Subtitle shows `DUMMY HIT PUSH`.
- Shooting a dummy should still reduce its HP as before.
- On a valid hit, the dummy should visibly step backward away from the player when space allows.
- Right after the hit, the dummy should not instantly re-stick to the player on the very next frame.
- Existing player damage flash, HP drain, death reset, shooting, pickups, keys, exit gating and phase loop still work.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: pending.

Human validation:

- Run in BlastEm and ares.
- Confirm the screen shows `MEGALDOOM REWRITE GATE 47`.
- Confirm the subtitle shows `DUMMY HIT PUSH`.
- Approach a dummy and shoot it at close or medium range.
- Confirm the dummy is pushed backward on hit when there is open space.
- Confirm the dummy still dies after the expected number of hits.
- Confirm the dummy resumes chasing after the short pause.

## Gate 48: dummy line of sight

Implementation status: ready for human validation.

Scope:

- Keep Gate 47 dummy hit pushback, player damage flash, dummy chase/contact attack, phase loop, clear-gated exit, locked-door unlock/consumption, billboard pickups, hitscan shot, shot cooldown HUD, visible weapon flash, use feedback HUD, per-type counters, collectible rules, depth sorting and wall occlusion.
- Gate dummy chase and contact attack behind a simple world-space line-of-sight check.
- Closed walls and doors should block dummy aggro.
- Add no hearing system, pathfinding around corners, projectile logic, sound, hardware sprite system or dynamic allocation.

Expected visual result:

- Screen title shows `MEGALDOOM REWRITE GATE 48`.
- Subtitle shows `DUMMY LOS`.
- If the player is visible in open space, the dummy still chases and attacks as before.
- If a wall or closed door is between the player and the dummy, it should stop chasing and stop dealing contact damage through the obstruction.
- Existing hit pushback, player damage flash, HP drain, death reset, shooting, pickups, keys, exit gating and phase loop still work.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: pending.

Human validation:

- Run in BlastEm and ares.
- Confirm the screen shows `MEGALDOOM REWRITE GATE 48`.
- Confirm the subtitle shows `DUMMY LOS`.
- Stand in open sight of the dummy and confirm it still chases.
- Put a wall or closed door between the player and the dummy and confirm it stops chasing.
- Re-open the line of sight and confirm it resumes normal behavior.

## Gate 49: multiple dummies

Implementation status: ready for human validation.

Scope:

- Keep Gate 48 dummy line of sight, hit pushback, player damage flash, dummy chase/contact attack, phase loop, clear-gated exit, locked-door unlock/consumption, billboard pickups, hitscan shot, shot cooldown HUD, visible weapon flash, use feedback HUD, per-type counters, collectible rules, depth sorting and wall occlusion.
- Add a second dummy enemy to each phase.
- Increase the billboard span budget so multiple active enemies do not get clipped out of the renderer too early.
- Reuse the existing enemy count, target gating and damage systems.
- Add no new enemy type, no pathfinding, no projectile logic, no sound, no hardware sprite system and no dynamic allocation.

Expected visual result:

- Screen title shows `MEGALDOOM REWRITE GATE 49`.
- Subtitle shows `MULTI DUMMY`.
- HUD should now start with `ENMY 02` in each phase.
- Two dummies can be found, damaged, pushed back and killed independently.
- Both dummies count toward the existing target-clear requirement and can both chase/attack when they have line of sight.
- Existing player damage flash, HP drain, death reset, shooting, pickups, keys, exit gating and phase loop still work.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: pending.

Human validation:

- Run in BlastEm and ares.
- Confirm the screen shows `MEGALDOOM REWRITE GATE 49`.
- Confirm the subtitle shows `MULTI DUMMY`.
- Confirm the HUD starts with `ENMY 02`.
- Find both dummies in the phase and confirm both can chase and attack.
- Shoot one dummy and confirm only that one takes damage/pushback.
- Kill both and confirm `ENMY 00`.
- Confirm the exit still requires killing all targets including both dummies.

## Gate 50: player invulnerability window

Implementation status: ready for human validation.

Scope:

- Keep Gate 49 multiple dummies, dummy line of sight, hit pushback, player damage flash, dummy chase/contact attack, phase loop, clear-gated exit, locked-door unlock/consumption, billboard pickups, hitscan shot, shot cooldown HUD, visible weapon flash, use feedback HUD, per-type counters, collectible rules, depth sorting and wall occlusion.
- After the player takes contact damage, add a short invulnerability window.
- During that window, further enemy contact should not drain additional HP.
- Keep the existing HP, reset and damage-flash behavior.
- Add no dodge move, armor system, healing item, knockback-on-player, sound, hardware sprite system or dynamic allocation.

Expected visual result:

- Screen title shows `MEGALDOOM REWRITE GATE 50`.
- Subtitle shows `PLAYER I-FRAME`.
- When one or more dummies touch the player, HP should drop by only one step, then pause before another valid hit can land.
- The existing damage flash still appears on the accepted hit.
- With two dummies crowding the player, HP should not melt instantly in consecutive frames.
- Death reset, shooting, pickups, keys, exit gating and phase loop still work.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: pending.

Human validation:

- Run in BlastEm and ares.
- Confirm the screen shows `MEGALDOOM REWRITE GATE 50`.
- Confirm the subtitle shows `PLAYER I-FRAME`.
- Let one dummy hit the player and confirm HP drops by one.
- Stay in contact and confirm HP does not keep dropping every immediate frame.
- Let two dummies crowd the player and confirm the same protection still holds.
- Confirm another hit can land again after the short protection window ends.

## Gate 51: dummy return to guard

Implementation status: ready for human validation.

Scope:

- Keep Gate 50 player invulnerability window, multiple dummies, dummy line of sight, hit pushback, player damage flash, dummy chase/contact attack, phase loop, clear-gated exit, locked-door unlock/consumption, billboard pickups, hitscan shot, shot cooldown HUD, visible weapon flash, use feedback HUD, per-type counters, collectible rules, depth sorting and wall occlusion.
- Store a simple guard position for each dummy.
- When the dummy loses sight of the player, make it walk back toward its guard position.
- Keep the return behavior grid-simple: no pathfinding around complex obstructions, no animation system, no sound and no dynamic allocation.

Expected visual result:

- Screen title shows `MEGALDOOM REWRITE GATE 51`.
- Subtitle shows `DUMMY RETURN`.
- A dummy that chases away from its starting position should walk back after losing line of sight or after the player leaves chase range.
- Once back near its original spot, it should stop there.
- Existing chase, attack, hit pushback, player i-frame, damage flash, death reset, shooting, pickups, keys, exit gating and phase loop still work.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: pending.

Human validation:

- Run in BlastEm and ares.
- Confirm the screen shows `MEGALDOOM REWRITE GATE 51`.
- Confirm the subtitle shows `DUMMY RETURN`.
- Pull a dummy away from its start position.
- Break line of sight or leave chase range and confirm it walks back.
- Confirm it settles near its original guard position.

## Gate 52: player hit knockback

Implementation status: ready for human validation.

Scope:

- Keep Gate 51 dummy return to guard, player invulnerability window, multiple dummies, dummy line of sight, hit pushback, player damage flash, dummy chase/contact attack, phase loop, clear-gated exit, locked-door unlock/consumption, billboard pickups, hitscan shot, shot cooldown HUD, visible weapon flash, use feedback HUD, per-type counters, collectible rules, depth sorting and wall occlusion.
- On an accepted dummy hit, apply a small knockback to the player away from the attacker.
- Reuse existing collision so the player does not get pushed through walls.
- Add no stun state, no player attack cancel, no sound, no sprite animation and no dynamic allocation.

Expected visual result:

- Screen title shows `MEGALDOOM REWRITE GATE 52`.
- Subtitle shows `PLAYER PUSH`.
- When a dummy lands a hit, the player view jolts backward a small step if space allows.
- The push should respect walls and doors instead of clipping through them.
- Existing i-frame, damage flash, death reset, dummy return/chase, shooting, pickups, keys, exit gating and phase loop still work.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: pending.

Human validation:

- Run in BlastEm and ares.
- Confirm the screen shows `MEGALDOOM REWRITE GATE 52`.
- Confirm the subtitle shows `PLAYER PUSH`.
- Let a dummy hit the player and confirm the camera is pushed back slightly.
- Confirm the push does not pass through a nearby wall or closed door.
- Confirm repeated accepted hits still combine correctly with the existing invulnerability window.

## Gate 53: dummy guard patrol

Implementation status: ready for human validation.

Scope:

- Keep Gate 52 player hit knockback, dummy return to guard, player invulnerability window, multiple dummies, dummy line of sight, hit pushback, player damage flash, dummy chase/contact attack, phase loop, clear-gated exit, locked-door unlock/consumption, billboard pickups, hitscan shot, shot cooldown HUD, visible weapon flash, use feedback HUD, per-type counters, collectible rules, depth sorting and wall occlusion.
- Add a very short patrol around each dummy's guard position when it is idle.
- Keep patrol constrained to a tiny axis-aligned path near home.
- Preserve existing chase and return behavior when the player is seen or lost.
- Add no pathfinding, no animation set, no sound, no dynamic allocation.

Expected visual result:

- Screen title shows `MEGALDOOM REWRITE GATE 53`.
- Subtitle shows `DUMMY PATROL`.
- An idle dummy should drift back and forth near its guard position.
- Once the dummy sees the player, it should leave patrol and chase as before.
- After losing the player, it should return home and resume the short patrol.
- Existing player push, i-frame, damage flash, death reset, shooting, pickups, keys, exit gating and phase loop still work.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: pending.

Human validation:

- Run in BlastEm and ares.
- Confirm the screen shows `MEGALDOOM REWRITE GATE 53`.
- Confirm the subtitle shows `DUMMY PATROL`.
- Watch an idle dummy and confirm it patrols in a short local path.
- Get spotted and confirm it stops patrolling and chases.
- Break line of sight, let it return, and confirm patrol resumes.

## Gate 54: dummy reacquire delay

Implementation status: ready for human validation.

Scope:

- Keep Gate 53 dummy guard patrol, player hit knockback, dummy return to guard, player invulnerability window, multiple dummies, dummy line of sight, hit pushback, player damage flash, dummy chase/contact attack, phase loop, clear-gated exit, locked-door unlock/consumption, billboard pickups, hitscan shot, shot cooldown HUD, visible weapon flash, use feedback HUD, per-type counters, collectible rules, depth sorting and wall occlusion.
- Add a short spotting delay when a dummy newly reacquires line of sight to the player.
- During that brief alert window, the dummy may chase but should not land contact damage yet.
- Reset the delay when line of sight is fully lost.
- Add no alert animation, no sound, no pathfinding and no dynamic allocation.

Expected visual result:

- Screen title shows `MEGALDOOM REWRITE GATE 54`.
- Subtitle shows `DUMMY SPOT DELAY`.
- A dummy that freshly sees the player should not damage instantly on the same reacquire moment.
- After the short delay, normal contact damage resumes.
- Breaking line of sight and reappearing should trigger the delay again.
- Existing patrol, return, player push, i-frame, damage flash, death reset, shooting, pickups, keys, exit gating and phase loop still work.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: pending.

Human validation:

- Run in BlastEm and ares.
- Confirm the screen shows `MEGALDOOM REWRITE GATE 54`.
- Confirm the subtitle shows `DUMMY SPOT DELAY`.
- Peek into dummy line of sight at close range and confirm it does not hit instantly.
- Stay exposed a moment longer and confirm it can then hit normally.
- Break line of sight and reacquire again; confirm the delay repeats.

## Gate 55: dummy separation

Implementation status: ready for human validation.

Scope:

- Keep Gate 54 dummy reacquire delay, dummy guard patrol, player hit knockback, dummy return to guard, player invulnerability window, multiple dummies, dummy line of sight, hit pushback, player damage flash, dummy chase/contact attack, phase loop, clear-gated exit, locked-door unlock/consumption, billboard pickups, hitscan shot, shot cooldown HUD, visible weapon flash, use feedback HUD, per-type counters, collectible rules, depth sorting and wall occlusion.
- Add a simple pairwise separation step so active dummies do not stack on top of each other as easily.
- Keep the resolution small and local; respect wall collision.
- Add no pathfinding, no flocking system, no animation set, no sound and no dynamic allocation.

Expected visual result:

- Screen title shows `MEGALDOOM REWRITE GATE 55`.
- Subtitle shows `DUMMY SEPARATION`.
- When two dummies converge on the player, they should avoid occupying the exact same spot and should spread a little.
- The separation should not push them through walls or closed doors.
- Existing reacquire delay, patrol, return, player push, i-frame, damage flash, death reset, shooting, pickups, keys, exit gating and phase loop still work.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: pending.

Human validation:

- Run in BlastEm and ares.
- Confirm the screen shows `MEGALDOOM REWRITE GATE 55`.
- Confirm the subtitle shows `DUMMY SEPARATION`.
- Lure two dummies together and confirm they do not fully stack.
- Confirm the separation still respects nearby walls and doors.

## Gate 56: dummy last seen pursuit

Implementation status: ready for human validation.

Scope:

- Keep Gate 55 dummy separation, dummy reacquire delay, dummy guard patrol, player hit knockback, dummy return to guard, player invulnerability window, multiple dummies, dummy line of sight, hit pushback, player damage flash, dummy chase/contact attack, phase loop, clear-gated exit, locked-door unlock/consumption, billboard pickups, hitscan shot, shot cooldown HUD, visible weapon flash, use feedback HUD, per-type counters, collectible rules, depth sorting and wall occlusion.
- Store a simple last-seen player position for each dummy while it has line of sight.
- After losing sight, make the dummy first walk toward that last-seen point before giving up and returning home.
- Keep the behavior simple and grid-like; add no pathfinding around complex corners, no sound and no dynamic allocation.

Expected visual result:

- Screen title shows `MEGALDOOM REWRITE GATE 56`.
- Subtitle shows `DUMMY LAST SEEN`.
- If the player breaks line of sight after being seen, the dummy should continue toward the last place it saw the player.
- If the player is no longer found there, the dummy should then return home and eventually resume patrol.
- Existing separation, reacquire delay, patrol, return, player push, i-frame, damage flash, death reset, shooting, pickups, keys, exit gating and phase loop still work.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: pending.

Human validation:

- Run in BlastEm and ares.
- Confirm the screen shows `MEGALDOOM REWRITE GATE 56`.
- Confirm the subtitle shows `DUMMY LAST SEEN`.
- Let a dummy see the player, then break line of sight around a corner.
- Confirm it walks to the last seen spot first.
- Confirm that after failing to find the player there, it returns home and resumes patrol.

## Gate 57: dummy post-hit recovery

Implementation status: ready for human validation.

Scope:

- Keep Gate 56 dummy last seen pursuit, dummy separation, dummy reacquire delay, dummy guard patrol, player hit knockback, dummy return to guard, player invulnerability window, multiple dummies, dummy line of sight, hit pushback, player damage flash, dummy chase/contact attack, phase loop, clear-gated exit, locked-door unlock/consumption, billboard pickups, hitscan shot, shot cooldown HUD, visible weapon flash, use feedback HUD, per-type counters, collectible rules, depth sorting and wall occlusion.
- After a dummy lands a valid hit, add a short recovery pause before it resumes movement pressure.
- Keep the existing attack cooldown and player i-frame; this recovery is additional dummy-side pacing.
- Add no new animation set, no sound, no pathfinding and no dynamic allocation.

Expected visual result:

- Screen title shows `MEGALDOOM REWRITE GATE 57`.
- Subtitle shows `DUMMY RECOVERY`.
- After hitting the player, a dummy should hesitate briefly instead of immediately continuing full movement pressure.
- In groups, this should make dummy pressure feel slightly more staggered and readable.
- Existing last-seen pursuit, separation, reacquire delay, patrol, return, player push, i-frame, damage flash, death reset, shooting, pickups, keys, exit gating and phase loop still work.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: pending.

Human validation:

- Run in BlastEm and ares.
- Confirm the screen shows `MEGALDOOM REWRITE GATE 57`.
- Confirm the subtitle shows `DUMMY RECOVERY`.
- Let a dummy hit the player and confirm it pauses briefly before resuming pressure.
- Confirm repeated attacks still work after the recovery and existing cooldown windows.
- Confirm the behavior still works when two dummies are active.

## Gate 58: dummy leash range

Implementation status: ready for human validation.

Scope:

- Keep Gate 57 dummy post-hit recovery, dummy last seen pursuit, dummy separation, dummy reacquire delay, dummy guard patrol, player hit knockback, dummy return to guard, player invulnerability window, multiple dummies, dummy line of sight, hit pushback, player damage flash, dummy chase/contact attack, phase loop, clear-gated exit, locked-door unlock/consumption, billboard pickups, hitscan shot, shot cooldown HUD, visible weapon flash, use feedback HUD, per-type counters, collectible rules, depth sorting and wall occlusion.
- Add a simple leash limit from each dummy's guard position.
- If a dummy is dragged too far from home, it should break off and head back instead of chasing forever.
- Keep the leash grid-simple; add no pathfinding, no sound and no dynamic allocation.

Expected visual result:

- Screen title shows `MEGALDOOM REWRITE GATE 58`.
- Subtitle shows `DUMMY LEASH`.
- A dummy can still chase the player normally near its home area.
- If pulled too far away, it should stop extending the chase and bias back toward home.
- Existing recovery, last-seen pursuit, separation, patrol, return, player push, i-frame, damage flash, death reset, shooting, pickups, keys, exit gating and phase loop still work.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: pending.

Human validation:

- Run in BlastEm and ares.
- Confirm the screen shows `MEGALDOOM REWRITE GATE 58`.
- Confirm the subtitle shows `DUMMY LEASH`.
- Lure a dummy away from its home area.
- Confirm that after enough distance it stops extending the chase and heads back.
- Confirm normal close-range chase still works near its guard region.

## Gate 59: dummy wake range

Implementation status: ready for human validation.

Scope:

- Keep Gate 58 dummy leash range, dummy post-hit recovery, dummy last seen pursuit, dummy separation, dummy reacquire delay, dummy guard patrol, player hit knockback, dummy return to guard, player invulnerability window, multiple dummies, dummy line of sight, hit pushback, player damage flash, dummy chase/contact attack, phase loop, clear-gated exit, locked-door unlock/consumption, billboard pickups, hitscan shot, shot cooldown HUD, visible weapon flash, use feedback HUD, per-type counters, collectible rules, depth sorting and wall occlusion.
- Add a simple wake radius for each dummy.
- Outside that radius, the dummy should stay in local idle behavior instead of trying to acquire the player.
- Once the player enters the wake range, normal LOS, chase and combat logic can start.
- Add no sound, no dynamic allocation and no new pathfinding.

Expected visual result:

- Screen title shows `MEGALDOOM REWRITE GATE 59`.
- Subtitle shows `DUMMY WAKE`.
- From far enough away, a dummy should remain in local patrol/guard behavior and not aggro yet.
- After entering the wake radius, the dummy should begin normal perception, chase and attack behavior.
- Existing leash, recovery, last-seen pursuit, separation, patrol, return, player push, i-frame, damage flash, death reset, shooting, pickups, keys, exit gating and phase loop still work.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: pending.

Human validation:

- Run in BlastEm and ares.
- Confirm the screen shows `MEGALDOOM REWRITE GATE 59`.
- Confirm the subtitle shows `DUMMY WAKE`.
- Observe a dummy from outside the wake range and confirm it does not aggro yet.
- Move closer until inside the wake range and confirm normal chase begins.

## Gate 60: low HP warning

Implementation status: ready for human validation.

Scope:

- Keep Gate 59 dummy wake range, dummy leash range, dummy post-hit recovery, dummy last seen pursuit, dummy separation, dummy reacquire delay, dummy guard patrol, player hit knockback, dummy return to guard, player invulnerability window, multiple dummies, dummy line of sight, hit pushback, player damage flash, dummy chase/contact attack, phase loop, clear-gated exit, locked-door unlock/consumption, billboard pickups, hitscan shot, shot cooldown HUD, visible weapon flash, use feedback HUD, per-type counters, collectible rules, depth sorting and wall occlusion.
- Add a subtle low-health warning overlay when the player is at critical HP.
- Keep it renderer-only; add no healing system, no audio warning and no gameplay rule change.

Expected visual result:

- Screen title shows `MEGALDOOM REWRITE GATE 60`.
- Subtitle shows `LOW HP WARNING`.
- At `HP 03` or `HP 02`, the view behaves as before.
- At `HP 01`, the 3D view shows a visible warning accent even when the damage flash is not currently active.
- The existing hit flash should still override the low-health warning momentarily when damage lands.
- Combat, reset, pickups, keys, exit gating and phase loop still work as before.
- No random noise, full-screen corruption or periodic one-second breakage.

Build validation:

- `.\tools\build-windows.ps1`
- Result: pending.

Human validation:

- Run in BlastEm and ares.
- Confirm the screen shows `MEGALDOOM REWRITE GATE 60`.
- Confirm the subtitle shows `LOW HP WARNING`.
- Drop to `HP 01` and confirm the warning overlay appears.
- Take another hit attempt and confirm the normal damage flash still overrides that frame.
