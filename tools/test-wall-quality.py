#!/usr/bin/env python3
"""Deterministic quality contracts for the 64x64, stride-2 wall pipeline."""
import importlib.util
import math
import re
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path

from PIL import Image

import raycast_constants
import wall_bake_preview
from e1m1_expected import E1M1_SEG_COUNT, E1M1_PREVIEW_PACKED_PAIR_BYTES

ROOT = Path(__file__).resolve().parents[1]
EXTRACTOR_PATH = ROOT / "tools" / "wad-map-extract.py"
WORLD_ASSETS_PATH = ROOT / "tools" / "world_assets.py"
ASSETS_PATH = ROOT / "src" / "bsp" / "generated_assets.h"
MAP_PATH = ROOT / "src" / "bsp" / "generated_e1m1_map.c"
MAP2_PATH = ROOT / "src" / "bsp" / "generated_e1m2_map.c"
MAP3_PATH = ROOT / "src" / "bsp" / "generated_e1m3_map.c"
LIMITS_PATH = ROOT / "src" / "bsp" / "generated_map_limits.h"
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


def worst_block_churn(rows, active_height, block):
    """Highest churn found in any block x block window, either axis.

    The texture-wide mean answers "is this material noisy overall"; this
    answers "is any part of it noise", which is the question a mostly-flat
    material with a dense inset panel defeats. Both axes matter and are taken
    separately rather than pooled: the runtime aliases them through different
    mechanisms -- RAY_COL_STRIDE 2 skips screen columns, while rows resample
    through MEGALDOOM_WALL_TEX_Y_BY_HEIGHT -- so a material may be quiet on one
    and confetti on the other.
    """
    width = len(rows[0])
    worst = 0.0
    for top in range(0, max(1, active_height - 1), block):
        for left in range(0, max(1, width - 1), block):
            bottom = min(top + block, active_height)
            right = min(left + block, width)
            horizontal = [1 if rows[y][x] != rows[y][x + 1] else 0
                          for y in range(top, bottom)
                          for x in range(left, right - 1)]
            vertical = [1 if rows[y][x] != rows[y + 1][x] else 0
                        for y in range(top, bottom - 1)
                        for x in range(left, right)]
            for samples in (horizontal, vertical):
                if samples:
                    worst = max(worst, sum(samples) / len(samples))
    return worst


def campaign_wall_area_shares():
    """Share of shipped wall area each texture paints, across all three maps.

    Area-weighted by seg length, not counted per texture: the memory of this
    codebase is that per-texture averages hide which materials actually fill
    the screen. A BspSeg initialiser is
    {v1, v2, nx, ny, tex_u_offset, tex_v_offset, texture_id, type, ...}, so the
    texture id is the seventh field and the endpoints index bsp_vertices.
    """
    atlas = {}
    assets = ASSETS_PATH.read_text(errors="ignore")
    for index, name, _width, _height in re.findall(
            r"// (\d+): (\S+) \((\d+)x(\d+)\)", assets):
        atlas[int(index)] = name
    totals = Counter()
    for path in (MAP_PATH, MAP2_PATH, MAP3_PATH):
        source = path.read_text(errors="ignore")
        vertex_body = re.search(r"bsp_vertices\[\d+\]\s*=\s*\{(.*?)\n\};",
                                source, re.S)
        seg_body = re.search(r"bsp_segs\[\d+\]\s*=\s*\{(.*?)\n\};", source, re.S)
        assert vertex_body and seg_body, path.name
        vertices = [(int(x), int(y)) for x, y in re.findall(
            r"\{\s*(-?\d+)\s*,\s*(-?\d+)\s*\}", vertex_body.group(1))]
        for row in re.findall(r"\{([^{}]*)\}", seg_body.group(1)):
            fields = [int(value) for value in re.findall(r"-?\d+", row)]
            if len(fields) < 8:
                continue
            (x1, y1), (x2, y2) = vertices[fields[0]], vertices[fields[1]]
            name = atlas.get(fields[6])
            if name:
                totals[name] += math.hypot(x2 - x1, y2 - y1)
    span = sum(totals.values())
    assert span > 0
    return {name: length / span for name, length in totals.items()}


def generated_wall_textures(source):
    """Parse FREEDOOM_WALL_TEXTURES into {name: [[index,...]x128]x128}."""
    start = source.index("FREEDOOM_WALL_TEXTURES")
    end = source.index("};", start)
    body = source[start:end]
    result = {}
    for tex_index, name, width, height, rows in re.findall(
            r"\{ // (\d+): (\S+) \((\d+)x(\d+)\)(.*?)\n    \},", body, re.S):
        result[name] = [[int(value) for value in re.findall(r"\d+", row)]
                        for row in re.findall(r"\{([^{}]*)\}", rows)]
    return result


def _tone_curved_source(extractor, path):
    """Resize to the wall grid and apply the same tone curve, per-texture
    contrast normalization AND spatial smoothing convert_texture() applies
    before quantizing, so quality checks compare against what the converter
    actually targets rather than the pre-lift source (see WALL_TONE_GAMMA,
    WALL_TARGET_SPREAD and WALL_SMOOTH_WEIGHT in tools/world_assets.py). Both
    the PAL3 result and the legacy-palette baseline in spatial_palette_error are
    scored against this same target, so widening a material's contrast or
    smoothing it cannot flatter one side of that comparison."""
    width = extractor.WALL_TEX_DISPLAY_WIDTH
    with Image.open(path) as image:
        height = extractor.sampled_texture_dimensions(
            path.stem.upper(), image.width, image.height)[1]
        source = image.convert("RGB").resize((width, height), Image.Resampling.BOX)
    pixels = [extractor.tone_curve(pixel) for pixel in source.get_flattened_data()]
    columns = extractor._spatial_smooth(extractor._contrast_normalize(
        [[pixels[y * width + x] for y in range(height)] for x in range(width)]))
    return [columns[x][y] for y in range(height) for x in range(width)]


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
    width = extractor.WALL_TEX_DISPLAY_WIDTH
    height = extractor.WALL_TEX_HEIGHT
    source_flat = _tone_curved_source(extractor, path)
    source = {(x, y): source_flat[y * width + x]
              for y in range(height) for x in range(width)}
    converted = extractor.convert_texture(path, palette)
    perceptual = 0.0
    rgb_baseline = 0.0
    for by in range(0, height, 4):
        for bx in range(0, width, 4):
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

    # The shipped quality profile, pinned in exactly one place. Changing any of
    # these is a deliberate visual decision (stride in particular: it IS the
    # wall's horizontal resolution, and stride 4 shipped once and was reverted),
    # so it must be edited here as well as in raycast.h.
    assert raycast_constants.wall_tex_dims() == (64, 128)
    assert raycast_constants.col_stride() == 2
    # ...and the bake reads that same header rather than carrying its own copy.
    assert (extractor.WALL_TEX_WIDTH, extractor.WALL_TEX_HEIGHT) == \
        raycast_constants.wall_tex_dims()
    assert "FREEDOOM_WALL_PACKED_PAIRS" in assets
    assert "FREEDOOM_WALL_PACKED_PAIRS[" in renderer
    # The door's frame/safety-stripe silhouette used to be re-derived per pixel
    # by style_wall_texel() in renderer_doors.c; it is now baked into
    # FREEDOOM_WALL_DOOR_PACKED_PAIRS, which the overlay reads through the same
    # packed_wall_column() the wall post uses. So the rule is checked where it
    # now lives -- in the bake -- and the C side is checked for reading the
    # baked table rather than for carrying its own copy of the rule.
    bake = (ROOT / "tools" / "world_assets.py").read_text()
    assert "border = WALL_TEX_WIDTH // 16" in bake
    assert 'safety = texture_meta[name]["height"] // 8' in bake
    assert "FREEDOOM_WALL_DOOR_PACKED_PAIRS" in assets
    assert "packed_wall_column(&descriptor)" in renderer

    palette = generated_palette(assets)
    assert tuple(palette) == extractor.FROZEN_WORLD_PALETTE
    vdp_channels = {extractor.md_color((value, value, value))[0]
                    for value in range(256)}
    assert palette[0] == (0, 0, 0)
    assert all(channel in vdp_channels for color in palette for channel in color)
    assert len(set(palette)) == 16
    assert sum(extractor.world_palette.is_neutral(color) for color in palette) >= 4
    assert sum(extractor.world_palette.is_warm(color) for color in palette) >= 4
    assert sum(extractor.world_palette.is_blue(color) for color in palette) >= 1
    # PAL3 carries exactly one olive/khaki rung and no green. These two moved
    # together: slot 9 held the sole green, which was measurably dead (0.11% of
    # E1M1 wall area, 0.53% of billboard pixels, 0.00% of the weapon overlay),
    # while the olive band it now covers is 15.1% of wall area -- BROWNGRN
    # alone is 26.8% and used to quantize 91.5% neutral. The ban this replaces
    # was guarding against an olive *cast* on grey surfaces; the guard that
    # actually prevents that is the low-chroma neutral clamp, and the check
    # below is the evidence it still holds.
    assert sum(extractor.world_palette.is_green(color) for color in palette) == 0
    assert sum(extractor.world_palette.is_olive(color)
               for color in palette[1:14]) == 1
    assert "BSP_FLOOR_COLOR" not in (ROOT / "src" / "bsp" / "bsp_render.c").read_text()
    ceiling_index = extractor.GLOBAL_CEILING_INDEX
    floor_index = extractor.GLOBAL_FLOOR_INDEX
    assert "FREEDOOM_WORLD_SHADE_MAP" not in assets,         "shading is baked per plane now; a remap table would be a second copy"
    # Depth shading is ON (WALL_SHADE_MODE 2), and these two properties -- a
    # wall must never darken to black, nor to either flat colour -- used to be
    # asserted of the shade CHAIN. The planes are quantized independently now
    # (world_assets.build_shade_planes), so the chain no longer decides them.
    # They are asserted below of the emitted planes themselves, which is what
    # actually reaches the screen: see the black check in the per-plane loop
    # and certify_flat_wall_contrast for the two flats.
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
    # 33 after tools/texture_aliases.py folds the rare-material tail onto the
    # generic head; the campaign is three maps but the atlas is smaller.
    assert len(wall_textures) == 34
    # What actually reaches the screen: one texel per displayed pixel, which is
    # what FREEDOOM_WALL_PACKED_PAIRS carries. Every quality contract below is
    # measured on this, and emit_world_assets certifies the very same grids, so
    # the test and the generator cannot be fed different pictures.
    display_planes = {}
    display_textures = {}
    for name in wall_textures:
        planes = []
        display_textures[name] = extractor.convert_texture(
            extractor.texture_path(name), palette, planes_out=planes)
        display_planes[name] = planes
    assert set(extractor.WALL_BAKE_RECIPES) == set(extractor.CURATED_WALL_MATERIALS)
    assert tuple(extractor.TECH_WALL_MATERIALS) == (
        "COMPTALL", "COMPTILE", "COMPUTE2", "LITE3", "STARG3",
        "STARGR1", "STARTAN1", "STARTAN3", "SUPPORT2",
    )
    # TEKWALL5 takes a magnified facade, not a composed one: its window covers
    # the whole source and only halves both axes, so the material stays Doom's
    # own art at one bake texel per two display texels. COMPUTE2 keeps the
    # authored composition. Pinning facade_compose per material is what keeps
    # "magnify this" from silently becoming "redraw this".
    assert extractor.WALL_BAKE_RECIPES["TEKWALL5"].facade_compose is False
    # COMPTALL must NOT take one. Halving its axes was tried and made its real
    # defect worse -- it drove the dominant index from 67% to 76% while churn
    # improved, which is the whole reason the dominant-index ceiling below
    # exists. Its fix is tone and window, not resolution.
    assert extractor.WALL_BAKE_RECIPES["COMPTALL"].facade_window is None
    # COMPTALL's window takes the right half of a 256-wide source, where every
    # screen and console bank in the art sits; the default left-128 crop had
    # none of them. Sampled dimensions stay (128,128) -- the same size the
    # default crop produced -- so the wall's world-space repeat is unchanged and
    # only the choice of which columns get baked moves.
    assert extractor.CURATED_TEXTURE_WINDOWS == {
        "COMPTALL": (128, 0, 128, 128),
        "COMPUTE2": (128, 0, 128, 56),
    }
    assert extractor.sampled_texture_dimensions("COMPTALL", 256, 128) == (128, 128)
    assert extractor.WALL_BAKE_RECIPES["COMPUTE2"].facade_window == (
        64, 0, 64, 56,
    )
    assert extractor.WALL_BAKE_RECIPES["COMPUTE2"].facade_compose is True
    assert extractor.MIXED_RAMP_MATERIALS == ("SW1STRTN",)
    # SW1STRTN is a brown STARTAN wall with a genuine grey exit-control
    # housing. It must keep both material families: forcing the full texture
    # onto the earth ramp leaves only its red/green lamps recognisable.
    switch = display_textures["SW1STRTN"]
    switch_panel = {switch[y][x] for y in range(62, 113) for x in range(16, 49)}
    assert 11 in switch_panel, "exit switch lost its red indicator"
    assert any(extractor.world_palette.is_neutral(palette[index])
               for index in switch_panel), "exit switch lost its metal housing"

    # The candidate and its old-converter baseline are both generated in
    # memory from the same selected source window and frozen PAL3. This proves
    # the curation is restricted to the eight declared technological walls;
    # doors, switches and every other material remain byte-identical.
    for texture_name, generated in wall_textures.items():
        path = extractor.texture_path(texture_name)
        current = extractor.convert_texture(
            path, palette, use_wall_bake_recipe=False)
        candidate = extractor.convert_texture(
            path, palette, use_wall_bake_recipe=True)
        emitted = [extractor.pair_column_texels(row) for row in candidate]
        assert generated == emitted, "%s generated bake drifted" % texture_name
        if texture_name not in extractor.CURATED_WALL_MATERIALS:
            assert current == candidate, "%s changed outside curated set" % texture_name

    # The normal plane remains fully prepacked; the sparse door plane avoids
    # duplicating all 53 textures for the two door styles.
    door_count = generated_define(assets, "FREEDOOM_WALL_DOOR_TEXTURE_COUNT")
    packed_pair_bytes = (4 * (len(wall_textures) + door_count) *
                         extractor.WALL_TEX_WIDTH * extractor.WALL_TEX_HEIGHT)
    # Unchanged by the sub-texel pair change: the same byte count now carries
    # WALL_TEX_DISPLAY_WIDTH texels per row instead of WALL_TEX_WIDTH.
    # 1835008 for a THREE-map campaign, against 2195456 for the old two-map
    # one: tools/texture_aliases.py folds the rare-material tail onto the
    # generic head, so E1M3 fits with the atlas shrinking rather than growing.
    assert packed_pair_bytes == 1835008

    curated_metrics = [wall_bake_preview.texture_metrics(name)
                       for name in extractor.TECH_WALL_MATERIALS]
    strict_improvements = wall_bake_preview.certify_metrics(curated_metrics)
    # COMPTILE rejoined this list, and its earlier absence was accounting rather
    # than quality. certify_metrics used to score the recipe's edge retention
    # against a recipe-off bake that still carried its isolated texels, and
    # every speck in that bake is a closed boundary the detector credits as
    # retained structure -- so a material noisy enough to be full of them starts
    # from an edge F1 the recipe can only lose by cleaning up, which is exactly
    # what cleanup_isolated exists to do. COMPTILE carries 104 such texels.
    # Scored against a despeckled recipe-off bake, like for like (see
    # wall_bake_preview.texture_metrics), it reads +0.055 rather than negative.
    # No other curated material moves by more than 0.01 under that change, which
    # is what says the fix is confined to the accounting it was aimed at.
    #
    # LITE3 is the one material that does not strictly improve (+0.005 is the
    # bar; it measures -0.001). It has no isolated texels on either side, so
    # nothing above applies to it -- it is simply a material the recipe leaves
    # alone, and every hard guard in certify_metrics still holds for it.
    # Measured from the source PNGs through the curated converter, so this list
    # is unaffected by which materials the campaign actually uses.
    #
    # COMPTALL is deliberately NOT here, and its absence is the honest reading
    # of what its recipe does. This list is "materials whose recipe retains more
    # palette edges than the recipe-off bake", and COMPTALL's recipe rebases the
    # tone instead: it measures 0.781 -> 0.720, because the reference mask is
    # built from the candidate's own expanded contrast while the recipe-off side
    # is scored on a flat grey that has few boundaries to miss. certify_metrics
    # names it CONTRAST_REBASED and holds it to perceptual error and isolated
    # count instead -- 0.47 and 213 -> 60, both large wins -- plus the
    # dominant-index ceiling above, which is the check that actually describes
    # the defect. Listing it here anyway would be claiming a result the metric
    # does not support.
    assert strict_improvements == [
        "COMPTILE", "COMPUTE2", "STARG3", "STARGR1", "STARTAN1",
        "STARTAN3", "SUPPORT2",
    ]
    # The facade certification reads the BAKED atlas, so it only applies while
    # COMPUTE2 is still in it. tools/texture_aliases.py currently folds it onto
    # COMPTALL, and the check restores itself if that alias is ever removed.
    if "COMPUTE2" in display_textures:
        wall_bake_preview.certify_compute2_facade(display_textures["COMPUTE2"])

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
    # Measured on the DISPLAY grid -- one texel per screen pixel, which is what
    # FREEDOOM_WALL_PACKED_PAIRS carries and the eye sees. The emitted
    # FREEDOOM_WALL_TEXTURES is a decimation for the door overlay and is the
    # noisiest possible view of a material, so it is the wrong thing to gate on.
    CHURN_LIMIT = extractor.WALL_CHURN_LIMIT
    # TEKWALL2 and TEKWALL5 used to sit here beside EXITDOOR. Neither belonged:
    # the pair was added inside 12466b1, a large E1M2 commit whose message does
    # not mention textures at all, and unlike EXITDOOR neither carried a written
    # justification. TEKWALL2 does not even exist any more -- texture_aliases
    # folds it away and it has no atlas slot -- and TEKWALL5's noise turned out
    # to be fixable rather than intrinsic: given the magnified facade its
    # vertical churn falls 0.435 -> 0.285, inside the ceiling, so it is now held
    # to the same bar as everything else. EXITDOOR remains the one real outlier.
    CHURN_EXEMPT = {"EXITDOOR"}
    for texture_name in sorted(display_textures):
        with Image.open(extractor.texture_path(texture_name)) as image:
            active_height = extractor.sampled_texture_dimensions(
                texture_name, image.width, image.height)[1]
        rows = display_textures[texture_name]
        churn = extractor.horizontal_churn(rows, active_height)
        vertical_churn = sum(
            1 for y in range(active_height - 1)
            for x in range(len(rows[0]))
            if rows[y][x] != rows[y + 1][x]
        )
        vertical_churn /= max(1, (active_height - 1) * len(rows[0]))
        if texture_name not in CHURN_EXEMPT:
            assert churn <= CHURN_LIMIT, (texture_name, churn)
            assert vertical_churn <= CHURN_LIMIT, (texture_name, vertical_churn)

    # The ceiling above is a texture-wide AVERAGE, and an average has a blind
    # spot big enough to drive the campaign's largest material through. COMPTALL
    # is 14.4% of all wall area and its computer panels baked with 87% of
    # horizontally adjacent texels changing index -- visibly, they read as
    # coloured confetti -- yet it measured 0.255 against the 0.35 limit and no
    # test complained, because the flat grey field surrounding those panels is
    # most of the texture and diluted them away.
    #
    # So bound the WORST 16x16 block as well as the mean. The limit is looser
    # than the average's on purpose: a block that lands wholly inside a panel
    # boundary or a door stripe legitimately churns hard, and this contract is
    # meant to catch "a material the player stares at is made of noise", not to
    # relitigate every local edge.
    #
    # It applies only to materials carrying real screen area. A one-off
    # decorative door earns the benefit of the doubt; a wall you spend an eighth
    # of the game looking at does not. Measured across the three shipped maps,
    # every material over the area threshold passes today, the closest being
    # STARG3 at 0.542 -- worth a look on its own merits later, but deliberately
    # not touched here.
    LOCAL_CHURN_AREA_SHARE = 0.03
    LOCAL_CHURN_LIMIT = 0.55
    LOCAL_CHURN_BLOCK = 16
    area_shares = campaign_wall_area_shares()
    checked = 0
    for texture_name, share in sorted(area_shares.items()):
        if share < LOCAL_CHURN_AREA_SHARE or texture_name not in display_textures:
            continue
        if texture_name in CHURN_EXEMPT:
            continue
        with Image.open(extractor.texture_path(texture_name)) as image:
            active_height = extractor.sampled_texture_dimensions(
                texture_name, image.width, image.height)[1]
        local = worst_block_churn(display_textures[texture_name], active_height,
                                  LOCAL_CHURN_BLOCK)
        assert local <= LOCAL_CHURN_LIMIT, (texture_name, share, local)
        checked += 1
    # The threshold has to actually select the big materials. If aliasing or a
    # map change ever empties this set the contract silently stops running.
    assert checked >= 8, checked
    assert "COMPTALL" in area_shares and \
        area_shares["COMPTALL"] >= LOCAL_CHURN_AREA_SHARE, area_shares.get("COMPTALL")

    # Structure floor, the other half of the same contract: killing the noise
    # must not be achieved by flattening a material into one block. Two clauses,
    # because neither alone says "this still reads as a wall":
    #   - at least two indices carry real area (a lone dominant index plus a
    #     scattering of strays is a flat block, whatever len(counts) says), and
    #   - no index may swallow the whole surface.
    # The cap is NOT the flat-collision contract -- that is enforced directly and
    # far more precisely by certify_flat_wall_contrast below. It is the ceiling
    # on monotony, and it sat at 0.85 while COMPTALL baked 76% of itself to a
    # single grey: the largest wall material in the campaign was three quarters
    # one colour and every check here passed it. 0.85 was inherited from a
    # loosening whose stated reason was a two-tone brick at 57/41, which never
    # needed anything above 0.75 in the first place.
    #
    # 0.70 is where the measured distribution actually separates. Post-fix, the
    # highest non-uniform material is BROWNHUG at 0.65 (a genuinely plain brown
    # wall, worth its own look but not touched here), then SW1COMP 0.59 and
    # BROWNGRN 0.58; COMPTALL now sits at 0.51. Nothing legitimate is near the
    # line, and a material that crosses it is making the same mistake COMPTALL
    # made rather than expressing something Doom drew.
    #
    # This is the check the churn work needed and did not have. Churn measures
    # how OFTEN neighbours differ, so flattening a texture improves every churn
    # number at once -- the facade attempt on COMPTALL scored 0.062 horizontal
    # churn, the best in the atlas, by deleting the wall. Only a dominance
    # ceiling can tell those two outcomes apart.
    MIN_STRUCTURAL_SHARE = 0.02
    # Materials whose source really is a near-uniform field: LITE3 is a white
    # light panel, COMPTILE a two-tone tile. Manufacturing spread into them would
    # be inventing detail Doom never drew.
    UNIFORM_MATERIALS = ("LITE3", "COMPTILE", "DOORSTOP", "STARGR1", "SUPPORT2")
    SOLID_MATERIALS = {"DOOR1"}
    for texture_name, rows in sorted(display_textures.items()):
        flat = [value for row in rows for value in row]
        counts = Counter(flat)
        structural = [index for index, hits in counts.items()
                      if hits >= MIN_STRUCTURAL_SHARE * len(flat)]
        assert len(structural) >= (1 if texture_name in SOLID_MATERIALS else 2), \
            (texture_name, counts)
        dominant_share = counts.most_common(1)[0][1] / len(flat)
        cap = 1.0 if texture_name in SOLID_MATERIALS else \
              (0.90 if texture_name in UNIFORM_MATERIALS else 0.70)
        assert dominant_share <= cap, (texture_name, dominant_share, counts)
    # Named so a regression reports as "COMPTALL went monotone again" rather
    # than as an anonymous cap breach on the campaign's largest wall.
    comptall_flat = [v for row in display_textures["COMPTALL"] for v in row]
    comptall_share = (Counter(comptall_flat).most_common(1)[0][1] /
                      len(comptall_flat))
    assert comptall_share <= 0.60, comptall_share

    # The contract behind the global flats: no wall may read as an unbroken
    # field of the ceiling's or the floor's colour, at ANY shade level. Index
    # inequality alone is not enough -- adjacent PAL3 rungs are only ~0.13 apart
    # in Oklab, so this is enforced perceptually. Reuses the same function the
    # bake fails on, so the test and the generator cannot disagree.
    solid = extractor.certify_flat_wall_contrast(
        palette, display_planes, ceiling_index, floor_index)
    assert solid, "expected some near-solid wall materials to guard against"

    # What the shade planes must hold on their own, now that no chain derives
    # them. A distant wall may not go black (it would read as a hole), and it
    # may not lose its last tonal step -- a plane down to ONE index is the
    # "almost two simple colours" defect that motivated build_shade_planes,
    # and the old chain produced exactly that for every neutral material from
    # shade 2 onward. Dark materials whose level 0 is already mostly the
    # bottom rung have nowhere left to go, so they are named rather than
    # silently tolerated.
    BOTTOMED_OUT = {"METAL1", "DOORTRAK", "STONE3"}
    lightness = lambda color: extractor.world_palette.oklab(tuple(color))[0]
    for texture_name, planes in sorted(display_planes.items()):
        assert len(planes) == extractor.WORLD_SHADE_LEVELS, texture_name
        base_counts = Counter(v for row in planes[0] for v in row)
        for level, rows in enumerate(planes):
            counts = Counter(v for row in rows for v in row)
            # Shading may not INTRODUCE black or a flat colour. Texels that
            # already carry one at level 0 keep it: build_shade_planes is
            # monotonic, so it never brightens them away, and a black recess
            # or a texel that legitimately quantized to a flat rung up close
            # is not something distance should repaint.
            introduced = set(counts) - set(base_counts)
            assert 0 not in introduced, (
                "shading darkened a wall into black", texture_name, level)
            entered = {ceiling_index, floor_index} & introduced
            assert not entered, (
                "shading darkened a wall into a flat colour",
                texture_name, level, entered)
            # A plane down to ONE index is the flatness this whole
            # mechanism exists to prevent: the old index chain produced
            # exactly that for every neutral material from shade 2 on.
            # Materials whose level 0 already sits on the bottom rung have
            # nowhere left to go, so they are named, not silently allowed.
            minimum = 1 if texture_name in BOTTOMED_OUT else 2
            assert len(counts) >= minimum, (
                "shade plane collapsed to one colour", texture_name, level)
            # Fog must not shift hue and must not brighten. Asserted per
            # texel against level 0: that is what the old chain's family
            # filter and its darker-only rule bought, and it has to survive
            # each level being quantized independently.
            for y, row in enumerate(rows):
                for x, value in enumerate(row):
                    base = planes[0][y][x]
                    family = extractor.shade_family
                    assert family(palette[value]) == family(palette[base]), (
                        "shading shifted hue", texture_name, level,
                        base, value)
                    assert lightness(palette[value]) <= lightness(
                        palette[base]) + 1e-6, (
                        "shading brightened a texel", texture_name, level,
                        base, value)

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
        generated_map2 = temp_root / "generated_e1m2_map.c"
        generated_map3 = temp_root / "generated_e1m3_map.c"
        generated_assets = temp_root / "generated_assets.h"
        generated_limits = temp_root / "generated_map_limits.h"
        subprocess.run([
            sys.executable, str(EXTRACTOR_PATH),
            "--wad", str(ROOT / "DOOM1.WAD"),
            "--maps", "E1M1", "E1M2", "E1M3",
            "--map-out-dir", str(temp_root),
            "--assets-out", str(generated_assets),
            "--limits-out", str(generated_limits),
        ], cwd=ROOT, check=True, stdout=subprocess.DEVNULL)
        assert generated_map.read_bytes() == MAP_PATH.read_bytes()
        assert generated_map2.read_bytes() == MAP2_PATH.read_bytes()
        assert generated_map3.read_bytes() == MAP3_PATH.read_bytes()
        assert generated_assets.read_bytes() == ASSETS_PATH.read_bytes()
        assert generated_limits.read_bytes() == LIMITS_PATH.read_bytes()

        # Exercise the CLI's complete artifact contract in a disposable tree:
        # one atlas per curated material, exact-renderer scene pairs (including
        # the close COMPUTE2 failure angle), and one animated approach/lateral
        # sequence plus contact sheet per curated material the campaign places.
        preview_root = temp_root / "preview"
        report, scene_paths = wall_bake_preview.build_preview(
            ROOT / "DOOM1.WAD", preview_root)
        assert report["wad_sha256"] == wall_bake_preview.EXPECTED_WAD_SHA256
        assert report["segments"] == E1M1_SEG_COUNT
        assert report["packed_pair_bytes"] == E1M1_PREVIEW_PACKED_PAIR_BYTES
        # 13, not 15: COMPTILE and COMPUTE2 are aliased onto COMPTALL, so no
        # SEG places them and they get no in-world preview scene. COMPTALL
        # itself does get one now that it is curated -- and being the material
        # those two fold INTO, it is the scene that actually shows what every
        # computer panel in the campaign looks like.
        assert len(scene_paths) == 13
        assert len(list((preview_root / "atlases").glob("*.png"))) == 9
        # One motion strip per curated material still placed in E1M1: 7 of the
        # 9, for the same reason as the scene count above.
        assert len(list((preview_root / "motion").glob("*.gif"))) == 7
        assert len(list((preview_root / "motion").glob("*-contact.png"))) == 7
        assert (preview_root / "report.json").is_file()

    print("ok    walls: 64x128, stride 2/80 columns, native-V short textures")


if __name__ == "__main__":
    main()
