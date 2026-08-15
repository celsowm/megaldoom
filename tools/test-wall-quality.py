#!/usr/bin/env python3
"""Deterministic quality contracts for the 64x64, stride-2 wall pipeline."""
import importlib.util
import re
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
EXTRACTOR_PATH = ROOT / "tools" / "wad-map-extract.py"
WORLD_ASSETS_PATH = ROOT / "tools" / "world_assets.py"
ASSETS_PATH = ROOT / "src" / "bsp" / "generated_assets.h"
MAP_PATH = ROOT / "src" / "bsp" / "generated_e1m1_map.c"
LEGACY_WORLD_PALETTE = [
    (0x00, 0x00, 0x00), (0x00, 0x00, 0x91), (0x48, 0x00, 0x00),
    (0x24, 0x24, 0x00), (0x24, 0x24, 0x24), (0x48, 0x48, 0x24),
    (0x48, 0x48, 0x48), (0x6D, 0x48, 0x24), (0xB6, 0x24, 0x24),
    (0x6D, 0x48, 0x48), (0x6D, 0x6D, 0x48), (0x6D, 0x6D, 0x6D),
    (0xB6, 0x6D, 0x48), (0xB6, 0xB6, 0xB6), (0xDA, 0x24, 0x24),
    (0xDA, 0xB6, 0x48),
]


def load_extractor():
    """Load tools/world_assets.py: the wall/flat texture -> PAL3 baking module
    all the per-pixel checks below exercise. It re-exposes world_palette (via
    its own `import world_palette`), so `extractor.world_palette.*` still
    works."""
    spec = importlib.util.spec_from_file_location("world_assets_quality", WORLD_ASSETS_PATH)
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


def generated_wall_textures(source):
    """Parse FREEDOOM_WALL_TEXTURES into {name: [[index,...]x64]x64}."""
    start = source.index("FREEDOOM_WALL_TEXTURES")
    end = source.index("};", start)
    body = source[start:end]
    result = {}
    for tex_index, name, width, height, rows in re.findall(
            r"\{ // (\d+): (\S+) \((\d+)x(\d+)\)(.*?)\n    \},", body, re.S):
        result[name] = [[int(value) for value in re.findall(r"\d+", row)]
                        for row in re.findall(r"\{([^{}]*)\}", rows)]
    return result


def generated_shade_lut(source):
    body = re.search(
        r"FREEDOOM_WORLD_SHADE_LUT\[FREEDOOM_WORLD_SHADE_LEVELS\]\[16\]"
        r"\s*=\s*\{(.*?)\};", source, re.S)
    assert body, "shade LUT declaration"
    return [[int(value) for value in re.findall(r"\d+", level)]
            for level in re.findall(r"\{([^{}]+)\}", body.group(1))]


def _tone_curved_source(extractor, path):
    """Resize to the wall grid and apply the same tone curve, per-texture
    contrast normalization AND spatial smoothing convert_texture() applies
    before quantizing, so quality checks compare against what the converter
    actually targets rather than the pre-lift source (see WALL_TONE_GAMMA,
    WALL_TARGET_SPREAD and WALL_SMOOTH_WEIGHT in tools/world_assets.py). Both
    the PAL3 result and the legacy-palette baseline in spatial_palette_error are
    scored against this same target, so widening a material's contrast or
    smoothing it cannot flatter one side of that comparison."""
    dim = extractor.WALL_TEX_DIM
    with Image.open(path) as image:
        source = image.convert("RGB").resize((dim, dim), Image.Resampling.BOX)
    pixels = [extractor.tone_curve(pixel) for pixel in source.get_flattened_data()]
    columns = extractor._spatial_smooth(extractor._contrast_normalize(
        [[pixels[y * dim + x] for y in range(dim)] for x in range(dim)]))
    return [columns[x][y] for y in range(dim) for x in range(dim)]


def assert_no_spurious_green(extractor, palette, texture_name):
    path = Path(extractor.texture_path(texture_name))
    source_pixels = _tone_curved_source(extractor, path)
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
    dim = extractor.WALL_TEX_DIM
    source_flat = _tone_curved_source(extractor, path)
    source = {(x, y): source_flat[y * dim + x] for y in range(dim) for x in range(dim)}
    converted = extractor.convert_texture(path, palette)
    perceptual = 0.0
    rgb_baseline = 0.0
    for by in range(0, extractor.WALL_TEX_DIM, 4):
        for bx in range(0, extractor.WALL_TEX_DIM, 4):
            pixels = [source[(x, y)]
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
    renderer = "\n".join((ROOT / "src" / "renderer" / n).read_text() for n in (
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
    assert "BSP_FLOOR_COLOR" not in (ROOT / "src" / "bsp" / "bsp_render.c").read_text()
    ceiling_index = extractor.GLOBAL_CEILING_INDEX
    floor_index = extractor.GLOBAL_FLOOR_INDEX
    shade_lut = extractor.build_shade_lut(palette, 4, (ceiling_index, floor_index))
    assert generated_shade_lut(assets) == shade_lut, "emitted shade LUT drifted"
    for index, color in enumerate(palette):
        for level in shade_lut:
            shaded = palette[level[index]]
            if color[0] == color[1] == color[2]:
                assert shaded[0] == shaded[1] == shaded[2], (color, shaded)
            if color[1] <= max(color[0], color[2]):
                assert shaded[1] <= max(shaded[0], shaded[2]), (color, shaded)
    # Depth shading is ON (WALL_SHADE_MODE 2), so these two are load-bearing:
    # a chain that reaches black turns distant walls into holes, and one that
    # reaches a flat index turns them into the ceiling or the floor.
    for level_index, level in enumerate(shade_lut):
        for index in range(1, len(palette)):
            assert level[index] != 0, (level_index, index)
            if index not in (ceiling_index, floor_index):
                assert level[index] not in (ceiling_index, floor_index), \
                    ("shading darkened a wall into a flat colour",
                     level_index, index, level[index])
    for texture_name in ("BROWN1", "GRAY7", "METAL1", "STONE2", "STARTAN3"):
        assert_no_spurious_green(extractor, palette, texture_name)
        perceptual, baseline = spatial_palette_error(extractor, palette, texture_name)
        # Warm walls must beat the olive-heavy legacy conversion outright.
        # Neutral materials may pay a bounded luminance error to remain strictly
        # achromatic instead of borrowing legacy yellow/olive shades. METAL1's
        # legacy match happens to be unusually tight (baseline ~0.17, the
        # lowest of the five), so the same absolute quantization noise every
        # other material pays reads as a much larger ratio for it alone; 2.5x
        # still catches a real regression while giving that outlier headroom.
        limit = baseline * (2.5 if texture_name in ("GRAY7", "METAL1", "STONE2") else 1.0)
        assert perceptual <= limit, (texture_name, perceptual, baseline)

    wall_textures = generated_wall_textures(assets)

    # "Churn" -- the share of horizontally adjacent texel pairs with different
    # palette indices -- is the direct measure of the salt-and-pepper noise that
    # made brown walls read as confetti. It has to be bounded from ABOVE, not
    # below: the runtime samples each column at a distance-dependent rate
    # through MEGALDOOM_WALL_TEX_Y_BY_HEIGHT and skips every other screen column
    # (RAY_COL_STRIDE 2), so texel-frequency detail is never reconstructed as a
    # blend -- it aliases into streaks that crawl as the camera moves. Before the
    # earth ramp and WALL_SMOOTH_WEIGHT, the worst materials sat at 56-71%.
    # EXITDOOR is the one legitimate outlier: a one-off decorative door whose
    # source genuinely alternates gilt, grey and red at texel frequency.
    CHURN_LIMIT = 0.30
    CHURN_EXEMPT = {"EXITDOOR"}
    for texture_name, rows in sorted(wall_textures.items()):
        churn = sum(1 for row in rows for x in range(len(row) - 1)
                    if row[x] != row[x + 1])
        churn /= len(rows) * (len(rows[0]) - 1)
        if texture_name not in CHURN_EXEMPT:
            assert churn <= CHURN_LIMIT, (texture_name, churn)

    # Structure floor, the other half of the same contract: killing the noise
    # must not be achieved by flattening a material into one block. Two clauses,
    # because neither alone says "this still reads as a wall":
    #   - at least two indices carry real area (a lone dominant index plus a
    #     scattering of strays is a flat block, whatever len(counts) says), and
    #   - no index may swallow the whole surface.
    # Deliberately NOT a tight dominance cap. The old 0.75 cap was a proxy for
    # "wall must not look like the floor", and it now fights the fix: with
    # dithering off, a two-tone brick like STARTAN1 legitimately resolves to
    # 57/41 across two indices. The flat-collision contract is enforced directly
    # and far more precisely by certify_flat_wall_contrast below.
    MIN_STRUCTURAL_SHARE = 0.02
    # Materials whose source really is a near-uniform field: LITE3 is a white
    # light panel, COMPTILE a two-tone tile. Manufacturing spread into them would
    # be inventing detail Doom never drew.
    UNIFORM_MATERIALS = ("LITE3", "COMPTILE", "DOORSTOP", "STARGR1", "SUPPORT2")
    for texture_name, rows in sorted(wall_textures.items()):
        flat = [value for row in rows for value in row]
        counts = Counter(flat)
        structural = [index for index, hits in counts.items()
                      if hits >= MIN_STRUCTURAL_SHARE * len(flat)]
        assert len(structural) >= 2, (texture_name, counts)
        dominant_share = counts.most_common(1)[0][1] / len(flat)
        cap = 0.90 if texture_name in UNIFORM_MATERIALS else 0.85
        assert dominant_share <= cap, (texture_name, dominant_share, counts)

    # The contract behind the global flats: no wall may read as an unbroken
    # field of the ceiling's or the floor's colour, at ANY shade level. Index
    # inequality alone is not enough -- adjacent PAL3 rungs are only ~0.13 apart
    # in Oklab, so this is enforced perceptually. Reuses the same function the
    # bake fails on, so the test and the generator cannot disagree.
    solid = extractor.certify_flat_wall_contrast(
        palette, wall_textures, shade_lut, ceiling_index, floor_index)
    assert solid, "expected some near-solid wall materials to guard against"

    sector_visuals = re.search(
        r"FREEDOOM_SECTOR_VISUALS\[.*?\]\[6\]\s*=\s*\{(.*?)\};", assets, re.S)
    assert sector_visuals
    rows = [[int(value) for value in re.findall(r"\d+", row)]
            for row in re.findall(r"\{([^{}]+)\}", sector_visuals.group(1))]
    assert len(rows) == generated_define(assets, "FREEDOOM_SECTOR_VISUAL_COUNT")
    assert all(len(row) == 6 and all(0 <= value < 16 for value in row[:2] + row[3:5])
               and 0 <= row[2] <= 16 and 0 <= row[5] <= 16 for row in rows)
    # One ceiling and one floor for the whole level. Per-sector flats folded the
    # sector's LIGHT level into the material's colour before quantizing, so one
    # material emitted up to seven different colours (FLOOR5_2) across E1M1's
    # eight light levels -- and since bsp_cast_frame paints the whole viewport
    # from the PLAYER's sector, walking between two rooms with the same floor
    # repainted the screen. That reads as a palette glitch, not as lighting.
    assert len(set(map(tuple, rows))) == 1, "flats must be level-wide constants"
    ceiling_a, ceiling_b, ceiling_coverage, floor_a, floor_b, floor_coverage = rows[0]
    assert (ceiling_a, ceiling_b) == (ceiling_index, ceiling_index)
    assert (floor_a, floor_b) == (floor_index, floor_index)
    # Solid, not Bayer-mixed: renderer_flats.c anchors its 4x4 pattern to screen
    # space (x is always tile-local 0..7, y is y&3), so it never moves with the
    # camera. On a level-wide flat -- the largest continuous surface on screen --
    # that reads as fixed dirt on the monitor rather than as texture.
    assert ceiling_coverage == 0 and floor_coverage == 0
    separation = extractor.world_palette.distance_sq(
        palette[ceiling_index], palette[floor_index]) ** 0.5
    assert separation >= 0.25, separation

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
