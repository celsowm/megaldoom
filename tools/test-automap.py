#!/usr/bin/env python3
"""Automap conversion, discovery and bounded raster contracts."""
from collections import Counter
from dataclasses import fields
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import doom_map
from wad_reader import WadFile


EXPECTED = {
    "E1M1": (451, Counter({0: 303, 1: 97, 2: 35, 3: 16})),
    "E1M2": (1015, Counter({0: 755, 1: 194, 2: 39, 3: 27})),
}


def check_conversion(wad, map_name):
    mapped = doom_map.load_map(wad, map_name)
    control = doom_map.load_map(wad, map_name, apply_automap=False)
    count, kinds = EXPECTED[map_name]
    assert len(mapped.automap_lines) == count
    assert Counter(line["kind"] for line in mapped.automap_lines) == kinds
    assert control.automap_lines == []
    assert all(value == 0xFFFF for value in control.linedef_automap_indices)

    for field in fields(mapped):
        if field.name in {"automap_lines", "linedef_automap_indices"}:
            continue
        assert getattr(mapped, field.name) == getattr(control, field.name), field.name

    for line_id, linedef in enumerate(mapped.linedefs):
        index = mapped.linedef_automap_indices[line_id]
        if linedef["flags"] & doom_map.LINE_FLAG_DONTDRAW:
            assert index == 0xFFFF
            continue
        if index == 0xFFFF:
            continue
        line = mapped.automap_lines[index]
        assert (line["v1"], line["v2"]) == (linedef["v1"], linedef["v2"])
        assert line["front_sector"] == 0xFF or line["front_sector"] < len(mapped.sectors)
        assert line["back_sector"] == 0xFF or line["back_sector"] < len(mapped.sectors)
        if linedef["flags"] & doom_map.LINE_FLAG_SECRET:
            assert line["kind"] == doom_map.AUTOMAP_LINE_SOLID

    for seg in mapped.out_segs:
        expected = mapped.linedef_automap_indices[seg["source_linedef"]]
        assert 0 <= expected <= 0xFFFF
    return mapped


def outcode(x, y, width=160, height=120):
    return ((1 if x < 0 else 2 if x >= width else 0) |
            (4 if y < 0 else 8 if y >= height else 0))


def clip(x0, y0, x1, y1, width=160, height=120):
    c0, c1 = outcode(x0, y0), outcode(x1, y1)
    while True:
        if not (c0 | c1):
            return x0, y0, x1, y1
        if c0 & c1:
            return None
        code = c0 or c1
        if code & 4:
            if y1 == y0: return None
            y = 0; x = x0 + (x1-x0)*(y-y0)//(y1-y0)
        elif code & 8:
            if y1 == y0: return None
            y = height-1; x = x0 + (x1-x0)*(y-y0)//(y1-y0)
        elif code & 2:
            if x1 == x0: return None
            x = width-1; y = y0 + (y1-y0)*(x-x0)//(x1-x0)
        else:
            if x1 == x0: return None
            x = 0; y = y0 + (y1-y0)*(x-x0)//(x1-x0)
        if code == c0: x0, y0, c0 = x, y, outcode(x, y)
        else: x1, y1, c1 = x, y, outcode(x, y)


def raster(line):
    if line is None: return []
    x, y, ex, ey = line
    dx, sx = abs(ex-x), 1 if x < ex else -1
    dy, sy = -abs(ey-y), 1 if y < ey else -1
    error, pixels = dx+dy, []
    while True:
        pixels.append((x, y))
        if x == ex and y == ey: return pixels
        twice = error * 2
        if twice >= dy: error += dy; x += sx
        if twice <= dx: error += dx; y += sy


def check_raster_bounds():
    cases = [
        (10, 10, 80, 10), (20, 5, 20, 100), (0, 0, 159, 119),
        (-30, 60, 50, 60), (80, -40, 80, 30), (-10, -10, 200, 150),
        (-20, -20, -1, 100), (170, 0, 200, 119),
    ]
    guard = [0xA5] * (160 * 120 + 2)
    for case in cases:
        for x, y in raster(clip(*case)):
            assert 0 <= x < 160 and 0 <= y < 120
            guard[1 + y * 160 + x] = 1
    assert guard[0] == guard[-1] == 0xA5


def main():
    wad = WadFile(str(ROOT / "DOOM1.WAD"))
    maps = [check_conversion(wad, name) for name in ("E1M1", "E1M2")]
    check_raster_bounds()

    header = (ROOT / "src/bsp/bsp_map.h").read_text()
    runtime = (ROOT / "src/bsp/bsp_map.c").read_text()
    render = (ROOT / "src/bsp/bsp_render_columns.c").read_text()
    limits = (ROOT / "src/bsp/generated_map_limits.h").read_text()
    assert "sizeof(BspAutomapLine) == 8" in header
    assert "sizeof(BspSeg) == 16" in header
    assert "sizeof(RayDoorOverlay) == 10" in (ROOT / "src/raycast.h").read_text()
    assert "MEGALDOOM_MAP_MAX_AUTOMAP_LINES 1015" in limits
    assert "bsp_automap_mark_seg(seg_index);" in render
    assert "bsp_automap_mark_sector(sector);" in (ROOT / "src/bsp/bsp_render.c").read_text()
    assert "BSP_AUTOMAP_LINE_FLOOR" in runtime and "BSP_AUTOMAP_LINE_CEILING" in runtime
    assert [len(m.automap_lines) for m in maps] == [451, 1015]
    route = (ROOT / "tools/routes/automap-e1m1.txt").read_text().splitlines()
    events = [tuple(part for part in line.split()) for line in route if line.strip()]
    assert ("2100", "400") in events  # six-button Z opens
    assert ("2280", "100") in events  # X disables follow before pan
    assert ("2440", "10") in events and ("2480", "20") in events
    assert ("2520", "40") in events and ("2560", "200") in events
    assert events[-1] == ("2710", "0")
    print("ok    automap: WAD lines, negative control, discovery and clipped raster")


if __name__ == "__main__":
    main()
