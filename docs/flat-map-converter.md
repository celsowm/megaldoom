# Single-level playable WAD conversion

The Mega Drive target no longer attempts to reproduce Doom sector heights at runtime. The supported conversion profile is a textured, single-level BSP map inspired by *Duke Nukem 3D* on Mega Drive and *Zero Tolerance*:

- original wall and door textures stay recognizable;
- floors, stairs, ledges and lifts are projected onto one gameplay plane;
- two-sided height transitions become open 2D portals;
- one-sided and explicitly impassable lines remain walls;
- manual doors remain interactive;
- blue, yellow and red locks remain distinct;
- keys are reusable, as in Doom;
- conversion fails unless a route from Player 1 start to an exit can be proven.

## Commands

```powershell
python tools/test-flat-map-converter.py
.\tools\convert-flat-map.ps1 -WadPath DOOM1.WAD -Map E1M1
.\tools\build-windows.ps1
```

Do not pass `-SectorRenderer`. The default BSP renderer is the textured, uniform-height path.

The wrapper first writes `out/e1m1-flat-plan.json`. Only after the semantic preflight succeeds does it invoke `wad-map-extract.py` to regenerate the C map and texture catalog.

## Progression proof

The preflight constructs a sector graph after flattening. A search state is `(sector, key mask)`, where the mask records blue, yellow and red keys already collected. Entering a sector collects its keys. A locked-door edge is traversable only when the matching bit is present. Keys are never removed, so one key can open every matching door.

The output includes the validated route, door requirements, key locations, exit sectors, flattened lines and diagnostics. A missing or wrong-colour key causes the command to fail when it blocks every route to the exit.

## Deliberate simplification

Non-key remote geometry specials that require lifts, moving floors or multi-height sector state are normalized to always-open portals. They are reported as `REMOTE_SPECIAL_NORMALIZED`. Exit switches remain exits. This policy prioritizes guaranteed completion and recognizable layout over reproducing unsupported vertical machinery.
