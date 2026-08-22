"""Validate the source-faithful E1M1 THINGS and each runtime skill population."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parent.parent
MAP_SOURCE = ROOT / "src" / "bsp" / "generated_e1m1_map.c"
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
MAX_RUNTIME_OBJECTS = 112


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
    # The current 76A222...E6F9 WAD contains four additional supported THINGS
    # relative to the stale generated artifact.  Skill/single-player filtering
    # removes them, so the three actual runtime populations remain unchanged.
    if len(curated) != 102:
        raise ValueError(f"unexpected curated E1M1 THING count: {len(curated)}")
    populations = {
        name: [
            thing for thing in curated
            if (thing[4] & flag) and not (thing[4] & NOT_SINGLE_PLAYER_FLAG)
        ]
        for name, flag in SKILL_FLAGS.items()
    }
    mapped_types = {
        int(value)
        for value in re.findall(r"case\s+(\d+)\s*:", runtime_text)
    }
    eligible_types = {thing[2] for runtime in populations.values() for thing in runtime}
    missing_mappings = sorted(eligible_types - mapped_types)
    if missing_mappings:
        raise ValueError(
            "eligible E1M1 THING types have no map_thing_type mapping: "
            + ", ".join(map(str, missing_mappings))
        )
    actual = {
        name: (
            len(runtime),
            sum(thing[2] in ENEMY_TYPES for thing in runtime),
            sum(thing[2] not in ENEMY_TYPES | BARREL_TYPES for thing in runtime),
            sum(thing[2] in BARREL_TYPES for thing in runtime),
        )
        for name, runtime in populations.items()
    }
    expected = {
        "easy": (62, 4, 52, 6),
        "normal": (64, 6, 52, 6),
        "hard": (88, 29, 53, 6),
    }
    if actual != expected:
        raise ValueError(
            "unexpected billboard skill populations "
            f"actual={actual}, expected={expected}"
        )
    if any(len(runtime) > MAX_RUNTIME_OBJECTS for runtime in populations.values()):
        raise ValueError("a skill population exceeds BILLBOARD_OBJECT_COUNT")

    print("ok    billboard populations: easy 62, normal 64, hard 88 "
          "single-player objects (all fit the 112-object pool)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(f"error {error}", file=sys.stderr)
        raise SystemExit(1)
