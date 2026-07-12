"""Validate the source-faithful E1M1 THINGS and runtime skill policy counts."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parent.parent
MAP_SOURCE = ROOT / "src" / "generated_e1m1_map.c"
RUNTIME_SOURCE = ROOT / "src" / "billboard.c"

CURATED_TYPES = {
    5, 6, 9, 13, 2007, 2011, 2012, 2014, 2015, 2018, 2019,
    2035, 2048, 3001, 3004,
}
ENEMY_TYPES = {9, 3001, 3004}
BARREL_TYPES = {2035}
MEDIUM_FLAG = 0x0002
NOT_SINGLE_PLAYER_FLAG = 0x0010


def main() -> int:
    text = MAP_SOURCE.read_text(encoding="utf-8")
    runtime_text = RUNTIME_SOURCE.read_text(encoding="utf-8")
    initializer = re.search(
        r"const BspThing bsp_things\[\d+\] = \{(.*?)\n\};", text, re.DOTALL
    )
    if not initializer:
        raise ValueError("could not find generated bsp_things initializer")

    things = [
        tuple(map(int, match))
        for match in re.findall(
            r"\{\s*(-?\d+),\s*(-?\d+),\s*(\d+)u,\s*(\d+)u,\s*(\d+)u\}",
            initializer.group(1),
        )
    ]
    curated = [thing for thing in things if thing[2] in CURATED_TYPES]
    runtime = [
        thing for thing in curated
        if (thing[4] & MEDIUM_FLAG) and not (thing[4] & NOT_SINGLE_PLAYER_FLAG)
    ]
    mapped_types = {
        int(value)
        for value in re.findall(r"case\s+(\d+)\s*:", runtime_text)
    }
    eligible_types = {thing[2] for thing in runtime}
    missing_mappings = sorted(eligible_types - mapped_types)
    if missing_mappings:
        raise ValueError(
            "eligible E1M1 THING types have no map_thing_type mapping: "
            + ", ".join(map(str, missing_mappings))
        )
    enemies = sum(thing[2] in ENEMY_TYPES for thing in runtime)
    barrels = sum(thing[2] in BARREL_TYPES for thing in runtime)
    items = len(runtime) - enemies - barrels

    actual = (len(curated), len(runtime), enemies, items, barrels)
    expected = (87, 58, 6, 46, 6)
    if actual != expected:
        raise ValueError(
            "unexpected billboard population "
            f"curated/runtime/enemies/items/barrels={actual}, expected={expected}"
        )

    print("ok    billboard population: 87 useful, 58 medium single-player "
          "(6 enemies, 46 items, 6 barrels, 0 decor)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(f"error {error}", file=sys.stderr)
        raise SystemExit(1)
