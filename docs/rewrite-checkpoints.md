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

Implementation status: ready for human validation.

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
