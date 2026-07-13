#!/usr/bin/env python3
"""Constructive navigation tests for the flattened-map route verifier."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
from types import SimpleNamespace

MODULE_PATH = Path(__file__).with_name("wad-flat-route.py")
SPEC = importlib.util.spec_from_file_location("wad_flat_route", MODULE_PATH)
assert SPEC and SPEC.loader
route = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = route
SPEC.loader.exec_module(route)

NO_SIDE = route.base.NO_SIDE


def rectangle(width=256, divider=False, key_type=None, enemies=0, ammo_box=False):
    vertices = [(0, 0), (width, 0), (width, 256), (0, 256)]
    if divider:
        vertices.extend([(width // 2, 0), (width // 2, 256)])
    sectors = [dict(floor=0, ceiling=128), dict(floor=0, ceiling=128)]
    sidedefs = []
    linedefs = []
    segs = []

    def add_line(v1, v2, special=0, flags=0, right_sector=0, left_sector=None):
        right = len(sidedefs)
        sidedefs.append(dict(sector=right_sector))
        left = NO_SIDE
        if left_sector is not None:
            left = len(sidedefs)
            sidedefs.append(dict(sector=left_sector))
        line_id = len(linedefs)
        linedefs.append(dict(v1=v1, v2=v2, flags=flags, special=special,
                             tag=0, right=right, left=left))
        segs.append(dict(v1=v1, v2=v2, linedef=line_id, direction=0, offset=0))
        if left != NO_SIDE:
            segs.append(dict(v1=v2, v2=v1, linedef=line_id, direction=1, offset=0))

    add_line(0, 1)
    add_line(1, 2, special=11)
    add_line(2, 3)
    add_line(3, 0)
    if divider:
        add_line(4, 5, special=26, flags=route.base.LINE_FLAG_IMPASSABLE,
                 right_sector=0, left_sector=1)

    things = []
    if key_type is not None:
        things.append(dict(x=128, y=128, angle=0, type=key_type, flags=2))
    for index in range(enemies):
        things.append(dict(x=96 + (index % 5) * 32,
                           y=64 + (index // 5) * 32,
                           angle=0, type=3004, flags=2))
    if ammo_box:
        things.append(dict(x=160, y=128, angle=0, type=2048, flags=2))

    return SimpleNamespace(vertices=vertices, sectors=sectors, sidedefs=sidedefs,
                           linedefs=linedefs, segs=segs, things=things)


def main():
    direct = route.verify_route(rectangle(), {"x": 64, "y": 128})
    assert direct["playable"], direct

    blue = route.verify_route(rectangle(512, divider=True, key_type=5),
                              {"x": 64, "y": 128})
    assert blue["playable"], blue
    assert blue["final_keys"] == ["blue"]

    yellow = route.verify_route(rectangle(512, divider=True, key_type=6),
                                {"x": 64, "y": 128})
    assert not yellow["playable"]

    no_ammo = route.verify_route(rectangle(enemies=17), {"x": 64, "y": 128})
    assert not no_ammo["playable"]
    assert "shots required" in no_ammo["reason"]

    medium_only = rectangle(512, divider=True, key_type=5)
    medium_only.things[0]["flags"] = 1
    suppressed = route.suppress_unspawned_keys(medium_only)
    assert suppressed == [0]
    assert medium_only.things[0]["type"] == 0

    print("ok    constructive route covers walls, coloured doors, targets, ammo, and skill flags")


if __name__ == "__main__":
    main()
