#!/usr/bin/env python3
"""Build and validate a single-level, progression-safe Doom map conversion plan.

This tool is the semantic preflight for ``wad-map-extract.py``: it flattens
vertical traversal into open 2D portals, keeps walls/doors/textures
recognizable, preserves coloured key requirements, and proves that a route
from Player 1 start to an exit exists.
"""

from __future__ import annotations

import argparse
from collections import defaultdict, deque
from dataclasses import dataclass
import json
import os
import struct
import sys
from typing import Any, Iterable

KEY_BLUE = 0x01
KEY_YELLOW = 0x02
KEY_RED = 0x04
KEY_NAMES = {KEY_BLUE: "blue", KEY_YELLOW: "yellow", KEY_RED: "red"}
KEY_THINGS = {5: KEY_BLUE, 6: KEY_YELLOW, 13: KEY_RED}

DOOR_SPECIALS = {1, 26, 27, 28, 31, 32, 33, 34, 117, 118}
LOCKED_DOOR_SPECIALS = {
    26: KEY_BLUE,
    27: KEY_YELLOW,
    28: KEY_RED,
    32: KEY_BLUE,
    33: KEY_RED,
    34: KEY_YELLOW,
}
EXIT_SPECIALS = {11, 51}
LINE_FLAG_IMPASSABLE = 0x0001
NO_SIDE = 0xFFFF
LEAF_BIT = 0x8000


def clean_name(raw: bytes | str) -> str:
    if isinstance(raw, bytes):
        raw = raw.split(b"\x00", 1)[0].decode("ascii", "ignore")
    return raw.upper().rstrip("\x00").rstrip()


@dataclass(frozen=True)
class Lump:
    name: str
    filepos: int
    size: int


class WadFile:
    def __init__(self, path: str):
        with open(path, "rb") as handle:
            self.data = handle.read()
        if len(self.data) < 12:
            raise ValueError("WAD header is truncated")
        signature, count, directory_offset = struct.unpack_from("<4sii", self.data, 0)
        if signature not in (b"IWAD", b"PWAD"):
            raise ValueError("not an IWAD/PWAD file")
        self.lumps: list[Lump] = []
        for index in range(count):
            offset = directory_offset + index * 16
            filepos, size, raw_name = struct.unpack_from("<ii8s", self.data, offset)
            self.lumps.append(Lump(clean_name(raw_name), filepos, size))

    def map_lump(self, map_name: str, member: str) -> bytes:
        map_name = map_name.upper()
        member = member.upper()
        for index, lump in enumerate(self.lumps):
            if lump.name != map_name:
                continue
            for candidate in self.lumps[index + 1:index + 12]:
                if candidate.name == member:
                    return self.data[candidate.filepos:candidate.filepos + candidate.size]
                if is_map_marker(candidate.name):
                    break
            break
        raise KeyError(f"map {map_name}: lump {member} not found")


def is_map_marker(name: str) -> bool:
    return (
        len(name) == 4
        and name[0] == "E"
        and name[2] == "M"
        and name[1].isdigit()
        and name[3].isdigit()
    ) or (name.startswith("MAP") and name[3:].isdigit())


@dataclass
class MapModel:
    vertices: list[tuple[int, int]]
    sectors: list[dict[str, Any]]
    sidedefs: list[dict[str, Any]]
    linedefs: list[dict[str, Any]]
    segs: list[dict[str, Any]]
    subsectors: list[tuple[int, int]]
    nodes: list[dict[str, Any]]
    things: list[dict[str, Any]]


def parse_map(wad: WadFile, map_name: str) -> MapModel:
    vertices_raw = wad.map_lump(map_name, "VERTEXES")
    sectors_raw = wad.map_lump(map_name, "SECTORS")
    sidedefs_raw = wad.map_lump(map_name, "SIDEDEFS")
    linedefs_raw = wad.map_lump(map_name, "LINEDEFS")
    segs_raw = wad.map_lump(map_name, "SEGS")
    subsectors_raw = wad.map_lump(map_name, "SSECTORS")
    nodes_raw = wad.map_lump(map_name, "NODES")
    things_raw = wad.map_lump(map_name, "THINGS")

    vertices = [
        struct.unpack_from("<hh", vertices_raw, offset)
        for offset in range(0, len(vertices_raw), 4)
    ]

    sectors: list[dict[str, Any]] = []
    for offset in range(0, len(sectors_raw), 26):
        floor, ceiling, floor_name, ceiling_name, light, special, tag = struct.unpack_from(
            "<hh8s8shhh", sectors_raw, offset
        )
        sectors.append(
            {
                "floor": floor,
                "ceiling": ceiling,
                "floor_texture": clean_name(floor_name),
                "ceiling_texture": clean_name(ceiling_name),
                "light": light,
                "special": special,
                "tag": tag,
            }
        )

    sidedefs: list[dict[str, Any]] = []
    for offset in range(0, len(sidedefs_raw), 30):
        x_offset, y_offset, upper, lower, middle, sector = struct.unpack_from(
            "<hh8s8s8sH", sidedefs_raw, offset
        )
        sidedefs.append(
            {
                "x_offset": x_offset,
                "y_offset": y_offset,
                "upper": clean_name(upper),
                "lower": clean_name(lower),
                "middle": clean_name(middle),
                "sector": sector,
            }
        )

    linedefs: list[dict[str, Any]] = []
    for offset in range(0, len(linedefs_raw), 14):
        v1, v2, flags, special, tag, right, left = struct.unpack_from(
            "<HHHHHHH", linedefs_raw, offset
        )
        linedefs.append(
            {
                "v1": v1,
                "v2": v2,
                "flags": flags,
                "special": special,
                "tag": tag,
                "right": right,
                "left": left,
            }
        )

    segs: list[dict[str, Any]] = []
    for offset in range(0, len(segs_raw), 12):
        v1, v2, _angle, linedef, direction, seg_offset = struct.unpack_from(
            "<HHhHHh", segs_raw, offset
        )
        segs.append(
            {
                "v1": v1,
                "v2": v2,
                "linedef": linedef,
                "direction": direction,
                "offset": seg_offset,
            }
        )

    subsectors = [
        struct.unpack_from("<HH", subsectors_raw, offset)
        for offset in range(0, len(subsectors_raw), 4)
    ]

    nodes: list[dict[str, Any]] = []
    for offset in range(0, len(nodes_raw), 28):
        values = struct.unpack_from("<hhhhhhhhhhhhHH", nodes_raw, offset)
        nodes.append(
            {
                "x": values[0],
                "y": values[1],
                "dx": values[2],
                "dy": values[3],
                "right_child": values[12],
                "left_child": values[13],
            }
        )

    things: list[dict[str, Any]] = []
    for offset in range(0, len(things_raw), 10):
        x, y, angle, thing_type, flags = struct.unpack_from("<hhHHH", things_raw, offset)
        things.append(
            {"x": x, "y": y, "angle": angle, "type": thing_type, "flags": flags}
        )

    return MapModel(vertices, sectors, sidedefs, linedefs, segs, subsectors, nodes, things)


def front_side_for(model: MapModel, seg: dict[str, Any]) -> int:
    linedef = model.linedefs[seg["linedef"]]
    side = linedef["right"] if seg["direction"] == 0 else linedef["left"]
    if side == NO_SIDE:
        side = linedef["right"]
    return side


def subsector_sector(model: MapModel, subsector_index: int) -> int:
    if not 0 <= subsector_index < len(model.subsectors):
        raise ValueError(f"invalid subsector index {subsector_index}")
    count, first = model.subsectors[subsector_index]
    if count == 0:
        raise ValueError(f"subsector {subsector_index} has no segs")
    side = front_side_for(model, model.segs[first])
    if side == NO_SIDE:
        raise ValueError(f"subsector {subsector_index} has no front side")
    return int(model.sidedefs[side]["sector"])


def find_sector(model: MapModel, x: int, y: int) -> int:
    """Resolve a source-coordinate point through Doom's BSP tree."""
    child = len(model.nodes) - 1 if model.nodes else LEAF_BIT
    guard = 0
    while (child & LEAF_BIT) == 0:
        guard += 1
        if guard > len(model.nodes) + 1:
            raise ValueError("BSP traversal did not converge")
        if child >= len(model.nodes):
            raise ValueError(f"invalid BSP child {child}")
        node = model.nodes[child]
        cross = (x - node["x"]) * node["dy"] - (y - node["y"]) * node["dx"]
        child = node["left_child"] if cross <= 0 else node["right_child"]
    return subsector_sector(model, child & 0x7FFF)


def side_sector(model: MapModel, side_index: int) -> int | None:
    if side_index == NO_SIDE:
        return None
    if not 0 <= side_index < len(model.sidedefs):
        raise ValueError(f"invalid sidedef index {side_index}")
    return int(model.sidedefs[side_index]["sector"])


def first_texture(side: dict[str, Any] | None) -> str | None:
    if not side:
        return None
    for field in ("middle", "lower", "upper"):
        value = side[field]
        if value and value != "-":
            return value
    return None


def key_name(mask: int) -> str | None:
    return KEY_NAMES.get(mask)


def key_mask_for_special(special: int) -> int:
    return LOCKED_DOOR_SPECIALS.get(special, 0)


def masks_to_names(mask: int) -> list[str]:
    return [name for bit, name in KEY_NAMES.items() if mask & bit]


def solve_progression(
    sector_count: int,
    adjacency: dict[int, list[dict[str, Any]]],
    keys_by_sector: dict[int, int],
    start_sector: int,
    exit_sectors: Iterable[int],
) -> dict[str, Any]:
    exits = set(exit_sectors)
    if not 0 <= start_sector < sector_count:
        return {"playable": False, "reason": "player start is outside the sector graph"}
    if not exits:
        return {"playable": False, "reason": "map has no supported exit linedef"}

    start_mask = keys_by_sector.get(start_sector, 0)
    start_state = (start_sector, start_mask)
    queue = deque([start_state])
    previous: dict[tuple[int, int], tuple[tuple[int, int], dict[str, Any]] | None] = {
        start_state: None
    }
    goal: tuple[int, int] | None = None

    while queue:
        sector, key_mask = queue.popleft()
        if sector in exits:
            goal = (sector, key_mask)
            break
        for edge in adjacency.get(sector, []):
            required = int(edge.get("required_key_mask", 0))
            if required and (key_mask & required) != required:
                continue
            destination = int(edge["to"])
            next_mask = key_mask | keys_by_sector.get(destination, 0)
            state = (destination, next_mask)
            if state in previous:
                continue
            previous[state] = ((sector, key_mask), edge)
            queue.append(state)

    if goal is None:
        reachable_sectors = sorted({sector for sector, _mask in previous})
        acquired = 0
        for _sector, mask in previous:
            acquired |= mask
        return {
            "playable": False,
            "reason": "no route reaches an exit with the available coloured keys",
            "reachable_sectors": reachable_sectors,
            "acquirable_keys": masks_to_names(acquired),
        }

    transitions: list[dict[str, Any]] = []
    cursor = goal
    while previous[cursor] is not None:
        parent, edge = previous[cursor]  # type: ignore[misc]
        destination, destination_mask = cursor
        parent_mask = parent[1]
        transitions.append(
            {
                "from_sector": parent[0],
                "to_sector": destination,
                "linedef": edge.get("linedef"),
                "kind": edge.get("kind", "portal"),
                "required_key": key_name(int(edge.get("required_key_mask", 0))),
                "new_keys": masks_to_names(destination_mask & ~parent_mask),
            }
        )
        cursor = parent
    transitions.reverse()
    return {
        "playable": True,
        "start_sector": start_sector,
        "start_keys": masks_to_names(start_mask),
        "exit_sector": goal[0],
        "final_keys": masks_to_names(goal[1]),
        "route": transitions,
        "visited_state_count": len(previous),
    }


def build_conversion_plan(model: MapModel, map_name: str, wad_name: str) -> dict[str, Any]:
    diagnostics: list[dict[str, Any]] = []
    player_starts = [thing for thing in model.things if thing["type"] == 1]
    if not player_starts:
        raise ValueError("Player 1 start (THING type 1) is missing")
    player_start = player_starts[0]
    start_sector = find_sector(model, player_start["x"], player_start["y"])

    keys_by_sector: dict[int, int] = defaultdict(int)
    keys: list[dict[str, Any]] = []
    for index, thing in enumerate(model.things):
        key_mask = KEY_THINGS.get(thing["type"], 0)
        if not key_mask:
            continue
        sector = find_sector(model, thing["x"], thing["y"])
        keys_by_sector[sector] |= key_mask
        keys.append(
            {
                "thing_index": index,
                "color": key_name(key_mask),
                "sector": sector,
                "x": thing["x"],
                "y": thing["y"],
            }
        )

    adjacency: dict[int, list[dict[str, Any]]] = defaultdict(list)
    flattened_lines: list[dict[str, Any]] = []
    doors: list[dict[str, Any]] = []
    exit_sectors: set[int] = set()

    for line_index, linedef in enumerate(model.linedefs):
        right_sector = side_sector(model, linedef["right"])
        left_sector = side_sector(model, linedef["left"])
        right_side = None if linedef["right"] == NO_SIDE else model.sidedefs[linedef["right"]]
        left_side = None if linedef["left"] == NO_SIDE else model.sidedefs[linedef["left"]]
        texture = first_texture(right_side) or first_texture(left_side)
        special = int(linedef["special"])
        required_key_mask = key_mask_for_special(special)
        is_door = special in DOOR_SPECIALS
        is_exit = special in EXIT_SPECIALS

        if is_exit:
            if right_sector is not None:
                exit_sectors.add(right_sector)
            if left_sector is not None:
                exit_sectors.add(left_sector)

        if right_sector is None or left_sector is None:
            flattened_lines.append(
                {
                    "linedef": line_index,
                    "kind": "exit_switch_wall" if is_exit else "wall",
                    "texture": texture,
                    "special": special,
                }
            )
            continue

        if right_sector == left_sector:
            flattened_lines.append(
                {
                    "linedef": line_index,
                    "kind": "same_sector_detail",
                    "texture": texture,
                    "special": special,
                }
            )
            continue

        if (linedef["flags"] & LINE_FLAG_IMPASSABLE) and not is_door:
            flattened_lines.append(
                {
                    "linedef": line_index,
                    "kind": "impassable_wall",
                    "texture": texture,
                    "special": special,
                }
            )
            continue

        if is_door:
            kind = "locked_door" if required_key_mask else "door"
            doors.append(
                {
                    "linedef": line_index,
                    "kind": kind,
                    "special": special,
                    "required_key": key_name(required_key_mask),
                    "texture": texture,
                    "right_sector": right_sector,
                    "left_sector": left_sector,
                }
            )
        else:
            kind = "open_portal"
            right = model.sectors[right_sector]
            left = model.sectors[left_sector]
            if right["floor"] != left["floor"] or right["ceiling"] != left["ceiling"]:
                diagnostics.append(
                    {
                        "level": "info",
                        "code": "HEIGHT_PORTAL_FLATTENED",
                        "linedef": line_index,
                        "message": "vertical difference becomes a same-level open portal",
                    }
                )
            if special and not is_exit:
                diagnostics.append(
                    {
                        "level": "warning",
                        "code": "REMOTE_SPECIAL_NORMALIZED",
                        "linedef": line_index,
                        "special": special,
                        "message": "unsupported non-key trigger is normalized to an always-open portal",
                    }
                )

        edge = {
            "linedef": line_index,
            "kind": kind,
            "required_key_mask": required_key_mask,
        }
        adjacency[right_sector].append({**edge, "to": left_sector})
        adjacency[left_sector].append({**edge, "to": right_sector})
        flattened_lines.append(
            {
                "linedef": line_index,
                "kind": kind,
                "texture": texture,
                "special": special,
                "required_key": key_name(required_key_mask),
                "right_sector": right_sector,
                "left_sector": left_sector,
            }
        )

    for door in doors:
        color = door["required_key"]
        if color and not any(key["color"] == color for key in keys):
            diagnostics.append(
                {
                    "level": "warning",
                    "code": "LOCK_COLOR_HAS_NO_KEY",
                    "linedef": door["linedef"],
                    "message": f"{color} door exists but no {color} key THING exists",
                }
            )

    progression = solve_progression(
        len(model.sectors), adjacency, dict(keys_by_sector), start_sector, exit_sectors
    )
    if not progression["playable"]:
        diagnostics.append(
            {
                "level": "error",
                "code": "NO_COMPLETION_ROUTE",
                "message": progression["reason"],
            }
        )

    return {
        "schema": "megaldoom.flat-map-plan.v1",
        "source": {"wad": wad_name, "map": map_name.upper()},
        "conversion_mode": "single_level_textured_bsp",
        "contracts": {
            "render_sector_heights": False,
            "preserve_wall_textures": True,
            "preserve_door_textures": True,
            "preserve_coloured_keys": True,
            "keys_are_reusable": True,
            "completion_route_required": True,
        },
        "player_start": {
            "x": player_start["x"],
            "y": player_start["y"],
            "angle": player_start["angle"],
            "sector": start_sector,
        },
        "keys": keys,
        "doors": doors,
        "exit_sectors": sorted(exit_sectors),
        "flattened_lines": flattened_lines,
        "progression": progression,
        "diagnostics": diagnostics,
        "summary": {
            "sectors": len(model.sectors),
            "linedefs": len(model.linedefs),
            "keys": len(keys),
            "doors": len(doors),
            "height_portals_flattened": sum(
                1 for diagnostic in diagnostics if diagnostic["code"] == "HEIGHT_PORTAL_FLATTENED"
            ),
            "playable": bool(progression["playable"]),
        },
    }


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wad", default="DOOM1.WAD", help="source IWAD/PWAD")
    parser.add_argument("--map", default="E1M1", help="map marker, for example E1M1")
    parser.add_argument("--out", default=None, help="JSON conversion plan path")
    parser.add_argument(
        "--allow-unplayable",
        action="store_true",
        help="write the report but do not fail when no completion route exists",
    )
    parser.add_argument("--quiet", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    output = args.out or os.path.join("out", f"{args.map.lower()}-flat-plan.json")
    try:
        wad = WadFile(args.wad)
        model = parse_map(wad, args.map.upper())
        plan = build_conversion_plan(model, args.map, os.path.basename(args.wad))
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
            "flattened-height-portals={height_portals_flattened}".format(**summary)
        )
        print("  progression: " + ("PLAYABLE" if summary["playable"] else "UNPLAYABLE"))
        if plan["progression"].get("route"):
            print(f"  validated route transitions: {len(plan['progression']['route'])}")

    if not plan["progression"]["playable"] and not args.allow_unplayable:
        return 3
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
