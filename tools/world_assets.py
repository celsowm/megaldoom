#!/usr/bin/env python3
"""Bake Doom's wall textures and flats into MegalDoom's PAL3 palette and the
FREEDOOM_WALL_TEXTURES / FREEDOOM_SECTOR_VISUALS C tables.

Knows nothing about BSP geometry -- its inputs are a texture-name usage count
and a list of sectors (each a dict with floor/ceiling names and a light
level), and its output is one generated C header plus the (texture_ids,
texture_meta, sector_visuals, palette) bsp_emit.py needs to resolve each
seg's texture id.
"""

import os
import re
from collections import Counter

from PIL import Image
import world_palette

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSET_ROOT = os.path.join(PROJECT_ROOT, "res", "originaldoom")
RAYCAST_HEADER = os.path.join(PROJECT_ROOT, "src", "raycast.h")


def runtime_wall_tex_dim():
    with open(RAYCAST_HEADER, "r", encoding="utf-8") as stream:
        match = re.search(r"^#define\s+WALL_TEX_DIM\s+(\d+)\s*$",
                          stream.read(), re.MULTILINE)
    if not match:
        raise RuntimeError("WALL_TEX_DIM is missing from src/raycast.h")
    dimension = int(match.group(1))
    if dimension <= 0 or dimension & (dimension - 1):
        raise RuntimeError("WALL_TEX_DIM must be a positive power of two")
    return dimension


# Converter and runtime intentionally share the public definition in raycast.h.
WALL_TEX_DIM = runtime_wall_tex_dim()
FALLBACK_TEXTURE = "__FALLBACK__"
FALLBACK_TEXTURE_SOURCE = "GRAY7"
WORLD_COLOR_DAMAGE = 14
WORLD_COLOR_WARNING = 15
WORLD_SPRITE_INPUTS = [
    "PISGA0", "PISGB0", "PISFA0",
    "BON1A0", "BKEYA0", "YKEYA0", "RKEYA0", "STIMA0", "MEDIA0",
    "BON2A0", "ARM1A0", "ARM2A0", "CLIPA0", "AMMOA0", "CANDA0",
    "CBRAA0", "COLUA0", "ELECA0", "BAR1A0", "TREDA0",
    "POSSA1", "POSSB1", "POSSC1", "POSSD1", "POSSF1", "POSSH0",
    "POSSI0", "POSSJ0", "POSSK0", "POSSL0",
    "BEXPA0", "BEXPB0", "BEXPC0", "BEXPD0", "BEXPE0",
    "PUFFA0", "PUFFB0", "PUFFC0", "PUFFD0", "BLUDA0", "BLUDB0", "BLUDC0",
]
WEAPON_SPRITE_NAMES = ("PISGA0", "PISGB0", "PISFA0")


def texture_macro(name):
    if name == FALLBACK_TEXTURE:
        return "MEGALDOOM_TEX_FALLBACK"
    return "MEGALDOOM_TEX_" + re.sub(r"[^A-Z0-9_]", "_", name)


def texture_path(name):
    source = FALLBACK_TEXTURE_SOURCE if name == FALLBACK_TEXTURE else name
    return os.path.join(ASSET_ROOT, "textures", source + ".png")


def flat_path(name):
    if name == "F_SKY1":
        return os.path.join(ASSET_ROOT, "textures", "SKY1.png")
    return os.path.join(ASSET_ROOT, "flats", name + ".png")


def md_color(rgb):
    """Quantize RGB to the Mega Drive's three bits per channel."""
    return world_palette.md_color(rgb)


# Tuned so E1M1's dominant wall/flat median luminance (~39/255, see
# tools/test-wall-quality.py dark-brown checks) lands near the middle of
# PAL3's ramp instead of the bottom two rungs. This is a display-space
# "levels lift", not a physically accurate linear-light gamma: it exists to
# recover the light-level compensation the renderer does NOT apply to wall
# texels (see WALL_SHADE_MODE in renderer_pack.c, which only shades by depth
# and N/S side, never by the source sector's light field). Applied uniformly
# to every wall/flat pixel, so relative differences between materials (a lit
# STARTAN3 corridor vs a dark BROWN144 one) are preserved, not equalized.
WALL_TONE_GAMMA = 0.55


def tone_curve(rgb):
    return tuple(int(round(255.0 * ((max(0, min(255, channel)) / 255.0) ** WALL_TONE_GAMMA)))
                 for channel in rgb)


def _fill_transparent_with_average(image):
    """Return an opaque RGB copy, filling any transparent area with the
    average of the image's own opaque pixels instead of black.

    Composited wall textures (TEXTURE1/2 patches over PNAMES) can leave real
    gaps -- BRNBIGC is 28% uncovered -- and Image.convert("RGB") on an RGBA
    source keeps whatever RGB the transparent pixels were initialized to
    (black, per wad-extract.py's Image.new("RGBA", ..., (0,0,0,0))). Composing
    over black instead of the texture's own tone painted 26% of BRNBIGC's
    quantized output as literal black pixels; composing over the texture's own
    average keeps the gap visually neutral instead.
    """
    if image.mode != "RGBA":
        return image.convert("RGB")
    rgba = image.convert("RGBA")
    pixels = list(rgba.get_flattened_data())
    opaque = [(r, g, b) for r, g, b, a in pixels if a >= 128]
    if not opaque or len(opaque) == len(pixels):
        return rgba.convert("RGB")
    fill = tuple(sum(p[channel] for p in opaque) // len(opaque) for channel in range(3))
    background = Image.new("RGB", rgba.size, fill)
    background.paste(rgba, (0, 0), rgba)
    return background


def image_average(path):
    with Image.open(path) as image:
        opaque = _fill_transparent_with_average(image)
        pixels = list(opaque.get_flattened_data())
    pixels = [tone_curve(p) for p in pixels]
    count = max(1, len(pixels))
    return tuple(sum(p[channel] for p in pixels) // count for channel in range(3))


def add_image_histogram(histogram, path, size, weight, apply_tone=False):
    with Image.open(path) as image:
        rgba = image.convert("RGBA").resize(size, Image.Resampling.BOX)
        for r, g, b, a in rgba.get_flattened_data():
            if a >= 128:
                color = tone_curve((r, g, b)) if apply_tone else (r, g, b)
                histogram[color] += weight


def nearest_palette_index(rgb, palette, allowed=None):
    return world_palette.nearest_index(rgb, palette, allowed)


def build_world_palette(texture_names, texture_usage, sectors):
    histogram = Counter()
    for name in texture_names:
        weight = max(1, min(16, texture_usage.get(name, 1)))
        add_image_histogram(histogram, texture_path(name), (WALL_TEX_DIM, WALL_TEX_DIM),
                            weight, apply_tone=True)

    # PAL3 is shared by the 3D scene, weapon and every runtime billboard. Feed
    # every consumed source into the same deterministic histogram so improving
    # walls cannot silently recolor actors or effects. The weapon sprites are
    # on screen for 100% of gameplay (unlike any single wall texture or actor),
    # so they get a much heavier weight than the rest of WORLD_SPRITE_INPUTS.
    # No tone curve here: sprites are drawn with Doom's original tones, not the
    # lifted wall/flat ramp, so they must not bias PAL3's family budgets toward
    # brightened colors that then look wrong on the actual sprite pixels.
    for name in WORLD_SPRITE_INPUTS:
        relative = os.path.join("sprites", name + ".png")
        weight = 48 if name in WEAPON_SPRITE_NAMES else 4
        add_image_histogram(histogram, os.path.join(ASSET_ROOT, relative),
                            (32, 32), weight)

    flat_usage = Counter()
    for sector in sectors:
        flat_usage[sector["floor_name"]] += 1
        flat_usage[sector["ceiling_name"]] += 1
    for name, count in flat_usage.items():
        average = image_average(flat_path(name))
        histogram[average] += max(512, count * 128)

    # Palette candidates already live on the VDP's 3-bit/channel grid. Collapse
    # the source histogram to that grid before the constrained search: the
    # objective stays identical at console precision and generation remains fast.
    vdp_histogram = Counter()
    for color, weight in histogram.items():
        vdp_histogram[md_color(color)] += weight
    return world_palette.build_palette(
        vdp_histogram, (0xD8, 0x28, 0x18), (0xD8, 0xB0, 0x48))


def build_shade_map(palette):
    result = []
    for index, color in enumerate(palette):
        if index == 0:
            result.append(0)
            continue
        luminance = world_palette.oklab(tuple(color))[0]
        darker = [i for i, candidate in enumerate(palette)
                  if world_palette.oklab(tuple(candidate))[0] < luminance - 1e-6]
        if world_palette.is_neutral(color):
            neutral = [i for i in darker
                       if world_palette.is_neutral(palette[i])]
            if neutral:
                darker = neutral
        elif not world_palette.is_green(color):
            non_green = [i for i in darker
                         if not world_palette.is_green(palette[i])]
            if non_green:
                darker = non_green
        target = tuple(channel * 2 // 3 for channel in color)
        result.append(nearest_palette_index(target, palette, darker or [0]))
    return result


def build_shade_lut(palette, levels=4):
    """Build deterministic progressively-darker palette-index shade levels."""
    shade_map = build_shade_map(palette)
    result = [list(range(len(palette)))]
    for _ in range(1, levels):
        result.append([shade_map[index] for index in result[-1]])
    return result


# Every texture is stored as a square WALL_TEX_DIM grid, so a source wider than
# this loses horizontal detail with nothing gained: COMPUTE2 (256x56) was being
# squashed 4:1 across while its 56 rows were stretched to 64, which turned a
# panel of small readouts into horizontal mush. Capping the sampled width at
# 2*WALL_TEX_DIM keeps the squash at 2:1 -- what every 128-wide texture already
# gets and what STARTAN3 et al look fine at -- at the cost of the material
# repeating over half the world distance. Only sources wider than the cap are
# touched; in E1M1 that is COMPUTE2 and the 256x128 fallback.
WALL_TEX_MAX_SOURCE_WIDTH = 2 * WALL_TEX_DIM


def sampled_texture_width(width):
    """Source width actually sampled into the WALL_TEX_DIM grid."""
    return min(width, WALL_TEX_MAX_SOURCE_WIDTH)


# Oklab-chroma clamp for wall/flat quantization: separates real dark browns
# (BROWN144 chroma~0.026, BROWN96 chroma~0.033) from truly neutral materials
# (COMPUTE2 chroma~0.008, BROWNGRN chroma~0.014) that the old absolute RGB
# spread of 36 could not tell apart. Sprites keep the legacy RGB clamp (see
# tools/world-palette-lut.py, which calls best_mix with no override) so this
# tuning cannot recolor the weapon or billboards.
WALL_CHROMA_THRESHOLD = 0.02
# Tighter dither gate for detailed/high-contrast walls: at RAY_COL_STRIDE 2
# each texel is 2 screen pixels, so the sprite-tuned defaults
# (pair_max_delta=73, gain_ratio=0.65) produce a salt-and-pepper checkerboard
# that flickers in motion (EXITDOOR was 66% pixel-to-pixel change, BROWN1 20%
# diagonal checker). NOT applied to the GRAY/METAL/STONE neutral-only bucket:
# those are smooth grey gradients where dithering is exactly what avoids
# visible banding, and the tight gate was pushing METAL1 to hard-snap its
# whole surface between two grey rungs instead of blending them.
WALL_PAIR_MAX_DELTA = 40
WALL_GAIN_RATIO = 0.4


def _has_short_period_vertical_banding(columns):
    """Detect a strong period-4-or-shorter vertical alternation, e.g. LITE3's
    pure-white band repeating every 4 texels. MEGALDOOM_WALL_TEX_Y_BY_HEIGHT's
    point-sampled vertical resampler turns that into a hard-edged grid on any
    wall short enough to skip rows (see the D4 grid artifact in the wall-
    quality plan). Row-averaged so per-column noise cannot trigger it, and
    gated tight enough that ordinary detailed materials (METAL1's slow panel
    gradient, STARTAN1's rivets) do not qualify -- this must stay a narrow
    fix, not a blanket blur, or it re-softens genuinely sharp texture detail.
    """
    dim = len(columns[0])
    row_luma = [sum(columns[x][y][c] for x in range(len(columns)) for c in range(3))
               for y in range(dim)]
    step1 = sum(abs(row_luma[y] - row_luma[y - 1]) for y in range(dim)) / dim
    step4 = sum(abs(row_luma[y] - row_luma[y - 4]) for y in range(dim)) / dim
    return step1 > 400 and step4 < step1 * 0.2


def convert_texture(path, palette):
    with Image.open(path) as image:
        sampled = sampled_texture_width(image.width)
        if sampled != image.width:
            image = image.crop((0, 0, sampled, image.height))
        opaque = _fill_transparent_with_average(image)
        resized = opaque.resize((WALL_TEX_DIM, WALL_TEX_DIM), Image.Resampling.BOX)
        columns = [[resized.getpixel((x, y)) for y in range(WALL_TEX_DIM)]
                  for x in range(WALL_TEX_DIM)]
        if _has_short_period_vertical_banding(columns):
            # 4-tap vertical box average: wide enough to flatten a period-4
            # alternation (unlike a narrow 1-2-1 blur, which softens the edges
            # but leaves the alternation itself intact and still aliasable).
            smoothed = [[None] * WALL_TEX_DIM for _ in range(WALL_TEX_DIM)]
            for x in range(WALL_TEX_DIM):
                column = columns[x]
                for y in range(WALL_TEX_DIM):
                    taps = [column[(y + offset) % WALL_TEX_DIM] for offset in (-1, 0, 1, 2)]
                    smoothed[x][y] = tone_curve(tuple(
                        sum(tap[c] for tap in taps) // 4 for c in range(3)))
        else:
            smoothed = [[tone_curve(columns[x][y]) for y in range(WALL_TEX_DIM)]
                       for x in range(WALL_TEX_DIM)]
        material_name = os.path.splitext(os.path.basename(path))[0].upper()
        is_neutral_material = material_name.startswith(("GRAY", "METAL", "STONE"))
        if is_neutral_material:
            allowed = [index for index in range(0, WORLD_COLOR_DAMAGE)
                       if world_palette.is_neutral(palette[index])]
            pair_max_delta, gain_ratio = 73, 0.65  # sprite-tuned defaults
        else:
            allowed = range(0, WORLD_COLOR_DAMAGE)
            pair_max_delta, gain_ratio = WALL_PAIR_MAX_DELTA, WALL_GAIN_RATIO
        cache = {}
        return [[world_palette.dither_index(smoothed[x][y], palette,
                                            x, y, False, cache,
                                            allowed, WALL_CHROMA_THRESHOLD,
                                            pair_max_delta, gain_ratio)
                 for x in range(WALL_TEX_DIM)] for y in range(WALL_TEX_DIM)]


def texture_u_scale_q12(size):
    """Map Doom's source-width repeat into the WALL_TEX_DIM runtime texture.

    Doom textures are not restricted to power-of-two widths (E1M2 uses a
    24-pixel-wide material).  A fixed-point scale keeps their world-space
    repeat period without requiring a division in the renderer's column loop.
    Rounding upward makes exact source-width boundaries wrap deterministically.
    """
    if size <= 0:
        raise ValueError("Invalid texture width %d" % size)
    return (WALL_TEX_DIM * 4096 + size - 1) // size


def emit_world_assets(path, texture_usage, sectors):
    texture_names = [FALLBACK_TEXTURE] + sorted(
        name for name in texture_usage if name != FALLBACK_TEXTURE)
    for name in texture_names:
        if not os.path.isfile(texture_path(name)):
            raise FileNotFoundError("Wall texture source not found: %s" % texture_path(name))

    # Derive PAL3 from the exact active-map surfaces plus every runtime sprite.
    # The result is deterministic, VDP-valid and avoids forcing Doom browns and
    # grays through the old olive-heavy hand-authored ramp.
    palette = build_world_palette(texture_names, texture_usage, sectors)
    if len(palette) != 16:
        raise RuntimeError("World palette must contain exactly 16 colors")
    shade_map = build_shade_map(palette)
    shade_lut = build_shade_lut(palette)
    # Retained as MEGALDOOM_WORLD_COLOR_FLOOR: a neutral fallback still used by
    # the (currently dead, RENDERER_SPARSE_FB==0) static-atlas boot path in
    # renderer.c. Per-sector floors below no longer force every sector to it.
    floor_candidates = [
        index for index in range(1, WORLD_COLOR_DAMAGE)
        if world_palette.is_neutral(palette[index])
    ]
    if not floor_candidates:
        raise RuntimeError("World palette has no neutral fixed-floor color")
    fixed_floor_index = nearest_palette_index(
        (36, 36, 36), palette, floor_candidates)
    texture_ids = {name: index for index, name in enumerate(texture_names)}
    texture_meta = {}
    converted = []
    for name in texture_names:
        source = texture_path(name)
        with Image.open(source) as image:
            width, height = image.size
        # The stored grid only ever holds sampled_texture_width() columns, so the
        # world repeat must be derived from that, not from the source width, or
        # the material would be stretched instead of repeated.
        width = sampled_texture_width(width)
        texture_meta[name] = dict(
            width=width,
            height=height,
            u_scale_q12=texture_u_scale_q12(width),
        )
        converted.append(convert_texture(source, palette))

    # Both ceiling AND floor are per-sector flats now (previously the floor was
    # forced to one ROM-constant color for the whole level, which combined with
    # dark walls collapsing to the same grey -- see D1/D3 in the wall-quality
    # plan -- made whole rooms read as one undifferentiated grey field). The
    # renderer light level is applied to both, matching the ceiling's existing
    # treatment and giving the floor the source-flat identity it never had.
    flat_allowed_base = list(range(1, WORLD_COLOR_DAMAGE))

    def flat_color(name, light, allowed):
        average = image_average(flat_path(name))
        if name != "F_SKY1":
            average = tuple(channel * light // 255 for channel in average)
        first, second, coverage = world_palette.best_mix(
            average, palette, True, allowed, WALL_CHROMA_THRESHOLD)
        # Damage and warning are runtime effects, not flat materials.
        if first >= WORLD_COLOR_DAMAGE or second >= WORLD_COLOR_DAMAGE:
            first = nearest_palette_index(average, palette, allowed)
            second = first
            coverage = 0
        return first, second, coverage

    sector_visuals = []
    for sector in sectors:
        ceiling = flat_color(sector["ceiling_name"], sector["light"], flat_allowed_base)
        # Excluding the ceiling's resolved index(es) guarantees a sector's
        # floor never quantizes to the exact same solid color as its own
        # ceiling -- previously 65/85 E1M1 sectors did exactly that (with the
        # OLD single ROM-constant floor), rendering as one flat gray from top
        # to bottom with no visible seam.
        floor_allowed = [index for index in flat_allowed_base
                         if index not in (ceiling[0], ceiling[1])] or flat_allowed_base
        floor = flat_color(sector["floor_name"], sector["light"], floor_allowed)
        sector_visuals.append(ceiling + floor)

    lines = [
        "#ifndef MEGALDOOM_GENERATED_ASSETS_H",
        "#define MEGALDOOM_GENERATED_ASSETS_H",
        "",
        "#include <genesis.h>",
        "#include \"raycast.h\"",
        "",
        "// Generated deterministically by tools/wad-map-extract.py.",
        "// Exact solid-wall texture catalog for E1M1; index 0 is the fallback.",
        "#define FREEDOOM_WALL_TEXTURE_COUNT %d" % len(texture_names),
        "#define FREEDOOM_SECTOR_VISUAL_COUNT %d" % len(sector_visuals),
        "#define MEGALDOOM_WORLD_COLOR_FLOOR %d" % fixed_floor_index,
        "#define MEGALDOOM_WORLD_COLOR_DAMAGE %d" % WORLD_COLOR_DAMAGE,
        "#define MEGALDOOM_WORLD_COLOR_WARNING %d" % WORLD_COLOR_WARNING,
    ]
    for name in texture_names:
        lines.append("#define %s %d" % (texture_macro(name), texture_ids[name]))
    lines.extend([
        "",
        "static const u32 FREEDOOM_WORLD_PALETTE[16] = {",
        "    " + ", ".join("0x%02X%02X%02X" % color for color in palette),
        "};",
        "",
        "// Per-sector ceiling plus fixed neutral floor: primary, secondary, Bayer coverage.",
        "static const u8 FREEDOOM_SECTOR_VISUALS[FREEDOOM_SECTOR_VISUAL_COUNT][6] = {",
    ])
    for visual in sector_visuals:
        lines.append("    {%s}," % ", ".join(str(value) for value in visual))
    lines.extend([
        "};",
        "",
        "static const u8 FREEDOOM_WORLD_SHADE_MAP[16] = {",
        "    " + ", ".join(str(value) for value in shade_map),
        "};",
        "",
        "#define FREEDOOM_WORLD_SHADE_LEVELS 4",
        "static const u8 FREEDOOM_WORLD_SHADE_LUT[FREEDOOM_WORLD_SHADE_LEVELS][16] = {",
    ])
    for level in shade_lut:
        lines.append("    {" + ", ".join(str(value) for value in level) + "},")
    lines.extend([
        "};",
        "",
        "static const u16 FREEDOOM_WALL_TEXTURE_USCALE_Q12[FREEDOOM_WALL_TEXTURE_COUNT] = {",
        "    " + ", ".join(str(texture_meta[name]["u_scale_q12"]) for name in texture_names),
        "};",
        "",
        "static const u16 FREEDOOM_WALL_TEXTURE_WIDTH[FREEDOOM_WALL_TEXTURE_COUNT] = {",
        "    " + ", ".join(str(texture_meta[name]["width"]) for name in texture_names),
        "};",
        "",
        "static const u16 FREEDOOM_WALL_TEXTURE_HEIGHT[FREEDOOM_WALL_TEXTURE_COUNT] = {",
        "    " + ", ".join(str(texture_meta[name]["height"]) for name in texture_names),
        "};",
        "",
        "static const u8 FREEDOOM_WALL_TEXTURES[FREEDOOM_WALL_TEXTURE_COUNT][WALL_TEX_DIM][WALL_TEX_DIM] = {",
    ])
    for texture_index, rows in enumerate(converted):
        lines.append("    { // %d: %s (%dx%d)" % (
            texture_index, texture_names[texture_index],
            texture_meta[texture_names[texture_index]]["width"],
            texture_meta[texture_names[texture_index]]["height"]))
        for row in rows:
            lines.append("        {" + ", ".join(str(value) for value in row) + "},")
        lines.append("    },")
    lines.extend([
        "};",
        "",
        "// ROM-resident, shade-ready pairs for the shipped stride-2 wall packer.",
        "// Layout is [door_style][shade][texture][x][y]; each u8 repeats one 4bpp",
        "// palette index across the two horizontal pixels of a sampled ray.",
        "static const u8 FREEDOOM_WALL_PACKED_PAIRS",
        "    [2][FREEDOOM_WORLD_SHADE_LEVELS][FREEDOOM_WALL_TEXTURE_COUNT]",
        "    [WALL_TEX_DIM][WALL_TEX_DIM] = {",
    ])
    for door_style in range(2):
        lines.append("    { // door style %d" % door_style)
        for level_index, level in enumerate(shade_lut):
            lines.append("        { // shade %d" % level_index)
            for texture_index, rows in enumerate(converted):
                lines.append("            { // %d: %s" %
                             (texture_index, texture_names[texture_index]))
                for tex_x in range(WALL_TEX_DIM):
                    colors = []
                    for tex_y in range(WALL_TEX_DIM):
                        texel = rows[tex_y][tex_x] & 0x0F
                        if door_style:
                            border = WALL_TEX_DIM // 16
                            safety = WALL_TEX_DIM // 8
                            if tex_x < border or tex_x >= WALL_TEX_DIM - border or tex_y < border:
                                texel = 0
                            elif tex_y >= WALL_TEX_DIM - safety:
                                texel = WORLD_COLOR_WARNING if tex_x & safety else 0
                        colors.append(level[texel] * 0x11)
                    lines.append("                {" + ", ".join(
                        "0x%04X" % value for value in colors) + "},")
                lines.append("            },")
            lines.append("        },")
        lines.append("    },")
    lines.extend(["};", "", "#endif", ""])
    with open(path, "w", newline="\n") as fh:
        fh.write("\n".join(lines))
    return texture_ids, texture_meta, sector_visuals, palette
