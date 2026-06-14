# MegalDoom 0.0.4

MegalDoom is a tiny SGDK prototype for a Mega Drive / Genesis Doom-like, originally inspired by the DoomGeo approach, but rewritten around Mega Drive constraints.

This version still uses SGDK's software `BMP` engine so the early milestones can focus on map, camera, collision, raycasting and gameplay state before moving to a proper VDP/tile renderer. SGDK's bitmap mode is useful for prototypes, but it is not the final renderer target.

## Current state

- SGDK project layout.
- Hardcoded mutable 16x16 map.
- 1024-step Q8 sine table, no runtime float math.
- Player movement, strafing and wall collision.
- DDA raycaster with 64 columns, 4 pixels each.
- Per-column fish-eye correction.
- Precomputed DDA reciprocal tables for runtime cast speed.
- Fake 64x64 procedural wall patterns.
- Different wall types: stone, door and tech wall.
- Action button for doors.
- Placeholder billboard enemy with depth-buffer occlusion.
- Placeholder pistol with recoil.
- HUD and minimap.

## New in 0.0.4

- Project renamed from `MegaDoomGeo` to `MegalDoom`.
- Include guards and version string updated.
- Added `.gitignore` for SGDK outputs, local toolchains and downloaded emulators.
- Added Windows and Linux setup scripts for SGDK.
- Added Windows and Linux scripts to download BlastEm.
- Added Windows and Linux build scripts that auto-detect local `.toolchain/sgdk`.
- Added Windows and Linux run scripts that build and launch the ROM in BlastEm.
- Added clean scripts.
- Added `.env.example` for local path overrides.

## What is not included yet

- WAD import.
- Real Doom textures.
- Animated doors.
- Real enemy AI.
- Hardware sprite weapon/enemies.
- Tilemap/VDP optimized renderer.
- Sound.

## Controls

- D-Pad Up: move forward
- D-Pad Down: move backward
- D-Pad Left/Right: rotate
- A: strafe left
- C: strafe right
- B: action/fire; opens or closes a door if one is in front of the player

## Fast path: Windows PowerShell

From the project root:

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
.\tools\setup-sgdk-windows.ps1
.\tools\download-emulator-windows.ps1
.\tools\run-windows.ps1
```

Optional: build the SGDK library during setup:

```powershell
.\tools\setup-sgdk-windows.ps1 -BuildLibrary
```

If you already have SGDK installed:

```powershell
$env:GDK="C:\sgdk"
.\tools\build-windows.ps1
```

If you already have an emulator installed:

```powershell
$env:EMULATOR="C:\tools\blastem\blastem.exe"
.\tools\run-windows.ps1
```

## Fast path: Linux shell

From the project root:

```bash
chmod +x tools/*.sh
./tools/setup-sgdk-linux.sh
./tools/download-emulator-linux.sh
./tools/run-linux.sh
```

Important Linux note: SGDK release packages are convenient, but the m68k GCC toolchain is usually not bundled for Linux. The script downloads SGDK and checks what it can, but you may still need to install/build `m68k-elf-gcc`, Java, `make`, `curl`, `unzip`, and `python3` on your distro.

If you already have SGDK installed:

```bash
export GDK=/opt/sgdk
./tools/build-linux.sh
```

If you already have an emulator installed:

```bash
export EMULATOR=/opt/blastem/blastem
./tools/run-linux.sh
```

## Script reference

### Windows

```powershell
.\tools\setup-sgdk-windows.ps1 [-BuildLibrary] [-Force]
.\tools\download-emulator-windows.ps1 [-Stable] [-Force]
.\tools\build-windows.ps1 [-NoClean]
.\tools\run-windows.ps1 [-NoBuild] [-NoClean] [-RomPath out\rom.bin]
.\tools\clean-windows.ps1
```

### Linux

```bash
./tools/setup-sgdk-linux.sh [install-dir]
BUILD_LIBRARY=1 ./tools/setup-sgdk-linux.sh
FORCE=1 ./tools/setup-sgdk-linux.sh
./tools/download-emulator-linux.sh [install-dir]
STABLE=1 ./tools/download-emulator-linux.sh
FORCE=1 ./tools/download-emulator-linux.sh
./tools/build-linux.sh
NO_CLEAN=1 ./tools/build-linux.sh
./tools/run-linux.sh
NO_BUILD=1 ./tools/run-linux.sh
./tools/clean-linux.sh
```

## Local folders created by scripts

```text
.toolchain/sgdk       downloaded SGDK
.tools/blastem        downloaded BlastEm
out/                  SGDK build output
```

These folders are ignored by git.

## Design notes

This is deliberately not a direct DoomGeo renderer port. DoomGeo leans on Neo Geo sprite-strip strengths. The Mega Drive has much tighter sprite-per-line and VRAM constraints, so the first milestones use software bitmap mode. The correct long-term route is to replace this renderer with a tile/column renderer using pre-scaled wall slices.

Suggested next milestones:

1. Replace per-pixel `BMP_setPixelFast` drawing with tile slice bands.
2. Add precomputed wall-height / texture-step tables.
3. Add animated doors.
4. Add multiple actors with visibility list and depth sort.
5. Move weapon and enemies to sprites or tile assets.
6. Add offline asset converter for maps/textures.
