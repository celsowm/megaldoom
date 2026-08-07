#!/usr/bin/env python3
"""Contracts for 1:1 WAD geometry and 35 Hz Doom player movement."""

import importlib.util
import re
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EXTRACTOR_PATH = ROOT / "tools/wad-map-extract.py"
GENERATED_PATH = ROOT / "src/bsp/generated_e1m1_map.c"
CONTROLLER_PATH = ROOT / "src/player_controller.c"

DOOM_HZ = 35
VBLANK_HZ = 60
THRUST_SCALE = 2048
FRICTION_NUMERATOR = 29
FRICTION_DENOMINATOR = 32
MAX_MOVE = 30 << 16


def load_extractor():
    spec = importlib.util.spec_from_file_location("wad_map_scale", EXTRACTOR_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def generated_vertices(source):
    body = re.search(
        r"const BspVertex bsp_vertices\[\d+\] = \{(.*?)\n\};", source, re.S)
    assert body
    return [tuple(map(int, values)) for values in re.findall(
        r"\{\s*(-?\d+),\s*(-?\d+)\s*\}", body.group(1))]


def trunc_div(numerator, denominator):
    quotient = abs(numerator) // abs(denominator)
    return -quotient if (numerator < 0) != (denominator < 0) else quotient


def simulate_updates(seconds, cadence, command):
    momentum = 0
    remainder = 0
    position = 0
    accumulator = 0
    for _ in range(seconds * VBLANK_HZ // cadence):
        accumulator += cadence * DOOM_HZ
        while accumulator >= VBLANK_HZ:
            accumulator -= VBLANK_HZ
            momentum = max(-MAX_MOVE, min(MAX_MOVE,
                momentum + command * THRUST_SCALE))
            total = momentum + remainder
            whole = trunc_div(total, 65536)
            remainder = total - whole * 65536
            position += whole
            momentum -= (momentum * 3) >> 5
    return position


def simulate_reference(seconds, command):
    # One update per original Doom game tic.
    return simulate_updates(seconds, VBLANK_HZ, command)


def main():
    extractor = load_extractor()
    generated = GENERATED_PATH.read_text()
    controller = CONTROLLER_PATH.read_text()

    wad = extractor.WadFile(str(ROOT / "DOOM1.WAD"))
    raw = wad.map_lump("E1M1", "VERTEXES")
    original = [struct.unpack_from("<hh", raw, offset)
                for offset in range(0, len(raw), 4)]
    expected = [(x, -y) for x, y in original]
    assert generated_vertices(generated) == expected

    # Subsector material ownership must cover the original subsector set and
    # refer only to emitted E1M1 sectors.
    sector_body = re.search(
        r"const u16 bsp_subsector_sector\[\d+\] = \{(.*?)\n\};", generated, re.S)
    assert sector_body
    sector_ids = [int(value) for value in re.findall(r"\d+", sector_body.group(1))]
    assert len(sector_ids) == 237
    assert min(sector_ids) >= 0 and max(sector_ids) < 85

    required = (
        "#define DOOM_TICS_PER_SECOND 35",
        "#define VIDEO_VBLANKS_PER_SECOND 60",
        "#define DOOM_FORWARD_WALK 25",
        "#define DOOM_FORWARD_RUN 50",
        "#define DOOM_STRAFE_WALK 24",
        "#define DOOM_STRAFE_RUN 40",
        "#define DOOM_THRUST_SCALE 2048",
        "#define DOOM_FRICTION 0xE800L",
        "#define DOOM_MAX_MOVE (30L << 16)",
        "s_doom_tic_accumulator",
        "player_apply_world_push",
    )
    assert all(token in controller for token in required)
    assert "#define MOVE_MAX 180" not in controller

    for seconds in (1, 5, 10):
        for command in (25, 50, 24, 40):
            reference = simulate_reference(seconds, command)
            distances = [simulate_updates(seconds, cadence, command)
                         for cadence in (1, 2, 3)]
            assert all(abs(value - reference) <= 1 for value in distances), (
                seconds, command, reference, distances)

    walk = simulate_reference(10, 25)
    run = simulate_reference(10, 50)
    assert 2700 <= walk <= 3000, walk
    assert 5400 <= run <= 6000, run
    assert 1.9 <= run / walk <= 2.1
    print(f"ok    world scale: WAD vertices 1:1, walk={walk / 10:.1f}u/s, "
          f"run={run / 10:.1f}u/s at 60/30/20fps")


if __name__ == "__main__":
    main()
