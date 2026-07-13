#!/usr/bin/env python3
"""Run the flat-map analyser with the exact legacy-renderer collision policy.

The textured single-level extractor keeps a two-sided line solid when its
source opening is fully closed. This adapter marks those unsupported lift/
moving-floor portals as impassable before the progression search so the proof
matches the C geometry that will actually be generated.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
from pathlib import Path
import struct
import sys
from typing import Any

MODULE_PATH = Path(__file__).with_name("wad-flat-playable.py")
SPEC = importlib.util.spec_from_file_location("wad_flat_playable", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot load {MODULE_PATH}")
base = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = base
SPEC.loader.exec_module(base)


def normalize_for_textured_legacy(model: Any) -> list[int]:
    """Make the analyser's graph match wad-map-extract.py's solid-line policy."""
    closed: list[int] = []
    for index, linedef in enumerate(model.linedefs):
        if linedef["left"] == base.NO_SIDE or linedef["right"] == base.NO_SIDE:
            continue
        if linedef["special"] in base.DOOR_SPECIALS:
            continue
        if linedef["flags"] & base.LINE_FLAG_IMPASSABLE:
            continue
        right_sector = model.sidedefs[linedef["right"]]["sector"]
        left_sector = model.sidedefs[linedef["left"]]["sector"]
        right = model.sectors[right_sector]
        left = model.sectors[left_sector]
        opening = min(right["ceiling"], left["ceiling"]) - max(
            right["floor"], left["floor"]
        )
        if opening <= 0:
            linedef["flags"] |= base.LINE_FLAG_IMPASSABLE
            closed.append(index)
    return closed


def build_plan(wad_path: str, map_name: str) -> dict[str, Any]:
    wad = base.WadFile(wad_path)
    model = base.parse_map(wad, map_name.upper())
    closed = normalize_for_textured_legacy(model)
    plan = base.build_conversion_plan(model, map_name, os.path.basename(wad_path))
    for linedef in closed:
        plan["diagnostics"].append(
            {
                "level": "warning",
                "code": "UNSUPPORTED_CLOSED_PORTAL",
                "linedef": linedef,
                "message": (
                    "fully closed non-door sector machinery remains a wall in "
                    "the single-level textured renderer"
                ),
            }
        )
    plan["summary"]["unsupported_closed_portals"] = len(closed)
    plan["contracts"]["progression_matches_generated_collision"] = True
    return plan


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wad", default="DOOM1.WAD")
    parser.add_argument("--map", default="E1M1")
    parser.add_argument("--out", default=None)
    parser.add_argument("--allow-unplayable", action="store_true")
    parser.add_argument("--quiet", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    output = args.out or os.path.join("out", f"{args.map.lower()}-flat-plan.json")
    try:
        plan = build_plan(args.wad, args.map)
    except (OSError, KeyError, ValueError, struct.error) as error:
        print(f"flat-map preflight failed: {error}", file=sys.stderr)
        return 2

    os.makedirs(os.path.dirname(os.path.abspath(output)), exist_ok=True)
    with open(output, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(plan, handle, indent=2, sort_keys=True)
        handle.write("\n")

    if not args.quiet:
        summary = plan["summary"]
        print(f"Wrote {output}")
        print(
            "  sectors={sectors} linedefs={linedefs} doors={doors} keys={keys} "
            "flattened-height-portals={height_portals_flattened} "
            "closed-portals={unsupported_closed_portals}".format(**summary)
        )
        print("  progression: " + ("PLAYABLE" if summary["playable"] else "UNPLAYABLE"))
        if plan["progression"].get("route"):
            print(f"  validated route transitions: {len(plan['progression']['route'])}")

    if not plan["progression"]["playable"] and not args.allow_unplayable:
        return 3
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
