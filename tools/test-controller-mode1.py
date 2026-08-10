#!/usr/bin/env python3
"""Source contracts for the Doom 32X Mode 1 controller mapping."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/player_controller.c").read_text()
HEADER = (ROOT / "src/player_controller.h").read_text()
MAIN = (ROOT / "src/main.c").read_text()


def main():
    assert "PLAYER_CONTROL_PREVIOUS_WEAPON 0x0008" in HEADER
    assert "PLAYER_CONTROL_NEXT_WEAPON 0x0010" in HEADER
    assert "PLAYER_CONTROL_TOGGLE_AUTOMAP 0x0020" in HEADER
    assert "PLAYER_CONTROL_FIRE_HELD 0x0040" in HEADER
    assert "JOY_getJoypadType(JOY_1) == JOY_TYPE_PAD6" in SOURCE

    # A selects Doom's 50-unit run command (2x walk), not the old strafe-left
    # binding. C only strafes with a lateral direction and blocks turning.
    assert "#define DOOM_FORWARD_WALK 25" in SOURCE
    assert "#define DOOM_FORWARD_RUN 50" in SOURCE
    assert "#define DOOM_STRAFE_WALK 24" in SOURCE
    assert "#define DOOM_STRAFE_RUN 40" in SOURCE
    assert "const bool running = ((joy & BUTTON_A) != 0) && !three_button_map_chord;" in SOURCE
    assert "const bool strafing = ((joy & BUTTON_C) != 0)" in SOURCE
    assert "const bool turning_left = !strafing" in SOURCE
    assert "const bool turning_right = !strafing" in SOURCE
    assert "target_strafe -= strafe_command;" in SOURCE
    assert "target_strafe += strafe_command;" in SOURCE
    assert ("((joy & BUTTON_C) != 0) && ((s_previous_joy & BUTTON_C) == 0) &&\n"
            "             !strafing && !weapon_chord") in SOURCE

    # Weapon selection: C+UP / C+DOWN on either pad type, edge-triggered on the
    # direction so holding it does not run through the whole arsenal. Like
    # strafe, the chord suppresses both USE and forward/back movement.
    assert ("const bool weapon_chord = ((joy & BUTTON_C) != 0) &&\n"
            "                              ((joy & (BUTTON_UP | BUTTON_DOWN)) != 0);") in SOURCE
    assert "!three_button_map_chord && !weapon_chord && ((joy & BUTTON_UP) != 0)" in SOURCE
    assert "!three_button_map_chord && !weapon_chord && ((joy & BUTTON_DOWN) != 0)" in SOURCE
    assert "if (direction != s_weapon_chord_dir) {" in SOURCE
    assert "s_weapon_chord_dir = 0;" in SOURCE
    # Only the automatic weapons act on the held bit; B's rising edge still
    # drives everything else.
    assert "result |= PLAYER_CONTROL_FIRE_HELD;" in SOURCE
    assert "weapon->automatic ? (PLAYER_CONTROL_FIRE | PLAYER_CONTROL_FIRE_HELD)" in MAIN

    # The 3-button A+B+C chord is edge-latched and suppresses its ordinary
    # A/B/C behaviours. X/Y/Z only emit on a detected 6-button pad.
    assert "THREE_BUTTON_AUTOMAP_CHORD (BUTTON_A | BUTTON_B | BUTTON_C)" in SOURCE
    assert "!six_button_pad && ((joy & THREE_BUTTON_AUTOMAP_CHORD) == THREE_BUTTON_AUTOMAP_CHORD)" in SOURCE
    assert "s_three_button_map_chord_active" in SOURCE
    assert "result |= PLAYER_CONTROL_TOGGLE_AUTOMAP;" in SOURCE
    assert "result |= PLAYER_CONTROL_PREVIOUS_WEAPON;" in SOURCE
    assert "result |= PLAYER_CONTROL_NEXT_WEAPON;" in SOURCE
    assert "(joy & BUTTON_Z)" in SOURCE
    assert "(joy & BUTTON_MODE)" not in SOURCE

    # The automap is the one action still without gameplay attached.
    assert "PLAYER_CONTROL_TOGGLE_AUTOMAP is still reserved" in MAIN

    print("ok    controller: Doom 32X Mode 1, 3-button chord, six-button actions")


if __name__ == "__main__":
    main()
