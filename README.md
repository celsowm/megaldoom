# MegalDoom

MegalDoom is a Doom-inspired first-person shooter built for the Sega Mega Drive / Genesis.
It is a technical experiment in bringing textured 3D-style environments, doors, enemies,
items, music and sound effects to the console's hardware limits.

## Play the ROM

The playable ROM will be available on itch.io:

**[Download MegalDoom](https://celsowm.itch.io/megaldoom)**

The current build is a playable prototype focused on E1M1, with exploration, combat,
pickups, doors, switches and the classic Doom front-end experience.

## Features

- Textured first-person rendering designed for the Mega Drive VDP.
- Doom-inspired E1M1 map conversion with walls, doors and switches.
- Player movement, strafing, collision and run mode.
- Enemies, blocking decorations and depth-buffered billboards.
- Health, armor, keys and pistol-ammo pickups.
- Pistol with recoil and basic combat feedback.
- HUD, playable Doom-style automap and pause menu.
- Title screen, animated menu, skill selection and audio options.
- XGM2 music and Doom sound effects.
- Blue, yellow and red key doors with persistent key state.

## Controls

### Menus

- D-Pad Up/Down: move the menu cursor
- Start, A or C: confirm
- B: back
- Start during gameplay: pause or resume

### Gameplay

- D-Pad Up/Down: move forward/backward
- D-Pad Left/Right: turn
- A (hold): run
- B: fire
- C: open doors and activate switches
- C + D-Pad Left/Right: strafe

### Automap (6-button controller)

- Z: open/close the automap
- D-Pad: move/turn while follow is on; pan while follow is off
- X: toggle follow
- Y: show the full map / restore the previous view
- C: toggle the 128-unit grid
- A/B: zoom in/out (A + D-Pad also runs while following)
- Start: pause

### Automap (3-button controller)

- C + Start: open/close the automap
- D-Pad: move/turn while follow is on; pan while follow is off
- B: toggle follow
- C: toggle the grid
- A: alternate zoom in/out
- A + Start: show the full map / restore the previous view
- Start alone: pause

## Build from source

The project uses [SGDK](https://github.com/Stephane-D/SGDK) and can be built on
Windows or Linux. The setup scripts download or detect the required local tools;
build output and emulator folders are kept out of version control.

### Windows

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
.\tools\setup-sgdk-windows.ps1
.\tools\download-emulator-windows.ps1
.\tools\run-windows.ps1
```

With Node.js installed, the common commands are also available through npm:

```powershell
npm run build   # build the ROM
npm run test    # build and run project checks
npm run play    # build and launch in an emulator
```

If SGDK is already installed, set `GDK` before building:

```powershell
$env:GDK="C:\sgdk"
.\tools\build-windows.ps1
```

### Linux

```bash
chmod +x tools/*.sh
./tools/setup-sgdk-linux.sh
./tools/download-emulator-linux.sh
./tools/run-linux.sh
```

If SGDK is already installed:

```bash
export GDK=/opt/sgdk
./tools/build-linux.sh
```

## Project status

MegalDoom is an evolving prototype. The current focus is improving the playable
E1M1 experience and expanding gameplay while keeping the renderer within the
Mega Drive's memory and performance limits.

## License

The source code is released under the [MIT License](LICENSE).

MegalDoom is an unofficial fan project and is not affiliated with or endorsed by
id Software or Bethesda Softworks.
