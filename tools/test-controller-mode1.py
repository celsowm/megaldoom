#!/usr/bin/env python3
"""Source contracts for the Doom 32X Mode 1 controller mapping."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/player_controller.c").read_text()
HEADER = (ROOT / "src/player_controller.h").read_text()
MAIN = (ROOT / "src/main.c").read_text()


def main():
    # Public future-facing actions remain available even though main deliberately
    # does not attach gameplay to them yet.
    assert "PLAYER_CONTROL_PREVIOUS_WEAPON 0x0008" in HEADER
    assert "PLAYER_CONTROL_NEXT_WEAPON 0x0010" in HEADER
    assert "PLAYER_CONTROL_TOGGLE_AUTOMAP 0x0020" in HEADER
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
    assert "((joy & BUTTON_C) != 0) && ((s_previous_joy & BUTTON_C) == 0) && !strafing" in SOURCE

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

    # Future actions have no current gameplay side effects.
    assert "intentionally reserved until the" in MAIN

    print("ok    controller: Doom 32X Mode 1, 3-button chord, six-button actions")


if __name__ == "__main__":
    main()
