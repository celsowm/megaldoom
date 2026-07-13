# Single-level playable WAD conversion

The Mega Drive target no longer attempts to reproduce Doom sector heights at runtime. The supported conversion profile is a textured, single-level BSP map inspired by *Duke Nukem 3D* on Mega Drive and *Zero Tolerance*:

- original wall and door textures stay recognizable;
- traversable stairs, ledges and height changes are projected onto one gameplay plane;
- one-sided and explicitly impassable lines remain walls;
- fully closed lift/moving-floor geometry remains a wall when the legacy renderer cannot represent it;
- manual doors remain interactive;
- blue, yellow and red locks remain distinct;
- keys are reusable, as in Doom;
- conversion fails unless a route from Player 1 start to an exit can be proven against the geometry actually emitted.

## Commands

```powershell
npm run test:flat-map
npm run assets:flat-map -- -WadPath DOOM1.WAD -Map E1M1
npm run build
```

Do not pass `-SectorRenderer`. The default BSP renderer is the textured, uniform-height path.

The wrapper first writes `out/e1m1-flat-plan.json`. Only after the semantic preflight succeeds does it invoke `wad-map-extract.py` to regenerate the C map and texture catalog.

## Progression proof

The preflight constructs a sector graph after flattening. A search state is `(sector, key mask)`, where the mask records blue, yellow and red keys already collected. Entering a sector collects its keys. A locked-door edge is traversable only when the matching bit is present. Keys are never removed, so one key can open every matching door.

The preflight applies the same solid-line rule as `wad-map-extract.py` before searching. Unsupported fully closed non-door portals are reported as `UNSUPPORTED_CLOSED_PORTAL` and are treated as walls. Therefore a route accepted by the preflight uses the same collision topology that is compiled into the ROM.

The output includes the validated route, door requirements, key locations, exit sectors, flattened lines and diagnostics. A missing or wrong-colour key, or indispensable unsupported vertical machinery, causes the command to fail when it blocks every route to the exit.

## Runtime key contract

`src/keyed_runtime.c` records the colour of collected key sprites, maps Doom locked-door specials to blue/yellow/red, and delegates actual door state changes to the existing BSP implementation. The keys are not consumed. `Makefile` redirects only the three calls made by `main.c`, avoiding duplicated door or billboard state.

## Deliberate simplification

Passable source portals lose their height difference. Fully closed remote geometry that needs lifts, moving floors or multi-height sector state is not invented as an open passage: it remains a wall and can invalidate the conversion. Exit switches remain exits. This prioritizes a truthful completion guarantee and recognizable layout over pretending unsupported vertical machinery works.
