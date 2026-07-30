#!/usr/bin/env python3
"""Deterministic quality contracts for the 64x64, stride-2 wall pipeline."""
import importlib.util
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
EXTRACTOR_PATH = ROOT / "tools" / "wad-map-extract.py"
ASSETS_PATH = ROOT / "src" / "generated_assets.h"
MAP_PATH = ROOT / "src" / "generated_e1m1_map.c"
LEGACY_WORLD_PALETTE = [
    (0x00, 0x00, 0x00), (0x00, 0x00, 0x91), (0x48, 0x00, 0x00),
    (0x24, 0x24, 0x00), (0x24, 0x24, 0x24), (0x48, 0x48, 0x24),
    (0x48, 0x48, 0x48), (0x6D, 0x48, 0x24), (0xB6, 0x24, 0x24),
    (0x6D, 0x48, 0x48), (0x6D, 0x6D, 0x48), (0x6D, 0x6D, 0x6D),
    (0xB6, 0x6D, 0x48), (0xB6, 0xB6, 0xB6), (0xDA, 0x24, 0x24),
    (0xDA, 0xB6, 0x48),
]


def load_extractor():
    spec = importlib.util.spec_from_file_location("wad_map_extract_quality", EXTRACTOR_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def generated_palette(source):
    body = re.search(
        r"FREEDOOM_WORLD_PALETTE\[16\]\s*=\s*\{(.*?)\};", source, re.S)
    assert body, "world palette declaration"
    values = re.findall(r"0x([0-9A-Fa-f]{6})", body.group(1))
    assert len(values) == 16, len(values)
    return [tuple(int(value[i:i + 2], 16) for i in (0, 2, 4)) for value in values]


def generated_define(source, name):
    match = re.search(rf"#define\s+{name}\s+(\d+)\b", source)
    assert match, name
    return int(match.group(1))


def assert_no_spurious_green(extractor, palette, texture_name):
    path = Path(extractor.texture_path(texture_name))
    with Image.open(path) as image:
        source = image.convert("RGB").resize(
            (extractor.WALL_TEX_DIM, extractor.WALL_TEX_DIM), Image.Resampling.BOX)
        source_pixels = list(source.get_flattened_data())
    converted = extractor.convert_texture(path, palette)
    mapped = [palette[index] for row in converted for index in row]
    eligible = 0
    spurious = 0
    for original, result in zip(source_pixels, mapped):
        # Legitimately green source pixels remain allowed. Neutral and brown
        # source pixels must not acquire a visibly green-dominant palette entry.
        if original[1] <= max(original[0], original[2]) + 18:
            eligible += 1
            if result[1] > max(result[0], result[2]) + 36:
                spurious += 1
    assert eligible
    assert spurious / eligible <= 0.01, (texture_name, spurious, eligible)


def spatial_palette_error(extractor, palette, texture_name):
    path = Path(extractor.texture_path(texture_name))
    with Image.open(path) as image:
        source = image.convert("RGB").resize(
            (extractor.WALL_TEX_DIM, extractor.WALL_TEX_DIM), Image.Resampling.BOX)
    converted = extractor.convert_texture(path, palette)
    perceptual = 0.0
    rgb_baseline = 0.0
    for by in range(0, extractor.WALL_TEX_DIM, 4):
        for bx in range(0, extractor.WALL_TEX_DIM, 4):
            pixels = [source.getpixel((x, y))
                      for y in range(by, by + 4) for x in range(bx, bx + 4)]
            target_rgb = tuple(sum(pixel[channel] for pixel in pixels) // 16
                               for channel in range(3))
            target_lab = tuple(sum(extractor.world_palette.oklab(pixel)[channel]
                                   for pixel in pixels) / 16 for channel in range(3))
            mapped = [palette[converted[y][x]]
                      for y in range(by, by + 4) for x in range(bx, bx + 4)]
            mapped_rgb = tuple(sum(pixel[channel] for pixel in mapped) // 16
                               for channel in range(3))
            mapped_lab = tuple(sum(extractor.world_palette.oklab(pixel)[channel]
                                   for pixel in mapped) / 16 for channel in range(3))
            nearest_rgb = [min(LEGACY_WORLD_PALETTE, key=lambda candidate:
                           sum((pixel[channel] - candidate[channel]) ** 2
                           for channel in range(3))) for pixel in pixels]
            baseline_rgb = tuple(sum(pixel[channel] for pixel in nearest_rgb) // 16
                                 for channel in range(3))
            baseline_lab = tuple(sum(extractor.world_palette.oklab(pixel)[channel]
                                     for pixel in nearest_rgb) / 16 for channel in range(3))
            perceptual += (1.25 * (target_lab[0] - mapped_lab[0]) ** 2 +
                           (target_lab[1] - mapped_lab[1]) ** 2 +
                           (target_lab[2] - mapped_lab[2]) ** 2)
            rgb_baseline += (1.25 * (target_lab[0] - baseline_lab[0]) ** 2 +
                             (target_lab[1] - baseline_lab[1]) ** 2 +
                             (target_lab[2] - baseline_lab[2]) ** 2)
            source_is_olive = (abs(target_rgb[0] - target_rgb[1]) <= 18 and
                               target_rgb[1] >= target_rgb[2] + 18)
            if not source_is_olive:
                if abs(mapped_rgb[0] - mapped_rgb[1]) <= 18 and \
                        mapped_rgb[1] >= mapped_rgb[2] + 18:
                    perceptual += 0.01
                if abs(baseline_rgb[0] - baseline_rgb[1]) <= 18 and \
                        baseline_rgb[1] >= baseline_rgb[2] + 18:
                    rgb_baseline += 0.01
    return perceptual, rgb_baseline


def main():
    extractor = load_extractor()
    raycast = (ROOT / "src" / "raycast.h").read_text()
    # renderer_scene.c was split by SRP into several files; the pack-stage
    # code these checks look for now lives across that set.
    renderer = "\n".join((ROOT / "src" / n).read_text() for n in (
        "renderer_scene.c", "renderer_pack.c", "renderer_doors.c",
        "renderer_billboard_draw.c", "renderer_frame_overlay.c",
        "renderer_upload.c", "renderer_sparse.c",
        "renderer_flats.c",
    ))
    assets = ASSETS_PATH.read_text()

    assert extractor.WALL_TEX_DIM == 64
    assert re.search(r"#define WALL_TEX_DIM 64\b", raycast)
    assert re.search(r"#define RAY_COL_STRIDE 2\b", raycast)
    assert "FREEDOOM_WALL_PACKED_PAIRS" in assets
    assert "FREEDOOM_WALL_PACKED_PAIRS[" in renderer
    assert "DOOR_FRAME_TEXELS (WALL_TEX_DIM / 16)" in renderer
    assert "DOOR_SAFETY_TEXELS (WALL_TEX_DIM / 8)" in renderer

    palette = generated_palette(assets)
    vdp_channels = {extractor.md_color((value, value, value))[0]
                    for value in range(256)}
    assert palette[0] == (0, 0, 0)
    assert all(channel in vdp_channels for color in palette for channel in color)
    assert len(set(palette)) == 16
    assert sum(extractor.world_palette.is_neutral(color) for color in palette) >= 4
    assert sum(extractor.world_palette.is_warm(color) for color in palette) >= 4
    assert sum(extractor.world_palette.is_green(color) for color in palette) == 1
    assert sum(extractor.world_palette.is_blue(color) for color in palette) >= 1
    assert not any(extractor.world_palette.is_olive(color)
                   for color in palette[1:14])
    assert "BSP_FLOOR_COLOR" not in (ROOT / "src" / "bsp_render.c").read_text()
    shade_lut = extractor.build_shade_lut(palette)
    for index, color in enumerate(palette):
        for level in shade_lut:
            shaded = palette[level[index]]
            if color[0] == color[1] == color[2]:
                assert shaded[0] == shaded[1] == shaded[2], (color, shaded)
            if color[1] <= max(color[0], color[2]):
                assert shaded[1] <= max(shaded[0], shaded[2]), (color, shaded)
    for texture_name in ("BROWN1", "GRAY7", "METAL1", "STONE2", "STARTAN3"):
        assert_no_spurious_green(extractor, palette, texture_name)
        perceptual, baseline = spatial_palette_error(extractor, palette, texture_name)
        # Warm walls must beat the olive-heavy legacy conversion outright.
        # Neutral materials may pay a bounded luminance error to remain strictly
        # achromatic instead of borrowing legacy yellow/olive shades.
        limit = baseline * (2.0 if texture_name in ("GRAY7", "METAL1", "STONE2") else 1.0)
        assert perceptual <= limit, (texture_name, perceptual, baseline)

    sector_visuals = re.search(
        r"FREEDOOM_SECTOR_VISUALS\[.*?\]\[6\]\s*=\s*\{(.*?)\};", assets, re.S)
    assert sector_visuals
    rows = [[int(value) for value in re.findall(r"\d+", row)]
            for row in re.findall(r"\{([^{}]+)\}", sector_visuals.group(1))]
    assert len(rows) == generated_define(assets, "FREEDOOM_SECTOR_VISUAL_COUNT")
    assert all(len(row) == 6 and all(0 <= value < 16 for value in row[:2] + row[3:5])
               and 0 <= row[2] <= 16 and 0 <= row[5] <= 16 for row in rows)
    fixed_floor = generated_define(assets, "MEGALDOOM_WORLD_COLOR_FLOOR")
    assert all(row[3:] == [fixed_floor, fixed_floor, 0] for row in rows), \
        "floor must remain one neutral color across every sector"
    floor_rgb = palette[fixed_floor]
    assert max(floor_rgb) - min(floor_rgb) <= 12, floor_rgb

    # Regeneration to alternate outputs must be byte-identical to the checked-in
    # generated contracts, including palette order and packed pair data.
    with tempfile.TemporaryDirectory(prefix="megaldoom-wall-quality-") as temp:
        temp_root = Path(temp)
        generated_map = temp_root / "generated_e1m1_map.c"
        generated_assets = temp_root / "generated_assets.h"
        subprocess.run([
            sys.executable, str(EXTRACTOR_PATH),
            "--wad", str(ROOT / "DOOM1.WAD"), "--map", "E1M1",
            "--out", str(generated_map), "--assets-out", str(generated_assets),
        ], cwd=ROOT, check=True, stdout=subprocess.DEVNULL)
        assert generated_map.read_bytes() == MAP_PATH.read_bytes()
        assert generated_assets.read_bytes() == ASSETS_PATH.read_bytes()

    print("ok    walls: 64x64, stride 2/80 columns, Doom-faithful deterministic PAL3")


if __name__ == "__main__":
    main()
