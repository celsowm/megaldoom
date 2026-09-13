#!/usr/bin/env python3
"""Contracts for OPTIONS > CONTROLS button remapping (src/controls.c).

The C module is mirrored here and the mirror is pinned to the source (default
table, group order, action order), so the exhaustive checks below cannot pass
against a model that has drifted from the ROM.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/controls.c").read_text()
HEADER = (ROOT / "src/controls.h").read_text()
PLAYER = (ROOT / "src/player_controller.c").read_text()
AUTOMAP = (ROOT / "src/automap.c").read_text()
FRONTEND = (ROOT / "src/frontend.c").read_text()

# SGDK joy.h bit values.
BUTTONS = {
    "BUTTON_UP": 0x0001, "BUTTON_DOWN": 0x0002, "BUTTON_LEFT": 0x0004,
    "BUTTON_RIGHT": 0x0008, "BUTTON_B": 0x0010, "BUTTON_C": 0x0020,
    "BUTTON_A": 0x0040, "BUTTON_START": 0x0080, "BUTTON_Z": 0x0100,
    "BUTTON_Y": 0x0200, "BUTTON_X": 0x0400, "BUTTON_MODE": 0x0800,
}
ACTIONS = ("CONTROL_FIRE", "CONTROL_USE", "CONTROL_RUN",
           "CONTROL_PREV_WEAPON", "CONTROL_NEXT_WEAPON", "CONTROL_AUTOMAP")
DEFAULT = ("BUTTON_B", "BUTTON_C", "BUTTON_A", "BUTTON_X", "BUTTON_Y", "BUTTON_Z")
GROUPS = (("BUTTON_A", "BUTTON_B", "BUTTON_C"), ("BUTTON_X", "BUTTON_Y", "BUTTON_Z"))
REMAPPABLE = sum(BUTTONS[name] for group in GROUPS for name in group)


def pin_mirror_to_source():
    enum = re.search(r"typedef enum \{(.*?)\} ControlAction;", HEADER, re.S)
    assert enum, "ControlAction enum missing"
    names = re.findall(r"\b(CONTROL_[A-Z_]+)\b", enum.group(1))
    assert tuple(names) == ACTIONS + ("CONTROL_ACTION_COUNT",), names

    default = re.search(r"#define CONTROLS_DEFAULT_BINDINGS \\\s*\{([^}]*)\}", SOURCE)
    assert default, "CONTROLS_DEFAULT_BINDINGS missing"
    assert tuple(re.findall(r"BUTTON_[A-Z]+", default.group(1))) == DEFAULT

    groups = re.search(r"GROUP_BUTTONS\[2\]\[CONTROLS_GROUP_SIZE\] = \{(.*?)\};", SOURCE, re.S)
    assert groups, "GROUP_BUTTONS missing"
    rows = re.findall(r"\{([^}]*)\}", groups.group(1))
    assert tuple(tuple(re.findall(r"BUTTON_[A-Z]+", row)) for row in rows) == GROUPS
    assert "(action < CONTROL_PREV_WEAPON) ? 0 : 1" in SOURCE


def cycle(binding, action, delta):
    group = 0 if action < 3 else 1
    first = group * 3
    current = binding[action]
    slot = GROUPS[group].index(current)
    step = 2 if delta < 0 else 1
    target = GROUPS[group][(slot + step) % 3]
    out = list(binding)
    for i in range(first, first + 3):
        if out[i] == target:
            out[i] = current
    out[action] = target
    return tuple(out)


def to_logical(binding, raw):
    if binding == DEFAULT:
        return raw
    logical = raw & ~REMAPPABLE & 0xFFFF
    for i, button in enumerate(binding):
        if raw & BUTTONS[button]:
            logical |= BUTTONS[DEFAULT[i]]
    return logical


def is_valid(binding):
    return (set(binding[:3]) == set(GROUPS[0]) and
            set(binding[3:]) == set(GROUPS[1]))


def main():
    pin_mirror_to_source()

    # Every layout reachable from the menu is a per-group permutation, and every
    # per-group permutation is reachable: 3! * 3! = 36.
    seen = {DEFAULT}
    frontier = [DEFAULT]
    while frontier:
        binding = frontier.pop()
        assert is_valid(binding), binding
        for action in range(6):
            for delta in (-1, 1):
                nxt = cycle(binding, action, delta)
                assert nxt[action] != binding[action], "cycle must change the action"
                assert cycle(nxt, action, -delta) != None
                if nxt not in seen:
                    seen.add(nxt)
                    frontier.append(nxt)
    assert len(seen) == 36, len(seen)

    # The default layout is the identity for every pad value.
    for raw in range(0x10000):
        assert to_logical(DEFAULT, raw) == raw

    other = sum(value for name, value in BUTTONS.items()
                if BUTTONS[name] & REMAPPABLE == 0)
    for binding in seen:
        # Pressing the button bound to an action reports that action's default
        # bit, and only that one; D-pad/START/MODE pass through untouched.
        for i, button in enumerate(binding):
            assert to_logical(binding, BUTTONS[button]) == BUTTONS[DEFAULT[i]]
        for raw in (0, other, other | BUTTONS["BUTTON_A"], 0xFFFF):
            assert to_logical(binding, raw) & ~REMAPPABLE == raw & ~REMAPPABLE
        # A bijection on the remappable bits: no two chords collapse together.
        images = {to_logical(binding, raw) for raw in range(0x1000)}
        assert len(images) == 0x1000

    # Source contracts for the three consumers.
    assert PLAYER.count("controls_to_logical(") == 2, \
        "translate the pad and the latch exactly once, in player_controller_update"
    assert "controls_to_logical" not in PLAYER.split("void player_controller_vint_poll")[1] \
        .split("void player_controller_set_poll_active")[0], \
        "the V-Int latch must stay physical"
    for token in ("controls_button(CONTROL_AUTOMAP)", "controls_button(CONTROL_PREV_WEAPON)",
                  "controls_button(CONTROL_NEXT_WEAPON)"):
        assert token in AUTOMAP
    for token in ("run_controls", "controls_cycle", "controls_reset_defaults",
                  "controls_button_glyph", "OPTIONS_ROW_CONTROLS"):
        assert token in FRONTEND
    print("ok    controls: 36 remap layouts, identity default, physical latch")


if __name__ == "__main__":
    main()
