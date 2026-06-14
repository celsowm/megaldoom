# Changelog

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
