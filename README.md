# MegalDoom 0.0.4

MegalDoom is a tiny SGDK prototype for a Mega Drive / Genesis Doom-like, originally inspired by the DoomGeo approach, but rewritten around Mega Drive constraints.

MegalDoom renders through a single flat-BSP pipeline tailored to the Mega Drive
VDP: the WAD converter emits textured wall segments and grouped doors, the BSP
caster fills the view columns, and the tile packer writes them directly to the
background plane. There is no software-BMP, DDA grid, sector-height, or
reference-renderer fallback in the product.

## Current state

- SGDK project layout.
- Converted single-level Doom maps, starting with E1M1.
- 1024-step Q8 sine table, no runtime float math.
- Player movement, strafing and wall collision.
- One textured flat-BSP cast per frame, packed directly into VDP tiles.
- Real Doom wall, door and switch textures extracted from the WAD.
- Grouped doors with persistent blue, yellow and red key bits.
- Action button opens/closes a complete Doom door group.
- Flat conversion opens former height-only transitions while keeping structural walls.
- Converter certification rejects maps without a proven start-to-key-to-exit route.
- Generated E1M1 pickups, enemies and blocking decorative billboards (candles,
  lamp/column props and barrels) with depth-buffer occlusion.
- Functional health, armor, keys and pistol-ammo pickups.
- Placeholder pistol with recoil.
- HUD and minimap.
- XGM2 background music (E1M1) and Doom PCM sound effects: pistol gunshot, enemy pain/death, player pain/death, door move and item pickup.

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

- General support for every Doom special (unsupported required mechanics fail conversion explicitly).
- Real enemy AI.
- Hardware sprite weapon/enemies.

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

To test in the alternate emulator:

```powershell
.\tools\run-windows.ps1 -Emulator ares
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
.\tools\download-emulator-windows.ps1 [-Emulator All|BlastEm|ares] [-Stable] [-Force]
.\tools\build-windows.ps1 [-NoClean]
.\tools\test-windows.ps1 [-NoBuild] [-NoClean]
.\tools\check-rom.ps1
.\tools\run-windows.ps1 [-Emulator Auto|BlastEm|ares] [-NoBuild] [-NoClean] [-RomPath out\rom.bin]
.\tools\clean-windows.ps1
```

If you have Node installed, `package.json` wraps the common ones so they feel like
any Node project: `npm run build`, `npm run test`, `npm run check`, `npm run play`,
`npm run assets`.

### Guardrails (`npm run test` / `tools\test-windows.ps1`)

`test-windows.ps1` builds the ROM and then runs `check-rom.ps1`, which fails the
build on problems that otherwise only surface at runtime in the emulator. The main
one is the **work-RAM budget**: the Mega Drive has 64 KB of work RAM shared by
static data (`.data` + `.bss`), the stack and SGDK's heap. Oversized static data
starves the heap and SGDK panics `not enough memory to reset VDP` at boot. The
check reads section sizes from `size.exe` and errors when too little RAM is left
free. Prefer `const` data (it lives in cartridge ROM, not work RAM) and avoid large
mutable globals. Run it before committing or launching BlastEm.

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
.tools/ares           downloaded ares
out/                  SGDK build output
```

These folders are ignored by git.

## Design notes

### Runtime complexity

- BSP rendering is worst-case `O(nodes + segments + sampled columns)`, but normal
  frames visit only view-relevant nodes and segments. Near/far node decisions are
  computed lazily once per reached node at each player position and reused while
  rotating in place.
- Player/enemy circle collision uses the generated 256-unit blockmap and is
  `O(k)` in local candidate segments instead of scanning every map segment.
- Enemy line-of-sight walks crossed blockmap cells and is `O(c + k)` for crossed
  cells plus unique segment candidates.
- Enemy separation remains `O(E^2)`; `E` is currently capped at seven objects.

Build with `tools/build-windows.ps1 -DebugPerf` to show cast, packing, billboard,
overlay, DMA, collision, LOS, blockmap-candidate and lazy BSP-side costs in the
on-screen diagnostic rows.

This is deliberately not a direct DoomGeo renderer port. DoomGeo leans on Neo Geo sprite-strip strengths. The Mega Drive has much tighter sprite-per-line and VRAM constraints, so the first milestones use software bitmap mode. The correct long-term route is to replace this renderer with a tile/column renderer using pre-scaled wall slices.

Suggested next milestones:

1. Replace per-pixel `BMP_setPixelFast` drawing with tile slice bands.
2. Add precomputed wall-height / texture-step tables.
3. Add animated doors.
4. Add multiple actors with visibility list and depth sort.
5. Move weapon and enemies to sprites or tile assets.
6. Add offline asset converter for maps/textures.
