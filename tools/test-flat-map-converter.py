#!/usr/bin/env python3
"""Deterministic tests for the flat-map progression solver."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import sys

MODULE_PATH = Path(__file__).with_name("wad-flat-playable.py")
SPEC = importlib.util.spec_from_file_location("wad_flat_playable", MODULE_PATH)
assert SPEC and SPEC.loader
module = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = module
SPEC.loader.exec_module(module)


def edge(to: int, linedef: int, key: int = 0, kind: str = "open_portal"):
    return {
        "to": to,
        "linedef": linedef,
        "kind": kind,
        "required_key_mask": key,
    }


def solve(adjacency, keys, exits, sectors=8):
    return module.solve_progression(sectors, adjacency, keys, 0, exits)


def main() -> None:
    direct = solve({0: [edge(1, 10)]}, {}, {1})
    assert direct["playable"]

    blue_route = solve(
        {
            0: [edge(1, 11)],
            1: [edge(0, 11), edge(2, 12, module.KEY_BLUE, "locked_door")],
            2: [edge(1, 12, module.KEY_BLUE, "locked_door")],
        },
        {1: module.KEY_BLUE},
        {2},
    )
    assert blue_route["playable"]
    assert blue_route["final_keys"] == ["blue"]
    assert blue_route["route"][1]["required_key"] == "blue"

    missing_key = solve(
        {0: [edge(1, 13, module.KEY_BLUE, "locked_door")]},
        {},
        {1},
    )
    assert not missing_key["playable"]

    wrong_color = solve(
        {0: [edge(1, 14)], 1: [edge(2, 15, module.KEY_RED, "locked_door")]},
        {1: module.KEY_YELLOW},
        {2},
    )
    assert not wrong_color["playable"]

    # Doom keys are reusable. One blue key must open multiple blue doors.
    reusable = solve(
        {
            0: [edge(1, 20)],
            1: [edge(2, 21, module.KEY_BLUE, "locked_door")],
            2: [edge(3, 22, module.KEY_BLUE, "locked_door")],
        },
        {1: module.KEY_BLUE},
        {3},
    )
    assert reusable["playable"]
    assert reusable["final_keys"] == ["blue"]

    # Backtracking after collecting a key is represented by a distinct state.
    backtrack = solve(
        {
            0: [edge(1, 30), edge(2, 31, module.KEY_RED, "locked_door")],
            1: [edge(0, 30)],
            2: [],
        },
        {1: module.KEY_RED},
        {2},
    )
    assert backtrack["playable"]
    assert len(backtrack["route"]) == 3

    assert module.key_mask_for_special(26) == module.KEY_BLUE
    assert module.key_mask_for_special(27) == module.KEY_YELLOW
    assert module.key_mask_for_special(28) == module.KEY_RED
    assert module.key_mask_for_special(1) == 0

    print("ok    flat map progression, coloured locks, reusable keys, and backtracking")


if __name__ == "__main__":
    main()
