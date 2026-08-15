#!/usr/bin/env python3
"""Navigation proof, colored-lock and atomic-emission regression tests."""
import importlib.util
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXTRACTOR = ROOT / "tools/wad-map-extract.py"
DOOM_MAP = ROOT / "tools/doom_map.py"


def load_extractor():
    """Load tools/doom_map.py: the flatten/door-group/seg-classify/certify
    module all the fixtures and certify_flat_progression() calls below
    exercise."""
    spec = importlib.util.spec_from_file_location("doom_map_progression", DOOM_MAP)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def seg(v1, v2, kind, key=0, group=255, flags=0):
    return {"v1": v1, "v2": v2, "type": kind,
            "required_key": key, "door_group": group, "flags": flags}


def fixture(module, key_thing=None, key_position=(256, 512), isolated=False,
            remote=False, include_exit=True):
    # Two 512x1024 rooms. The middle segment is the only connection and the
    # right wall is an exit switch, far outside start interaction range.
    vertices = [(0, 0), (1024, 0), (1024, 1024), (0, 1024),
                (512, 0), (512, 1024), (0, 448), (0, 576)]
    walls = [seg(0, 4, module.SEG_WALL), seg(4, 1, module.SEG_WALL),
             seg(3, 5, module.SEG_WALL), seg(5, 2, module.SEG_WALL),
             seg(0, 6, module.SEG_WALL), seg(7, 3, module.SEG_WALL)]
    middle_type = module.SEG_WALL if isolated else module.SEG_DOOR
    middle_flags = 0 if remote else module.SEG_FLAG_DIRECT_USE
    walls.append(seg(4, 5, middle_type, module.KEY_RED, 0, middle_flags))
    if include_exit:
        walls.append(seg(1, 2, module.SEG_EXIT))
    if remote:
        walls.append(seg(6, 7, module.SEG_SWITCH, module.KEY_NONE, 0))
    things = []
    if key_thing is not None:
        things.append((key_position[0], key_position[1], key_thing, 0,
                       module.DOOM_THING_SKILL_MEDIUM))
    return vertices, walls, things


def expect_failure(module, case, phrase):
    try:
        module.certify_flat_progression(*case, 128, 512)
    except ValueError as error:
        assert phrase in str(error), error
        return
    raise AssertionError("certificate unexpectedly succeeded")


def wad_without_e1m1_exit(source, target):
    data = bytearray(source.read_bytes())
    _, count, directory = struct.unpack_from("<4sii", data, 0)
    entries = []
    for index in range(count):
        pos, size, raw = struct.unpack_from("<ii8s", data, directory + index * 16)
        entries.append((raw.rstrip(b"\0").decode("ascii"), pos, size))
    marker = next(index for index, entry in enumerate(entries) if entry[0] == "E1M1")
    _, pos, size = next(entry for entry in entries[marker + 1:marker + 12]
                        if entry[0] == "LINEDEFS")
    changed = 0
    for offset in range(pos, pos + size, 14):
        special = struct.unpack_from("<H", data, offset + 6)[0]
        if special in (11, 51):
            struct.pack_into("<H", data, offset + 6, 0)
            changed += 1
    assert changed
    target.write_bytes(data)


def main():
    module = load_extractor()

    positive = fixture(module, 13)
    result = module.certify_flat_progression(*positive, 128, 512)
    assert result["key_mask"] == module.KEY_RED
    assert result["reached_masks"] == [0, module.KEY_RED]

    expect_failure(module, fixture(module, 13, (768, 512)), "exit unreachable")
    expect_failure(module, fixture(module, 5), "exit unreachable")
    expect_failure(module, fixture(module, 13, isolated=True), "exit unreachable")
    expect_failure(module, fixture(module, 13, include_exit=False),
                   "no supported exit")

    remote = fixture(module, 13, remote=True)
    remote_result = module.certify_flat_progression(*remote, 128, 512)
    assert remote_result["opened_groups"] & 1

    # A failed proof must leave both previously valid artifacts byte-identical.
    with tempfile.TemporaryDirectory() as temp_name:
        temp = Path(temp_name)
        bad_wad = temp / "no-exit.wad"
        out_map = temp / "map.c"
        out_assets = temp / "assets.h"
        wad_without_e1m1_exit(ROOT / "DOOM1.WAD", bad_wad)
        out_map.write_bytes(b"valid-map-sentinel\n")
        out_assets.write_bytes(b"valid-assets-sentinel\n")
        process = subprocess.run(
            [sys.executable, str(EXTRACTOR), "--wad", str(bad_wad),
             "--map", "E1M1", "--out", str(out_map),
             "--assets-out", str(out_assets)],
            text=True, capture_output=True)
        assert process.returncode != 0, process.stdout
        assert "no supported exit" in (process.stdout + process.stderr)
        assert out_map.read_bytes() == b"valid-map-sentinel\n"
        assert out_assets.read_bytes() == b"valid-assets-sentinel\n"

    print("ok    progression proof: RGB locks, self-lock/wrong-color/isolation, atomic failure")


if __name__ == "__main__":
    main()
