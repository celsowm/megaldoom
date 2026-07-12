#!/usr/bin/env python3
"""Deterministic source/generated contract checks for the E1M1 sector MVP."""
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def wad_lumps(path):
    data = path.read_bytes()
    _, count, offset = struct.unpack_from("<4sii", data, 0)
    entries = []
    for i in range(count):
        pos, size, raw = struct.unpack_from("<ii8s", data, offset + i * 16)
        entries.append((raw.rstrip(b"\0").decode("ascii"), data[pos:pos + size]))
    return entries


def main():
    lumps = wad_lumps(ROOT / "DOOM1.WAD")
    marker = next(i for i, (name, _) in enumerate(lumps) if name == "E1M1")
    section = dict(lumps[marker + 1:marker + 12])
    expected = {
        "bsp_sector_count": len(section["SECTORS"]) // 26,
        "bsp_line_count": len(section["LINEDEFS"]) // 14,
        "bsp_render_seg_count": len(section["SEGS"]) // 12,
        "bsp_subsector_count": len(section["SSECTORS"]) // 4,
    }
    assert expected == {
        "bsp_sector_count": 85,
        "bsp_line_count": 475,
        "bsp_render_seg_count": 732,
        "bsp_subsector_count": 237,
    }, expected

    sectors = [struct.unpack_from("<hh8s8shhh", section["SECTORS"], i)
               for i in range(0, len(section["SECTORS"]), 26)]
    sides = [struct.unpack_from("<hh8s8s8sH", section["SIDEDEFS"], i)
             for i in range(0, len(section["SIDEDEFS"]), 30)]
    lines = [struct.unpack_from("<HHHHHHH", section["LINEDEFS"], i)
             for i in range(0, len(section["LINEDEFS"]), 14)]
    initial_step_heights = set()
    initial_step_textures = set()
    for line_id in (39, 40, 41, 50, 51, 52, 53, 54):
        _, _, _, _, _, right, left = lines[line_id]
        for side_id in (right, left):
            if side_id == 0xFFFF:
                continue
            _, _, upper, lower, _, sector_id = sides[side_id]
            initial_step_heights.add(sectors[sector_id][0])
            initial_step_textures.add(lower.rstrip(b"\0"))
    assert {-16, -8, 0}.issubset(initial_step_heights), initial_step_heights
    assert b"STEP6" in initial_step_textures, initial_step_textures

    generated = (ROOT / "src/generated_e1m1_map.c").read_text()
    for symbol, value in expected.items():
        match = re.search(rf"const u16 {symbol} = (\d+)u;", generated)
        assert match and int(match.group(1)) == value, (symbol, match)
    assert generated.count("const BspLine bsp_lines[") == 1
    assert generated.count("const BspRenderSeg bsp_render_segs[") == 1
    assert generated.count("const BspSector bsp_sectors[") == 1
    assert "MEGALDOOM_TEX_STEP6" in (ROOT / "src/generated_assets.h").read_text()
    print("sector map contract: 85 sectors, 475 lines, 732 segs, 237 subsectors")


if __name__ == "__main__":
    main()
