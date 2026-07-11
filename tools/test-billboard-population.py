"""Validate the source-faithful E1M1 THINGS and runtime skill policy counts."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parent.parent
MAP_SOURCE = ROOT / "src" / "generated_e1m1_map.c"

CURATED_TYPES = {
    5, 6, 9, 13, 34, 35, 43, 44, 45, 46, 47, 48,
    2007, 2011, 2012, 2014, 2015, 2018, 2019, 2028,
    2035, 2046, 2048, 3001, 3004,
}
ENEMY_TYPES = {9, 3001, 3004}
DECOR_TYPES = {34, 35, 43, 44, 45, 46, 47, 48, 2028, 2035, 2046}
MEDIUM_FLAG = 0x0002
NOT_SINGLE_PLAYER_FLAG = 0x0010


def main() -> int:
    text = MAP_SOURCE.read_text(encoding="utf-8")
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
    enemies = sum(thing[2] in ENEMY_TYPES for thing in runtime)
    decor = sum(thing[2] in DECOR_TYPES for thing in runtime)
    items = len(runtime) - enemies - decor

    actual = (len(curated), len(runtime), enemies, items, decor)
    expected = (102, 70, 6, 46, 18)
    if actual != expected:
        raise ValueError(
            "unexpected billboard population "
            f"curated/runtime/enemies/items/decor={actual}, expected={expected}"
        )

    print("ok    billboard population: 102 curated, 70 medium single-player "
          "(6 enemies, 46 items, 18 decor)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(f"error {error}", file=sys.stderr)
        raise SystemExit(1)
