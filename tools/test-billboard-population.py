"""Validate source-faithful E1M1/E1M2 THINGS and runtime populations."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parent.parent
RUNTIME_SOURCE = ROOT / "src" / "billboard" / "billboard.c"

# Keep in sync with map_thing_type() in src/billboard/billboard.c and with
# RUNTIME_THING_TYPES in tools/wad-map-extract.py.
CURATED_TYPES = {
    5, 6, 9, 13, 2001, 2002, 2005, 2007, 2008, 2011, 2012, 2014, 2015,
    2018, 2019, 2035, 2048, 2049, 3001, 3004,
}
ENEMY_TYPES = {9, 3001, 3004}
BARREL_TYPES = {2035}
SKILL_FLAGS = {
    "easy": 0x0001,
    "normal": 0x0002,
    "hard": 0x0004,
}
NOT_SINGLE_PLAYER_FLAG = 0x0010
MAX_RUNTIME_OBJECTS = 207

EXPECTED = {
    "e1m1": {
        "curated": 102,
        "easy": (62, 4, 52, 6),
        "normal": (64, 6, 52, 6),
        "hard": (88, 29, 53, 6),
    },
    "e1m2": {
        "curated": 212,
        "easy": (148, 20, 104, 24),
        "normal": (168, 41, 103, 24),
        "hard": (207, 79, 104, 24),
    },
}


def main() -> int:
    runtime_text = RUNTIME_SOURCE.read_text(encoding="utf-8")
    mapped_types = {
        int(value)
        for value in re.findall(r"case\s+(\d+)\s*:", runtime_text)
    }
    for map_name, expected in EXPECTED.items():
        text = (ROOT / "src" / "bsp" / f"generated_{map_name}_map.c").read_text()
        initializer = re.search(
            rf"static const BspThing {map_name}_bsp_things\[\d+\] = \{{(.*?)\n\}};",
            text, re.DOTALL)
        if not initializer:
            raise ValueError(f"could not find generated {map_name} THINGS initializer")
        things = [tuple(map(int, match)) for match in re.findall(
            r"\{\s*(-?\d+),\s*(-?\d+),\s*(\d+)u,\s*(\d+)u,\s*(\d+)u\}",
            initializer.group(1))]
        curated = [thing for thing in things if thing[2] in CURATED_TYPES]
        if len(curated) != expected["curated"]:
            raise ValueError(f"unexpected curated {map_name} count: {len(curated)}")
        populations = {name: [thing for thing in curated
            if (thing[4] & flag) and not (thing[4] & NOT_SINGLE_PLAYER_FLAG)]
            for name, flag in SKILL_FLAGS.items()}
        eligible_types = {thing[2] for runtime in populations.values() for thing in runtime}
        missing_mappings = sorted(eligible_types - mapped_types)
        if missing_mappings:
            raise ValueError(f"{map_name} THING mappings missing: {missing_mappings}")
        actual = {name: (len(runtime),
            sum(thing[2] in ENEMY_TYPES for thing in runtime),
            sum(thing[2] not in ENEMY_TYPES | BARREL_TYPES for thing in runtime),
            sum(thing[2] in BARREL_TYPES for thing in runtime))
            for name, runtime in populations.items()}
        expected_populations = {name: expected[name] for name in SKILL_FLAGS}
        if actual != expected_populations:
            raise ValueError(f"unexpected {map_name} populations: {actual}")
        if any(len(runtime) > MAX_RUNTIME_OBJECTS for runtime in populations.values()):
            raise ValueError(f"{map_name} population exceeds object pool")

    print("ok    billboard populations: E1M1 62/64/88, E1M2 148/168/207")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(f"error {error}", file=sys.stderr)
        raise SystemExit(1)
