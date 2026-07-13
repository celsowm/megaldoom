# Single-level playable WAD conversion

The Mega Drive target no longer attempts to reproduce Doom sector heights at runtime. The supported conversion profile is a textured, single-level BSP map inspired by *Duke Nukem 3D* on Mega Drive and *Zero Tolerance*:

- original wall and door textures stay recognizable;
- traversable stairs, ledges and height changes are projected onto one gameplay plane;
- one-sided and explicitly impassable lines remain walls;
- fully closed lift/moving-floor geometry remains a wall when the legacy renderer cannot represent it;
- manual doors remain interactive;
- blue, yellow and red locks remain distinct;
- keys are reusable, as in Doom;
- conversion fails unless both a semantic progression route and a concrete collision-clear completion route are proven against the geometry actually emitted.

## Commands

```powershell
npm run test:flat-map
npm run assets:flat-map -- -WadPath DOOM1.WAD -Map E1M1
npm run build
```

Do not pass `-SectorRenderer`. The default BSP renderer is the textured, uniform-height path.

The wrapper first writes `out/e1m1-flat-plan.json`. Only after every preflight succeeds does it invoke `wad-map-extract.py` to regenerate the C map and texture catalog.

## Two completion proofs

The first pass constructs a sector graph after flattening. A search state is `(sector, key mask)`, where the mask records blue, yellow and red keys already collected. Entering a sector collects its keys. A locked-door edge is traversable only when the matching bit is present. Keys are never removed, so one key can open every matching door.

The sector graph is useful for explaining progression, but it is not sufficient by itself because one Doom sector identifier can occur in disconnected pieces. `wad-flat-route.py` therefore performs a second, constructive search over actual world coordinates:

- 16-unit navigation grid anchored at Player 1 start;
- the runtime player radius of 32 units;
- the exact wall/door segments emitted by the uniform-height extractor;
- the medium-skill, single-player THING filter and 112-object runtime cap;
- coloured reusable keys and the corresponding locked-door specials;
- blocking barrels;
- every spawned target, because the current game locks the exit while targets remain;
- starting ammunition and reachable clip/ammo-box pickups;
- line of sight and an approach point for each target and the exit switch.

A successful proof produces an explicit sequence of collision-clear moves. The JSON stores it under `navigation_proof`, including pickup/target/exit events and compressed waypoints. This is a topology and resource proof; it does not attempt to guarantee player health or combat skill.

## Geometry fidelity

The preflight applies the same solid-line rule as `wad-map-extract.py` before searching. Unsupported fully closed non-door portals are reported as `UNSUPPORTED_CLOSED_PORTAL` and are treated as walls. Door or exit specials on source lines that produce no runtime segment are also removed from the proof and reported. Therefore an accepted route uses the collision topology that is compiled into the ROM.

Keys filtered out by runtime skill flags, single-player flags or the 112-object cap are reported as `KEY_NOT_SPAWNED_BY_RUNTIME` and cannot satisfy the proof. A missing or wrong-colour key, insufficient ammunition, an unreachable target, an indispensable unsupported lift, or an unreachable exit causes conversion to fail before generated C files are replaced.

## Runtime key contract

`src/keyed_runtime.c` records the colour of collected key sprites, maps Doom locked-door specials to blue/yellow/red, and delegates actual door state changes to the existing BSP implementation. The keys are not consumed. `Makefile` redirects only the three calls made by `main.c`, avoiding duplicated door or billboard state.

## Deliberate simplification

Passable source portals lose their height difference. Fully closed remote geometry that needs lifts, moving floors or multi-height sector state is not invented as an open passage: it remains a wall and can invalidate the conversion. Exit switches remain exits. This prioritizes a truthful completion guarantee and recognizable layout over pretending unsupported vertical machinery works.

The automated unit tests use synthetic maps. Run `npm run assets:flat-map -- -WadPath DOOM1.WAD -Map E1M1` locally with the original WAD to obtain the real E1M1 proof before accepting the generated map.
