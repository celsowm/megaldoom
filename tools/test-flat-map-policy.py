#!/usr/bin/env python3
"""Tests that the progression graph mirrors the legacy textured extractor."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
from types import SimpleNamespace

MODULE_PATH = Path(__file__).with_name("wad-flat-preflight.py")
SPEC = importlib.util.spec_from_file_location("wad_flat_preflight", MODULE_PATH)
assert SPEC and SPEC.loader
module = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = module
SPEC.loader.exec_module(module)


def model(opening: int, special: int = 0, flags: int = 0):
    return SimpleNamespace(
        linedefs=[{"left": 1, "right": 0, "special": special, "flags": flags}],
        sidedefs=[{"sector": 0}, {"sector": 1}],
        sectors=[
            {"floor": 0, "ceiling": 64},
            {"floor": 64 - opening, "ceiling": 128},
        ],
    )


def main() -> None:
    closed = model(0)
    assert module.normalize_for_textured_legacy(closed) == [0]
    assert closed.linedefs[0]["flags"] & module.base.LINE_FLAG_IMPASSABLE

    open_portal = model(1)
    assert module.normalize_for_textured_legacy(open_portal) == []
    assert open_portal.linedefs[0]["flags"] == 0

    door = model(0, special=26)
    assert module.normalize_for_textured_legacy(door) == []

    impassable = model(32, flags=module.base.LINE_FLAG_IMPASSABLE)
    assert module.normalize_for_textured_legacy(impassable) == []

    print("ok    preflight collision policy matches the textured legacy extractor")


if __name__ == "__main__":
    main()
