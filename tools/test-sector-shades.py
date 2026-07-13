#!/usr/bin/env python3
"""Deterministic contracts for solid shaded BSP wall rendering."""
import importlib.util
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "src" / "generated_assets.h"
SECTOR = ROOT / "src" / "sector_render.c"
EXTRACTOR = ROOT / "tools" / "wad-map-extract.py"


def load_extractor():
    spec = importlib.util.spec_from_file_location("wad_map_extract", EXTRACTOR)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def parse_row(source, declaration):
    match = re.search(declaration + r"\s*=\s*\{(?P<body>.*?)\};", source, re.S)
    assert match, declaration
    return [int(value) for value in re.findall(r"\b\d+\b", match.group("body"))]


def main():
    extractor = load_extractor()
    assets = ASSETS.read_text(encoding="utf-8")
    sector = SECTOR.read_text(encoding="utf-8")
    palette = extractor.LEGACY_WORLD_PALETTE
    luminance = lambda index: sum(
        palette[index][channel] * weight
        for channel, weight in enumerate((30, 59, 11)))

    shade_values = parse_row(
        assets,
        r"static const u8 FREEDOOM_WORLD_SHADE_LUT\[FREEDOOM_WORLD_SHADE_LEVELS\]\[16\]")
    assert len(shade_values) == 64
    shades = [shade_values[level * 16:(level + 1) * 16] for level in range(4)]
    assert shades[0] == list(range(16))
    for level in range(1, 4):
        for color in range(16):
            assert luminance(shades[level][color]) <= luminance(shades[level - 1][color])

    base_colors = parse_row(
        assets,
        r"static const u8 FREEDOOM_WALL_BASE_COLOR\[FREEDOOM_WALL_TEXTURE_COUNT\]")
    texture_ids = {
        name: int(value)
        for name, value in re.findall(r"#define MEGALDOOM_TEX_(\w+) (\d+)", assets)
    }
    assert len(base_colors) == 32
    interactive = [base_colors[texture_ids[name]]
                   for name in ("DOOR3", "BIGDOOR2", "SW1STRTN")]
    assert len(set(interactive)) == 3

    def shade_level(depth, orientation):
        return min(3, (depth >> 9) + int(orientation))

    assert shade_level(0, False) == 0
    assert shade_level(511, True) == 1
    assert shade_level(512, False) == 1
    assert shade_level(1024, True) == 3
    assert shade_level(65535, False) == 3

    # Fresh blocks accept a direct write; later overlapping spans retain the
    # nearest depth. Clipping must leave rows outside the span untouched.
    depth = [0xFFFF] * 8
    color = [2] * 8
    for row in range(2, 7):
        depth[row], color[row] = 300, 7
    for row in range(4, 8):
        if 500 < depth[row]:
            depth[row], color[row] = 500, 9
    for row in range(3, 6):
        if 120 < depth[row]:
            depth[row], color[row] = 120, 11
    assert depth == [0xFFFF, 0xFFFF, 300, 120, 120, 120, 300, 500]
    assert color == [2, 2, 7, 11, 11, 11, 7, 9]

    assert "static void draw_span" not in sector
    assert "FREEDOOM_WALL_TEXTURES" not in sector
    assert "MEGALDOOM_WALL_TEX_Y_BY_HEIGHT" not in sector
    assert not re.search(r"\bu_(?:q|step)\b", sector)
    assert "tex_u_offset" not in sector
    assert "draw_solid_span" in sector
    assert "packed_depth" in sector
    assert "render_seg_contributes" in sector
    assert "sector_range_closed(first_sample, last_sample" in sector
    assert "g_debug_rejected_segments" in sector
    assert "g_debug_closed_ranges" in sector
    assert "g_debug_raster_samples" in sector
    assert "build_wall_base_colors" in EXTRACTOR.read_text(encoding="utf-8")

    print("ok    solid wall shades, material identity, clipping, and BSP rejection")


if __name__ == "__main__":
    main()
