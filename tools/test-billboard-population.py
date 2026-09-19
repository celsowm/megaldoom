"""Validate source-faithful campaign THINGS and runtime populations."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parent.parent
RUNTIME_SOURCE = ROOT / "src" / "billboard" / "billboard.c"
INTERNAL_SOURCE = ROOT / "src" / "billboard" / "billboard_internal.h"
sys.path.insert(0, str(ROOT / "tools"))
import doom_map  # noqa: E402

# Keep in sync with map_thing_type() in src/billboard/billboard.c and with
# RUNTIME_THING_TYPES in tools/wad-map-extract.py.
CURATED_TYPES = {
    5, 6, 9, 13, 58, 2001, 2002, 2005, 2007, 2008, 2011, 2012, 2014, 2015,
    2018, 2019, 2035, 2048, 2049, 3001, 3002, 3004,
}
# The demon (3002) and the spectre (58, drawn as the demon for now) joined the
# runtime with E1M5 on 2026-09-18; E1M3 and E1M4 had been dropping theirs.
ENEMY_TYPES = {9, 58, 3001, 3002, 3004}
BARREL_TYPES = {2035}
SKILL_FLAGS = {
    "easy": 0x0001,
    "normal": 0x0002,
    "hard": 0x0004,
}
NOT_SINGLE_PLAYER_FLAG = 0x0010
LIMITS = ROOT / "src" / "bsp" / "generated_map_limits.h"

# 102 -> 98: the previous WAD did not match any official IWAD checksum (see
# tools/wad-map-extract.py); its E1M1 carried an extra multiplayer-only loot
# cluster (3 barrels + a shotgun, DOOM_THING flag 0x10 set) near the secret
# room that the verified Doom Registered v1.9 IWAD (md5
# 1cd63c5ddff1bf8ce844237f580e9cf3) does not have. That cluster was always
# excluded from every single-player population by the NOT_SINGLE_PLAYER_FLAG
# check below, so easy/normal/hard are unchanged byte-for-byte -- only the
# multiplayer-only THING count moves.
EXPECTED = {
    "e1m1": {
        "curated": 98,
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
    # E1M3 on hard is the campaign's largest population, and so sets
    # MEGALDOOM_MAP_MAX_ACTIVE_THINGS: 317 before its 9 demons were spawned.
    "e1m3": {
        "curated": 327,
        "easy": (216, 34, 156, 26),
        "normal": (269, 74, 167, 28),
        "hard": (326, 131, 167, 28),
    },
    "e1m4": {
        "curated": 208,
        "easy": (136, 21, 83, 32),
        "normal": (168, 54, 82, 32),
        "hard": (197, 85, 80, 32),
    },
    # Normal: 28 zombiemen, 24 shotgun guys, 26 imps, 4 demons, 9 spectres.
    "e1m5": {
        "curated": 265,
        "easy": (137, 24, 85, 28),
        "normal": (201, 91, 82, 28),
        "hard": (242, 131, 83, 28),
    },
}


def table_sets(runtime_text: str, internal_text: str):
    """The Doom THING types billboard.c spawns as monsters, as targets and as
    blockers, read from map_thing_type() and the BILLBOARD_TYPES flags."""
    type_ids = {name: int(value) for name, value in re.findall(
        r"#define BILLBOARD_TYPE_(\w+) (\d+)", internal_text)}
    table = runtime_text[runtime_text.index("BILLBOARD_TYPES[BILLBOARD_TYPE_COUNT] = {"):]
    table = table[:table.index("};")]
    rows = [re.findall(r"\b(TRUE|FALSE)\b", row)
            for row in re.findall(r"\{(BILLBOARD_VISUAL_[^}]*)\}", table)]
    switch = runtime_text[runtime_text.index("static u8 map_thing_type"):]
    switch = switch[:switch.index("default:")]
    spawned = {}
    for cases, type_name in re.findall(
            r"((?:case\s+\d+:\s*)+)[^;]*;\s*return BILLBOARD_TYPE_(\w+);", switch):
        for doom_type in re.findall(r"\d+", cases):
            spawned[int(doom_type)] = type_ids[type_name]
    monsters = {t for t, type_id in spawned.items() if type_id == type_ids["DUMMY"]}
    targets = {t for t, type_id in spawned.items() if rows[type_id][1] == "TRUE"}
    blockers = {t for t, type_id in spawned.items() if rows[type_id][2] == "TRUE"}
    return monsters, targets, blockers


def check_table(runtime_text: str, internal_text: str) -> None:
    """The per-enemy arrays are sized by ENEMY_THING_TYPES and the target and
    blocking registries by TARGET_THING_TYPES, so the runtime must spawn
    exactly those as monsters and targets, and block only with targets."""
    monsters, targets, blockers = table_sets(runtime_text, internal_text)
    if monsters != doom_map.ENEMY_THING_TYPES:
        raise ValueError(f"billboard.c monsters {sorted(monsters)} != ENEMY_THING_TYPES")
    if targets != doom_map.TARGET_THING_TYPES:
        raise ValueError(f"billboard.c targets {sorted(targets)} != TARGET_THING_TYPES")
    if not blockers <= targets:
        raise ValueError(f"blocking non-targets {sorted(blockers - targets)} would "
                         "overflow s_blocking_indices[BILLBOARD_TARGET_COUNT]")


def main() -> int:
    runtime_text = RUNTIME_SOURCE.read_text(encoding="utf-8")
    internal_text = INTERNAL_SOURCE.read_text(encoding="utf-8")
    limits_text = LIMITS.read_text(encoding="utf-8")
    ceilings = {name: int(re.search(
        rf"#define MEGALDOOM_MAP_MAX_ACTIVE_{name} (\d+)", limits_text).group(1))
        for name in ("THINGS", "ENEMIES", "TARGETS")}
    max_runtime_objects = ceilings["THINGS"]
    if (ENEMY_TYPES != doom_map.ENEMY_THING_TYPES or
            ENEMY_TYPES | BARREL_TYPES != doom_map.TARGET_THING_TYPES):
        raise ValueError("doom_map ENEMY/TARGET_THING_TYPES drifted from this test's sets")
    check_table(runtime_text, internal_text)
    # Negative controls: a spawned decor prop (blocking, not targetable), and
    # a monster that stops being a DUMMY, must both be caught.
    for label, broken in (
            ("spawned candle", runtime_text.replace(
                "        case 2035:",
                "        case 34: *visual = BILLBOARD_VISUAL_CANDLE; "
                "return BILLBOARD_TYPE_CANDLE;\n        case 2035:")),
            ("spectre not a monster", runtime_text.replace(
                "case 3002: case 58: *visual = BILLBOARD_VISUAL_DEMON; return BILLBOARD_TYPE_DUMMY;",
                "case 3002: *visual = BILLBOARD_VISUAL_DEMON; return BILLBOARD_TYPE_DUMMY;\n"
                "        case 58: *visual = BILLBOARD_VISUAL_DEMON; return BILLBOARD_TYPE_BONUS;"))):
        if broken == runtime_text:
            raise ValueError(f"negative control {label!r} did not apply")
        try:
            check_table(broken, internal_text)
        except ValueError:
            continue
        raise ValueError(f"negative control {label!r} was not caught")
    peaks = {"THINGS": 0, "ENEMIES": 0, "TARGETS": 0}
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
        if any(len(runtime) > max_runtime_objects for runtime in populations.values()):
            raise ValueError(f"{map_name} population exceeds object pool")
        for objects, enemies, _, barrels in actual.values():
            peaks["THINGS"] = max(peaks["THINGS"], objects)
            peaks["ENEMIES"] = max(peaks["ENEMIES"], enemies)
            peaks["TARGETS"] = max(peaks["TARGETS"], enemies + barrels)

    # Each generated ceiling is exactly the campaign's largest population of
    # its kind: no smaller (overflow), no larger (wasted work RAM).
    if peaks != ceilings:
        raise ValueError(f"generated ceilings {ceilings} != campaign peaks {peaks}")

    print(f"ok    billboard populations: E1M1..E1M{len(EXPECTED)}; pool {ceilings['THINGS']} "
          f"objects, {ceilings['ENEMIES']} monsters, {ceilings['TARGETS']} targets; "
          "type table matches, 2 negative controls caught")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(f"error {error}", file=sys.stderr)
        raise SystemExit(1)
