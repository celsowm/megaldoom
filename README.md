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
- Grouped vertical-lift doors with persistent blue, yellow and red key bits.
- Action button smoothly opens/closes a complete Doom door group in 32 vblanks.
- Flat conversion opens former height-only transitions while keeping structural walls.
- Converter certification rejects maps without a proven start-to-key-to-exit route.
- Generated E1M1 pickups, enemies and blocking decorative billboards (candles,
  lamp/column props and barrels) with depth-buffer occlusion.
- Functional health, armor, keys and pistol-ammo pickups.
- Placeholder pistol with recoil.
- HUD and minimap.
- XGM2 background music (E1M1) and Doom PCM sound effects: pistol gunshot, enemy pain/death, player pain/death, door move and item pickup.
- Three-card fan-port/SGDK/social boot sequence with intro music, a bobbing Cacodemon splash, Doom `TITLEPIC`, animated-skull main menu, audio options and an in-game pause menu.

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

### Menus

- Start on the title screen: open the main menu
- D-Pad Up/Down: move the animated skull cursor
- Start, A or C: confirm
- B: back
- Start during gameplay: pause / resume through the pause menu
- Options: toggle music and sound effects for the current boot session

`NEW GAME` opens the five classic Doom skill choices. Their source-faithful E1M1
THING populations map to easy (the first two choices), normal (`HURT ME PLENTY`)
and hard (the last two choices); Nightmare-specific respawn and speed rules are
not part of this prototype. Save/load and episode selection are intentionally not
exposed yet.

### Gameplay

- D-Pad Up: move forward
- D-Pad Down: move backward
- D-Pad Left/Right: rotate
- A (hold): run at 1.5x movement speed
- B: fire
- C: open doors / activate switches
- C + D-Pad Left/Right: strafe left/right
- 3-button pad, A + B + C: automap toggle (reserved until the automap exists)
- 6-button pad, X/Y: previous/next weapon (reserved until weapon selection exists)
- 6-button pad, Z: automap toggle (reserved until the automap exists)
- 6-button pad, Mode: unused

## Fast path: Windows PowerShell

From the project root:

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
.\tools\setup-sgdk-windows.ps1
.\tools\download-emulator-windows.ps1
.\tools\run-windows.ps1
```

For the usual edit/build/debug loop, one command rebuilds with the performance
overlay, closes the previous BlastEm instance and starts the new ROM without
occupying the terminal:

```powershell
npm run debug
```

The project also keeps an ignored BlastEm development checkout in
`.externals/blastem`. Its pinned Libretro core can be rebuilt after emulator
changes with:

```powershell
npm run blastem:core
```

This build uses WSL `gcc`, `g++` and `make`, writes intermediates to the Linux
filesystem for speed, and copies the verified result to
`.externals/blastem/build/libblastem.so`. The downloaded Windows emulator in
`.tools/blastem` remains the normal interactive fallback until the custom
Windows frontend/toolchain is enabled.

To build that custom, isolated Windows frontend, run:

```powershell
npm run blastem:windows
```

It downloads a pinned LLVM MinGW-w64 toolchain plus SDL2 and GLEW to the
ignored `.externals/toolchain` directory and produces
`.externals/blastem/build/windows/blastem.exe`. It never changes a global
compiler installation.

The custom emulator accepts a deterministic route (`frame hexadecimalMask` per
line). For example, `0 0008` holds Right on emulated frame zero. Run a fixed,
headless route and write its cycle/PC report with:

```powershell
npm run blastem:route -- -Route tools\routes\second-room.txt -Frames 600
```

Use `-Mailbox FF0000:128` to include a DebugPerf RAM range in the JSON report;
`-CaptureDir out\route-frames -CaptureEvery 60` additionally writes periodic
PPM frames. This path uses no desktop focus or `SendKeys`.

The native sector renderer is the only renderer and needs no `-SectorRenderer`
switch.

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
.\tools\build-windows.ps1 [-Clean] [-DebugPerf] [-ForceAssets]
.\tools\test-windows.ps1 [-NoBuild] [-Clean] [-ForceAssets]
.\tools\check-rom.ps1
.\tools\run-windows.ps1 [-Emulator Auto|BlastEm|ares] [-NoBuild] [-Clean] [-DebugPerf] [-ForceAssets] [-Restart] [-Detach] [-RomPath out\rom.bin]
.\tools\clean-windows.ps1
```

If you have Node installed, `package.json` wraps the common ones so they feel like
any Node project: `npm run build`, `npm run test`, `npm run check`, `npm run play`,
`npm run debug`, `npm run assets`.

Builds are incremental by default. The frontend generator fingerprints the exact
copyrighted source PNGs it uses and leaves `res/frontend/` untouched on a cache
hit, allowing SGDK and `rescomp` to reuse their existing outputs. Use `-Clean`
to force a full compile, `-ForceAssets` to force only the frontend regeneration,
or `npm run assets` to rebuild every generated map/graphics header. The legacy
`-NoClean` switch and Linux `NO_CLEAN=1` remain accepted but are no longer necessary.

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
CLEAN=1 ./tools/build-linux.sh
DEBUG_PERF=1 ./tools/build-linux.sh
FORCE_ASSETS=1 ./tools/build-linux.sh
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
build/                incremental compiler-flag state
res/frontend/         generated Doom frontend PNG cache
```

These folders are ignored by git.

## Design notes

### Runtime complexity

- BSP rendering uses a successor set for solid sampled columns, so each ordinary
  wall sample becomes closed once per frame and farther overlapping segments jump
  over closed runs. Moving-door overlays deliberately revisit still-open samples.
  Near/far node decisions are computed lazily once per reached node at each player
  position and reused while rotating in place.
- Player/enemy wall collision uses the generated 256-unit blockmap and is
  `O(k)` in local candidate segments instead of scanning every map segment.
- Static billboard collision uses a compact blocker registry and is `O(B)` in
  active blocking props instead of scanning all active billboards.
- The shipped wall profile samples 80 stride-2 columns and uses 64x64 WAD
  textures. Shade-ready pixel pairs for ordinary walls and styled doors are
  packed offline into cartridge ROM, so the higher detail does not add runtime
  palette quantization or per-texel shading.
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
3. Add multiple actors with visibility list and depth sort.
4. Move weapon and enemies to sprites or tile assets.
5. Add offline asset converter for maps/textures.
