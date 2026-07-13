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


def normalize_for_textured_legacy(model: Any) -> dict[str, list[int]]:
    """Make the analyser graph match wad-map-extract.py exactly."""
    changes = {
        "closed_portals": [],
        "open_door_specials": [],
        "open_exit_specials": [],
    }
    for index, linedef in enumerate(model.linedefs):
        one_sided = (
            linedef["left"] == base.NO_SIDE or linedef["right"] == base.NO_SIDE
        )
        impassable = bool(linedef["flags"] & base.LINE_FLAG_IMPASSABLE)
        opening = 0
        if not one_sided:
            right_sector = model.sidedefs[linedef["right"]]["sector"]
            left_sector = model.sidedefs[linedef["left"]]["sector"]
            right = model.sectors[right_sector]
            left = model.sectors[left_sector]
            opening = min(right["ceiling"], left["ceiling"]) - max(
                right["floor"], left["floor"]
            )

        emitted_as_seg = one_sided or impassable or opening <= 0
        special = linedef["special"]
        if special in base.DOOR_SPECIALS and not emitted_as_seg:
            # The legacy extractor skips this line, so it cannot be an
            # interactive/keyed edge in the proof.
            linedef["special"] = 0
            changes["open_door_specials"].append(index)
            continue
        if special in base.EXIT_SPECIALS and not emitted_as_seg:
            # No BspSeg means the runtime has no usable exit at this line.
            linedef["special"] = 0
            changes["open_exit_specials"].append(index)
            continue
        if (not one_sided and not impassable and opening <= 0 and
                special not in base.DOOR_SPECIALS):
            linedef["flags"] |= base.LINE_FLAG_IMPASSABLE
            changes["closed_portals"].append(index)
    return changes


def build_plan(wad_path: str, map_name: str) -> dict[str, Any]:
    wad = base.WadFile(wad_path)
    model = base.parse_map(wad, map_name.upper())
    changes = normalize_for_textured_legacy(model)
    plan = base.build_conversion_plan(model, map_name, os.path.basename(wad_path))
    for linedef in changes["closed_portals"]:
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
    for linedef in changes["open_door_specials"]:
        plan["diagnostics"].append(
            {
                "level": "warning",
                "code": "NON_EMITTED_DOOR_SPECIAL",
                "linedef": linedef,
                "message": "door special lies on a passable line skipped by the legacy extractor",
            }
        )
    for linedef in changes["open_exit_specials"]:
        plan["diagnostics"].append(
            {
                "level": "error",
                "code": "NON_EMITTED_EXIT_SPECIAL",
                "linedef": linedef,
                "message": "exit special lies on a line that produces no runtime segment",
            }
        )
    plan["summary"]["unsupported_closed_portals"] = len(changes["closed_portals"])
    plan["summary"]["non_emitted_door_specials"] = len(changes["open_door_specials"])
    plan["summary"]["non_emitted_exit_specials"] = len(changes["open_exit_specials"])
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
            "closed-portals={unsupported_closed_portals} "
            "non-emitted-specials={non_emitted_door_specials}/{non_emitted_exit_specials}".format(**summary)
        )
        print("  progression: " + ("PLAYABLE" if summary["playable"] else "UNPLAYABLE"))
        if plan["progression"].get("route"):
            print(f"  validated route transitions: {len(plan['progression']['route'])}")

    if not plan["progression"]["playable"] and not args.allow_unplayable:
        return 3
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
