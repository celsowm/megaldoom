#!/usr/bin/env python3
"""Source contracts for gameplay plus the Doom 32X automap controls."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/player_controller.c").read_text()
HEADER = (ROOT / "src/player_controller.h").read_text()
MAIN = (ROOT / "src/main.c").read_text()
AUTOMAP = (ROOT / "src/automap.c").read_text()


def main():
    for token in (
        "PLAYER_CONTROL_MODE_GAMEPLAY",
        "PLAYER_CONTROL_MODE_AUTOMAP_FOLLOW",
        "PLAYER_CONTROL_MODE_AUTOMAP_PAN",
        "PLAYER_CONTROL_MODE_SUPPRESSED",
    ):
        assert token in HEADER
    assert "const bool gameplay" in SOURCE
    assert "const bool map_follow" in SOURCE
    assert "gameplay || (map_follow && six_button_pad)" in SOURCE
    assert "if (gameplay && strafing)" in SOURCE
    assert "if (gameplay && six_button_pad)" in SOURCE
    assert "result |= PLAYER_CONTROL_FIRE_HELD;" in SOURCE
    assert "weapon->automatic ? (PLAYER_CONTROL_FIRE | PLAYER_CONTROL_FIRE_HELD)" in MAIN

    assert "BUTTON_C | BUTTON_START" in AUTOMAP
    assert "BUTTON_A | BUTTON_START" in AUTOMAP
    assert "pressed & BUTTON_Z" in AUTOMAP
    assert "pressed & BUTTON_X" in AUTOMAP
    assert "pressed & BUTTON_Y" in AUTOMAP
    assert "AUTOMAP_INPUT_CONSUME_START" in MAIN
    assert "PLAYER_CONTROL_MODE_SUPPRESSED" in MAIN
    assert "PLAYER_CONTROL_TOGGLE_AUTOMAP is still reserved" not in MAIN
    assert "THREE_BUTTON_AUTOMAP_CHORD" not in SOURCE

    print("ok    controller: gameplay and Doom 32X automap contexts")


if __name__ == "__main__":
    main()
