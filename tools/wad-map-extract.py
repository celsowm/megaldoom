#!/usr/bin/env python3
"""Parse, flatten, certify and atomically emit a Doom map for MegalDoom.

Doom's prebuilt VERTEXES/SEGS/SSECTORS/NODES map almost 1:1 onto our Bsp*
structs (even the 0x8000 subsector-leaf bit is identical), so we convert them
directly -- no on-device node builder needed.

The runtime is deliberately one level high. Structural walls, grouped doors,
switches, colored locks and exits survive; height-only stairs, lifts, ledges and
drop-offs become open passages. Before either generated file is replaced, an
offline navigation proof must find a medium-skill single-player route to an exit.

Usage: python tools/wad-map-extract.py [--map E1M1] [--wad DOOM1.WAD]
"""

import argparse
from array import array
from collections import Counter
import math
import os
import re
import struct

from PIL import Image
import world_palette

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_WAD = os.path.join(PROJECT_ROOT, "DOOM1.WAD")
DEFAULT_ASSET_OUT = os.path.join(PROJECT_ROOT, "src", "generated_assets.h")
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


def validate_spatial_grid(vertices, segs, grid_min_x, grid_min_y, grid_w,
                          grid_h, grid_cell, grid_cells, start_x, start_y):
    """Prove indexed queries match exhaustive queries on deterministic samples."""
    def cell_coord(value, origin):
        return (value - origin) // grid_cell

    def circle_candidates(x, y, radius):
        result = set()
        cx0 = max(0, cell_coord(x - radius, grid_min_x))
        cy0 = max(0, cell_coord(y - radius, grid_min_y))
        cx1 = min(grid_w - 1, cell_coord(x + radius, grid_min_x))
        cy1 = min(grid_h - 1, cell_coord(y + radius, grid_min_y))
        for cy in range(cy0, cy1 + 1):
            for cx in range(cx0, cx1 + 1):
                result.update(grid_cells[cy * grid_w + cx])
        return result

    def los_candidates(x0, y0, x1, y1):
        cx, cy = cell_coord(x0, grid_min_x), cell_coord(y0, grid_min_y)
        ex, ey = cell_coord(x1, grid_min_x), cell_coord(y1, grid_min_y)
        sx = 1 if ex > cx else (-1 if ex < cx else 0)
        sy = 1 if ey > cy else (-1 if ey < cy else 0)
        ray_dx, ray_dy = abs(x1 - x0), abs(y1 - y0)
        result = set()
        guard = 0
        while True:
            guard += 1
            if guard > (grid_w + grid_h + 8):
                raise SystemExit("blockmap LOS traversal did not converge for ray %r" %
                                 ((x0, y0, x1, y1),))
            if 0 <= cx < grid_w and 0 <= cy < grid_h:
                result.update(grid_cells[cy * grid_w + cx])
            if cx == ex and cy == ey:
                return result
            if sx == 0:
                cy += sy
                continue
            if sy == 0:
                cx += sx
                continue
            if cx == ex:
                cy += sy
                continue
            if cy == ey:
                cx += sx
                continue
            xb = grid_min_x + ((cx + 1 if sx > 0 else cx) * grid_cell)
            yb = grid_min_y + ((cy + 1 if sy > 0 else cy) * grid_cell)
            xdist = xb - x0 if sx > 0 else x0 - xb
            ydist = yb - y0 if sy > 0 else y0 - yb
            lhs, rhs = xdist * ray_dy, ydist * ray_dx
            if lhs == rhs:
                cx, cy = cx + sx, cy + sy
            elif lhs < rhs:
                cx += sx
            else:
                cy += sy

    def cross(ax, ay, bx, by, cx, cy):
        return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax)

    def strict_intersection(x0, y0, x1, y1, ax, ay, bx, by):
        d1 = cross(x0, y0, x1, y1, ax, ay)
        d2 = cross(x0, y0, x1, y1, bx, by)
        d3 = cross(ax, ay, bx, by, x0, y0)
        d4 = cross(ax, ay, bx, by, x1, y1)
        return (((d1 > 0 and d2 < 0) or (d1 < 0 and d2 > 0)) and
                ((d3 > 0 and d4 < 0) or (d3 < 0 and d4 > 0)))

    # Circle broad phase: any segment whose expanded AABB contains the sample
    # point must be present. Include route start, cell boundaries and wall ends.
    points = [(start_x, start_y)]
    for x, y in vertices:
        points.extend(((x, y), (x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)))
    for cy in range(grid_h + 1):
        for cx in range(grid_w + 1):
            points.append((grid_min_x + cx * grid_cell,
                           grid_min_y + cy * grid_cell))
    for radius in (24, 32):
        for x, y in points:
            candidates = circle_candidates(x, y, radius)
            for i, seg in enumerate(segs):
                ax, ay = vertices[seg["v1"]]
                bx, by = vertices[seg["v2"]]
                if (x + radius >= min(ax, bx) and x - radius <= max(ax, bx) and
                        y + radius >= min(ay, by) and y - radius <= max(ay, by) and
                        i not in candidates):
                    raise SystemExit("blockmap circle validation failed at segment %d" % i)

    # LOS broad phase: a segment can intersect a query only if both AABBs
    # overlap. Exercise horizontal, vertical, diagonal, zero-length and the
    # player-start-to-wall route family.
    rays = [(start_x, start_y, start_x, start_y)]
    bounds = (grid_min_x, grid_min_y,
              grid_min_x + grid_w * grid_cell - 1,
              grid_min_y + grid_h * grid_cell - 1)
    rays.extend(((bounds[0], start_y, bounds[2], start_y),
                 (start_x, bounds[1], start_x, bounds[3]),
                 (bounds[0], bounds[1], bounds[2], bounds[3]),
                 (bounds[0], bounds[3], bounds[2], bounds[1])))
    rays.extend((start_x, start_y, x, y) for x, y in vertices)
    for x0, y0, x1, y1 in rays:
        candidates = los_candidates(x0, y0, x1, y1)
        rminx, rmaxx = min(x0, x1), max(x0, x1)
        rminy, rmaxy = min(y0, y1), max(y0, y1)
        for i, seg in enumerate(segs):
            ax, ay = vertices[seg["v1"]]
            bx, by = vertices[seg["v2"]]
            if (rmaxx >= min(ax, bx) and rminx <= max(ax, bx) and
                    rmaxy >= min(ay, by) and rminy <= max(ay, by) and
                    strict_intersection(x0, y0, x1, y1, ax, ay, bx, by) and
                    i not in candidates):
                raise SystemExit("blockmap LOS validation failed at segment %d ray=(%d,%d)-(%d,%d) seg=(%d,%d)-(%d,%d)" %
                                 (i, x0, y0, x1, y1, ax, ay, bx, by))

    return len(points) * 2 + len(rays)


def clean_name(raw):
    if isinstance(raw, bytes):
        raw = raw.split(b"\x00", 1)[0].decode("ascii", "ignore")
    return raw.upper().rstrip("\x00").rstrip()


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


def image_average(path):
    with Image.open(path) as image:
        pixels = list(image.convert("RGB").get_flattened_data())
    count = max(1, len(pixels))
    return tuple(sum(p[channel] for p in pixels) // count for channel in range(3))


def add_image_histogram(histogram, path, size, weight):
    with Image.open(path) as image:
        rgba = image.convert("RGBA").resize(size, Image.Resampling.BOX)
        for r, g, b, a in rgba.get_flattened_data():
            if a >= 128:
                histogram[(r, g, b)] += weight


def median_cut_colors(histogram, color_count):
    entries = [(color, weight) for color, weight in histogram.items() if weight > 0]
    boxes = [entries]
    while len(boxes) < color_count:
        split_index = -1
        split_channel = 0
        split_score = -1
        for index, box in enumerate(boxes):
            if len(box) < 2:
                continue
            ranges = [max(c[0][ch] for c in box) - min(c[0][ch] for c in box)
                      for ch in range(3)]
            channel = max(range(3), key=lambda ch: (ranges[ch], -ch))
            score = ranges[channel] * sum(item[1] for item in box)
            if score > split_score:
                split_index, split_channel, split_score = index, channel, score
        if split_index < 0:
            break
        box = sorted(boxes.pop(split_index), key=lambda item: (item[0][split_channel], item[0]))
        total = sum(item[1] for item in box)
        running = 0
        cut = 1
        for cut in range(1, len(box)):
            running += box[cut - 1][1]
            if running * 2 >= total:
                break
        boxes.append(box[:cut])
        boxes.append(box[cut:])

    colors = []
    for box in boxes:
        total = max(1, sum(item[1] for item in box))
        average = tuple(sum(item[0][ch] * item[1] for item in box) // total for ch in range(3))
        colors.append(md_color(average))
    return colors


def nearest_palette_index(rgb, palette, allowed=None):
    return world_palette.nearest_index(rgb, palette, allowed)


WEAPON_SPRITE_NAMES = ("PISGA0", "PISGB0", "PISFA0")


def build_world_palette(texture_names, texture_usage, sectors):
    histogram = Counter()
    for name in texture_names:
        weight = max(1, min(16, texture_usage.get(name, 1)))
        add_image_histogram(histogram, texture_path(name), (WALL_TEX_DIM, WALL_TEX_DIM), weight)

    # PAL3 is shared by the 3D scene, weapon and every runtime billboard. Feed
    # every consumed source into the same deterministic histogram so improving
    # walls cannot silently recolor actors or effects. The weapon sprites are
    # on screen for 100% of gameplay (unlike any single wall texture or actor),
    # so they get a much heavier weight than the rest of WORLD_SPRITE_INPUTS.
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


def convert_texture(path, palette):
    with Image.open(path) as image:
        sampled = sampled_texture_width(image.width)
        if sampled != image.width:
            image = image.crop((0, 0, sampled, image.height))
        resized = image.convert("RGB").resize((WALL_TEX_DIM, WALL_TEX_DIM), Image.Resampling.BOX)
        material_name = os.path.splitext(os.path.basename(path))[0].upper()
        if material_name.startswith(("GRAY", "METAL", "STONE")):
            allowed = [index for index in range(0, WORLD_COLOR_DAMAGE)
                       if world_palette.is_neutral(palette[index])]
        else:
            allowed = range(0, WORLD_COLOR_DAMAGE)
        cache = {}
        return [[world_palette.dither_index(resized.getpixel((x, y)), palette,
                                            x, y, False, cache,
                                            allowed)
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

    # The floor is a single ROM-constant color for the whole level (see
    # renderer_flats.c). Excluding that index from the ceiling search
    # guarantees a ceiling can never quantize to the exact same solid color as
    # the floor beneath it -- previously 65/85 E1M1 sectors did exactly that,
    # rendering as one flat gray from top to bottom with no visible seam.
    ceiling_allowed = [index for index in range(1, WORLD_COLOR_DAMAGE)
                       if index != fixed_floor_index]

    sector_visuals = []
    for sector in sectors:
        colors = []
        for field in ("ceiling_name", "floor_name"):
            if field == "floor_name":
                colors.extend((fixed_floor_index, fixed_floor_index, 0))
                continue
            name = sector[field]
            average = image_average(flat_path(name))
            if name != "F_SKY1":
                average = tuple(channel * sector["light"] // 255 for channel in average)
            first, second, coverage = world_palette.best_mix(
                average, palette, True, ceiling_allowed)
            # Damage and warning are runtime effects, not flat materials.
            if first >= WORLD_COLOR_DAMAGE or second >= WORLD_COLOR_DAMAGE:
                first = nearest_palette_index(average, palette, ceiling_allowed)
                second = first
                coverage = 0
            colors.extend((first, second, coverage))
        sector_visuals.append(tuple(colors))

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

# --- Minimal WAD reader (same layout as tools/wad-extract.py) --------------- #
WAD_HEADER_FMT = "<4sii"   # signature, numLumps, dirOffset
LUMP_ENTRY_FMT = "<ii8s"   # filepos, size, name
LUMP_ENTRY_SIZE = struct.calcsize(LUMP_ENTRY_FMT)


class Lump:
    __slots__ = ("name", "filepos", "size")

    def __init__(self, name, filepos, size):
        self.name = name
        self.filepos = filepos
        self.size = size


class WadFile:
    def __init__(self, path):
        with open(path, "rb") as fh:
            self.data = fh.read()
        sig, num, diroff = struct.unpack_from(WAD_HEADER_FMT, self.data, 0)
        sig = sig.decode("ascii", "ignore")
        if sig not in ("IWAD", "PWAD"):
            raise ValueError("Not a WAD file (signature=%r)." % sig)
        self.lumps = []
        for i in range(num):
            off = diroff + i * LUMP_ENTRY_SIZE
            filepos, size, raw = struct.unpack_from(LUMP_ENTRY_FMT, self.data, off)
            name = raw.split(b"\x00", 1)[0].decode("ascii", "ignore").upper()
            self.lumps.append(Lump(name, filepos, size))

    def lump_bytes(self, lump):
        return self.data[lump.filepos:lump.filepos + lump.size]

    def map_lump(self, map_name, member):
        """Return bytes of `member` belonging to the map marker `map_name`.

        Map member lumps immediately follow the marker in directory order.
        """
        for i, lump in enumerate(self.lumps):
            if lump.name == map_name:
                for j in range(i + 1, min(i + 12, len(self.lumps))):
                    if self.lumps[j].name == member:
                        return self.lump_bytes(self.lumps[j])
                    # Stop if we hit the next map marker.
                    if _is_map_marker(self.lumps[j].name):
                        break
                return None
        return None


def _is_map_marker(name):
    if len(name) == 4 and name[0] == "E" and name[2] == "M":
        return name[1].isdigit() and name[3].isdigit()
    return name.startswith("MAP") and name[3:].isdigit()


# Flat runtime contract (must match src/bsp/bsp_map.h / billboard_internal.h).
SEG_WALL = 0
SEG_DOOR = 1
SEG_EXIT = 2
SEG_SWITCH = 3
SEG_TRIGGER = 4
SEG_FLAG_DIRECT_USE = 0x01

KEY_NONE = 0
KEY_BLUE = 1
KEY_YELLOW = 2
KEY_RED = 4
DOOR_GROUP_NONE = 0xFF
MAX_DOOR_GROUPS = 64
# Doom's collision diameter is 32 map units; the radius is therefore 16.  Keep
# this synchronized with PLAYER_COLLISION_RADIUS in src/raycast.h.
PLAYER_RADIUS = 16
NAV_STEP = 16
PICKUP_RADIUS = 128
USE_RADIUS = 256
BILLBOARD_OBJECT_COUNT = 112
DOOM_THING_SKILL_MEDIUM = 0x0002
DOOM_THING_NOT_SINGLE_PLAYER = 0x0010

DIRECT_DOOR_SPECIALS = {1, 26, 27, 28, 31, 32, 33, 34, 117, 118}
REMOTE_DOOR_SPECIALS = {2, 3, 4, 16, 29, 42, 46, 61, 63, 75, 76, 86, 90, 103}
DOOR_SPECIALS = DIRECT_DOOR_SPECIALS | REMOTE_DOOR_SPECIALS
LOCKED_DOOR_KEYS = {
    26: KEY_BLUE, 32: KEY_BLUE,
    27: KEY_YELLOW, 34: KEY_YELLOW,
    28: KEY_RED, 33: KEY_RED,
}
EXIT_SPECIALS = {11, 51}
TELEPORT_SPECIALS = {39, 97, 125, 126}
BOSS_TRIGGER_MAPS = {"E1M8", "E2M8", "E3M8", "E4M6", "E4M8"}

LINE_FLAG_IMPASSABLE = 0x0001

RUNTIME_THING_TYPES = {5, 6, 9, 13, 2007, 2011, 2012, 2014, 2015,
                       2018, 2019, 2035, 2048, 3001, 3004}
KEY_THING_MASK = {5: KEY_BLUE, 6: KEY_YELLOW, 13: KEY_RED}
BLOCKING_THING_RADIUS = {2035: 20}


def point_segment_dist2(ax, ay, bx, by, px, py):
    abx = bx - ax
    aby = by - ay
    apx = px - ax
    apy = py - ay
    ab2 = abx * abx + aby * aby
    if ab2 <= 0:
        cx, cy = ax, ay
    else:
        dot = apx * abx + apy * aby
        if dot <= 0:
            cx, cy = ax, ay
        elif dot >= ab2:
            cx, cy = bx, by
        else:
            cx = ax + (abx * dot) // ab2
            cy = ay + (aby * dot) // ab2
    dx = px - cx
    dy = py - cy
    return dx * dx + dy * dy


def runtime_things(things):
    result = []
    for thing in things:
        x, y, thing_type, angle, flags = thing
        if not (flags & DOOM_THING_SKILL_MEDIUM) or flags & DOOM_THING_NOT_SINGLE_PLAYER:
            continue
        if thing_type not in RUNTIME_THING_TYPES:
            continue
        if len(result) >= BILLBOARD_OBJECT_COUNT:
            break
        result.append(thing)
    return result


def certify_flat_progression(vertices, segs, things, start_x, start_y):
    """Prove a concrete medium/single-player route through the emitted flat map."""
    exits = [(index, seg) for index, seg in enumerate(segs)
             if seg["type"] == SEG_EXIT]
    if not exits:
        raise ValueError("no supported exit linedef (special 11/51) survives flattening")

    active_things = runtime_things(things)
    source_key_mask = KEY_NONE
    medium_key_mask = KEY_NONE
    for _, _, thing_type, _, flags in things:
        key_bit = KEY_THING_MASK.get(thing_type, KEY_NONE)
        source_key_mask |= key_bit
        if ((flags & DOOM_THING_SKILL_MEDIUM) and
                not (flags & DOOM_THING_NOT_SINGLE_PLAYER)):
            medium_key_mask |= key_bit
    keys = [(x, y, KEY_THING_MASK[thing_type])
            for x, y, thing_type, _, _ in active_things
            if thing_type in KEY_THING_MASK]
    blockers = [(x, y, BLOCKING_THING_RADIUS[thing_type])
                for x, y, thing_type, _, _ in active_things
                if thing_type in BLOCKING_THING_RADIUS]
    available_key_mask = KEY_NONE
    for _, _, key_bit in keys:
        available_key_mask |= key_bit
    interactions = [seg for seg in segs
                    if seg["type"] in (SEG_SWITCH, SEG_TRIGGER) or
                    (seg["type"] == SEG_DOOR and
                     seg.get("flags", 0) & SEG_FLAG_DIRECT_USE)]

    min_x = (min(x for x, _ in vertices) // NAV_STEP) * NAV_STEP
    min_y = (min(y for _, y in vertices) // NAV_STEP) * NAV_STEP
    max_x = ((max(x for x, _ in vertices) + NAV_STEP - 1) // NAV_STEP) * NAV_STEP
    max_y = ((max(y for _, y in vertices) + NAV_STEP - 1) // NAV_STEP) * NAV_STEP
    width = ((max_x - min_x) // NAV_STEP) + 1
    height = ((max_y - min_y) // NAV_STEP) + 1

    broad_cell = 256
    broad_w = ((max_x - min_x) // broad_cell) + 1
    broad_h = ((max_y - min_y) // broad_cell) + 1
    broad = [[] for _ in range(broad_w * broad_h)]
    for index, seg in enumerate(segs):
        ax, ay = vertices[seg["v1"]]
        bx, by = vertices[seg["v2"]]
        cx0 = max(0, (min(ax, bx) - PLAYER_RADIUS - min_x) // broad_cell)
        cx1 = min(broad_w - 1, (max(ax, bx) + PLAYER_RADIUS - min_x) // broad_cell)
        cy0 = max(0, (min(ay, by) - PLAYER_RADIUS - min_y) // broad_cell)
        cy1 = min(broad_h - 1, (max(ay, by) + PLAYER_RADIUS - min_y) // broad_cell)
        for cy in range(cy0, cy1 + 1):
            for cx in range(cx0, cx1 + 1):
                broad[cy * broad_w + cx].append(index)

    grid_collision_state = bytearray(width * height)  # 0 unknown, 1 clear, 2 static wall
    grid_door_groups = array("Q", [0]) * (width * height)
    group_keys = {}
    for seg in segs:
        if seg["type"] == SEG_DOOR:
            group_keys[seg["door_group"]] = seg["required_key"]

    def collision_components(px, py):
        if px < min_x or px > max_x or py < min_y or py > max_y:
            return True, 0
        cx = min(broad_w - 1, max(0, (px - min_x) // broad_cell))
        cy = min(broad_h - 1, max(0, (py - min_y) // broad_cell))
        door_groups = 0
        for index in broad[cy * broad_w + cx]:
            seg = segs[index]
            if seg["type"] == SEG_TRIGGER:
                continue
            ax, ay = vertices[seg["v1"]]
            bx, by = vertices[seg["v2"]]
            if point_segment_dist2(ax, ay, bx, by, px, py) < PLAYER_RADIUS ** 2:
                if seg["type"] == SEG_DOOR:
                    door_groups |= 1 << seg["door_group"]
                else:
                    return True, door_groups
        for ox, oy, radius in blockers:
            dx = px - ox
            dy = py - oy
            if dx * dx + dy * dy < (PLAYER_RADIUS + radius) ** 2:
                return True, door_groups
        return False, door_groups

    def blocked(px, py, key_mask, opened_groups):
        aligned = ((px - min_x) % NAV_STEP == 0 and
                   (py - min_y) % NAV_STEP == 0 and
                   min_x <= px <= max_x and min_y <= py <= max_y)
        if aligned:
            index = ((py - min_y) // NAV_STEP) * width + \
                ((px - min_x) // NAV_STEP)
            if grid_collision_state[index] == 0:
                static, door_groups = collision_components(px, py)
                grid_collision_state[index] = 2 if static else 1
                grid_door_groups[index] = door_groups
            if grid_collision_state[index] == 2:
                return True
            door_groups = grid_door_groups[index]
        else:
            static, door_groups = collision_components(px, py)
            if static:
                return True

        if not door_groups:
            return False
        key_allowed = 0
        for group, required in group_keys.items():
            if required == KEY_NONE or key_mask & required == required:
                key_allowed |= 1 << group
        allowed = opened_groups & key_allowed
        return bool(door_groups & ~allowed)

    def exit_reached(px, py):
        for exit_index, seg in exits:
            ax, ay = vertices[seg["v1"]]
            bx, by = vertices[seg["v2"]]
            if point_segment_dist2(ax, ay, bx, by, px, py) <= USE_RADIUS ** 2:
                return exit_index
        return None

    seed_x = min(max_x, max(min_x, round((start_x - min_x) / NAV_STEP) * NAV_STEP + min_x))
    seed_y = min(max_y, max(min_y, round((start_y - min_y) / NAV_STEP) * NAV_STEP + min_y))
    if (blocked(start_x, start_y, KEY_NONE, 0) or
            blocked(seed_x, seed_y, KEY_NONE, 0)):
        raise ValueError("player start is blocked in emitted flat geometry")

    cell_count = width * height
    visited = [bytearray(cell_count) for _ in range(8)]
    visited_counts = [0] * 8
    opened_by_key = [0] * 8
    queue = array("I")
    queue_head = 0
    epochs = [0] * 8

    def queue_entry(index, key_mask):
        return ((epochs[key_mask] * 8 + key_mask) * cell_count) + index

    seed = ((seed_y - min_y) // NAV_STEP) * width + ((seed_x - min_x) // NAV_STEP)
    visited[KEY_NONE][seed] = 1
    visited_counts[KEY_NONE] = 1
    queue.append(queue_entry(seed, KEY_NONE))
    reached_masks = set()
    reached_open_groups = 0
    directions = ((NAV_STEP, 0), (-NAV_STEP, 0), (0, NAV_STEP), (0, -NAV_STEP),
                  (NAV_STEP, NAV_STEP), (NAV_STEP, -NAV_STEP),
                  (-NAV_STEP, NAV_STEP), (-NAV_STEP, -NAV_STEP))

    while queue_head < len(queue):
        encoded = queue[queue_head]
        queue_head += 1
        index = encoded % cell_count
        state = encoded // cell_count
        key_mask = state & 7
        if (state >> 3) != epochs[key_mask]:
            continue
        px = min_x + (index % width) * NAV_STEP
        py = min_y + (index // width) * NAV_STEP
        opened_groups = opened_by_key[key_mask]
        reached_masks.add(key_mask)
        reached_open_groups |= opened_groups
        exit_index = exit_reached(px, py)
        if exit_index is not None:
            return {
                "reachable": True,
                "key_mask": key_mask,
                "available_keys": available_key_mask,
                "reached_masks": sorted(reached_masks),
                "opened_groups": opened_groups,
                "exit_index": exit_index,
                "states": sum(visited_counts),
            }

        collected = key_mask
        for key_x, key_y, key_bit in keys:
            dx = px - key_x
            dy = py - key_y
            if dx * dx + dy * dy <= PICKUP_RADIUS ** 2:
                collected |= key_bit
        if collected != key_mask:
            index = ((py - min_y) // NAV_STEP) * width + ((px - min_x) // NAV_STEP)
            groups_changed = ((opened_by_key[collected] | opened_groups) !=
                              opened_by_key[collected])
            opened_by_key[collected] |= opened_groups
            if groups_changed:
                # More-open geometry is monotonic and movement is reversible.
                # Restart this mask from the concrete current position instead
                # of duplicating its entire reached component in the queue.
                epochs[collected] += 1
                visited[collected] = bytearray(cell_count)
                visited_counts[collected] = 0
            if not visited[collected][index]:
                visited[collected][index] = 1
                visited_counts[collected] += 1
                queue.append(queue_entry(index, collected))

        # Remote switches are monotonic for the proof: opening a group can only
        # add routes, and the runtime can reproduce the same sequence with use.
        newly_opened = opened_groups
        for interaction in interactions:
            required = interaction["required_key"]
            if required != KEY_NONE and key_mask & required != required:
                continue
            ax, ay = vertices[interaction["v1"]]
            bx, by = vertices[interaction["v2"]]
            if point_segment_dist2(ax, ay, bx, by, px, py) <= USE_RADIUS ** 2:
                newly_opened |= 1 << interaction["door_group"]
        if newly_opened != opened_groups:
            opened_by_key[key_mask] = newly_opened
            reached_open_groups |= newly_opened
            # Opening only removes collision. The current position remains a
            # valid seed for the whole previously reached undirected component,
            # so restart this mask compactly instead of enqueuing every cell.
            epochs[key_mask] += 1
            current_index = index
            visited[key_mask] = bytearray(cell_count)
            visited[key_mask][current_index] = 1
            visited_counts[key_mask] = 1
            queue.append(queue_entry(current_index, key_mask))
            opened_groups = newly_opened

        for dx, dy in directions:
            nx, ny = px + dx, py + dy
            if blocked(nx, ny, key_mask, opened_groups):
                continue
            if dx and dy:
                if (blocked(px + dx, py, key_mask, opened_groups) or
                        blocked(px, py + dy, key_mask, opened_groups)):
                    continue
            index = ((ny - min_y) // NAV_STEP) * width + ((nx - min_x) // NAV_STEP)
            if visited[key_mask][index]:
                continue
            visited[key_mask][index] = 1
            visited_counts[key_mask] += 1
            queue.append(queue_entry(index, key_mask))

        # Drop processed storage periodically; array('I') keeps the frontier at
        # four bytes per cell instead of Python tuple/object overhead.
        if queue_head >= 262144 and queue_head * 2 >= len(queue):
            queue = array("I", queue[queue_head:])
            queue_head = 0

    required = 0
    for seg in segs:
        required |= seg.get("required_key", KEY_NONE)
    missing = required & ~available_key_mask
    detail = "exit unreachable after %d navigation states; reached key masks=%s" % (
        sum(visited_counts), sorted(reached_masks))
    if reached_open_groups:
        detail += "; opened door groups=0x%X" % reached_open_groups
    if missing:
        detail += "; missing required key mask=0x%02X" % missing
        filtered = missing & source_key_mask & ~available_key_mask
        if filtered:
            reasons = []
            if filtered & ~medium_key_mask:
                reasons.append("skill/single-player filter")
            if filtered & medium_key_mask:
                reasons.append("runtime object cap")
            detail += " (discarded by %s)" % " and ".join(reasons)
    raise ValueError(detail)


def reduce_normal(nx, ny):
    g = math.gcd(abs(nx), abs(ny))
    if g > 1:
        nx //= g
        ny //= g
    return nx, ny


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wad", default=DEFAULT_WAD)
    ap.add_argument("--map", default="E1M1")
    ap.add_argument("--out", default=None)
    ap.add_argument("--assets-out", default=DEFAULT_ASSET_OUT)
    ap.add_argument("--certify-only", action="store_true",
                    help="parse/classify/certify and report without emitting files")
    args = ap.parse_args()

    wad = WadFile(args.wad)
    mapn = args.map.upper()
    out_path = args.out or os.path.join(
        PROJECT_ROOT, "src", "generated_%s_map.c" % mapn.lower()
    )

    def need(member):
        data = wad.map_lump(mapn, member)
        if data is None:
            raise SystemExit("Map %s: lump %s not found" % (mapn, member))
        return data

    verts_raw = need("VERTEXES")
    lines_raw = need("LINEDEFS")
    sides_raw = need("SIDEDEFS")
    sectors_raw = need("SECTORS")
    segs_raw = need("SEGS")
    ssectors_raw = need("SSECTORS")
    nodes_raw = need("NODES")
    things_raw = need("THINGS")

    # Doom is y-up; this engine's proven (hand-map) convention is y-down, so we
    # negate Y everywhere (vertices, node partition Y/dY, player start Y/angle)
    # and swap node front/back children to compensate the flipped partition-side
    # test. This keeps E1M1 in the exact convention the renderer was validated
    # against (not mirrored, turning correct).
    verts_up = [struct.unpack_from("<hh", verts_raw, i * 4)
                for i in range(len(verts_raw) // 4)]
    vertices = [(x, -y) for (x, y) in verts_up]

    sectors = []
    for i in range(len(sectors_raw) // 26):
        floor_h, ceiling_h, floor_name, ceiling_name, light, special, tag = struct.unpack_from(
            "<hh8s8shhh", sectors_raw, i * 26)
        sectors.append(dict(
            floor=floor_h,
            ceiling=ceiling_h,
            floor_name=clean_name(floor_name),
            ceiling_name=clean_name(ceiling_name),
            light=max(0, min(255, light)), special=special, tag=tag,
        ))

    sidedefs = []
    for i in range(len(sides_raw) // 30):
        xoff, yoff, up, lo, mid, sec = struct.unpack_from(
            "<hh8s8s8sH", sides_raw, i * 30)
        sidedefs.append(dict(xoff=xoff, yoff=yoff, upper=clean_name(up),
                             lower=clean_name(lo), middle=clean_name(mid), sector=sec))

    linedefs = []
    for i in range(len(lines_raw) // 14):
        v1, v2, flags, special, tag, right, left = struct.unpack_from(
            "<HHHHHHH", lines_raw, i * 14)
        linedefs.append(dict(v1=v1, v2=v2, flags=flags, special=special,
                             tag=tag, right=right, left=left))

    segs = []
    for i in range(len(segs_raw) // 12):
        v1, v2, angle, ld, direction, offset = struct.unpack_from(
            "<HHhHHh", segs_raw, i * 12)
        segs.append(dict(v1=v1, v2=v2, ld=ld, direction=direction, offset=offset))

    ssectors = []
    for i in range(len(ssectors_raw) // 4):
        count, first = struct.unpack_from("<HH", ssectors_raw, i * 4)
        ssectors.append((count, first))

    nodes = []

    def flip_bbox(raw):
        """Convert Doom (top,bottom,left,right) y-up bbox to engine y-down."""
        top, bottom, left, right = raw
        xs = (left, right)
        ys = (-top, -bottom)
        return (min(xs), min(ys), max(xs), max(ys))

    for i in range(len(nodes_raw) // 28):
        vals = struct.unpack_from("<hhhhhhhhhhhhHH", nodes_raw, i * 28)
        x, y, dx, dy = vals[0], vals[1], vals[2], vals[3]
        right_box = flip_bbox(vals[4:8])
        left_box = flip_bbox(vals[8:12])
        right_child, left_child = vals[12], vals[13]
        # Y-down flip: negate y and dy, and swap children (front=left, back=right)
        # so render_node's `cross >= 0 -> front` still selects Doom's right side.
        # Boxes follow their children through the same swap.
        nodes.append(dict(x=x, y=-y, dx=dx, dy=-dy,
                          front_box=left_box, back_box=right_box,
                          front=left_child, back=right_child))

    def line_sector_ids(ld):
        result = []
        for side_id in (ld["right"], ld["left"]):
            if side_id != 0xFFFF:
                result.append(sidedefs[side_id]["sector"])
        return result

    def line_opening(ld):
        sector_ids = line_sector_ids(ld)
        if len(sector_ids) != 2:
            return None
        first, second = (sectors[index] for index in sector_ids)
        return min(first["ceiling"], second["ceiling"]) - \
            max(first["floor"], second["floor"])

    # Door actions identify physical door sectors before heights are discarded.
    # All sectors targeted by one remote tag deliberately share one runtime
    # group: Doom opens them together, so every emitted face must change state
    # together as well. Direct doors use the narrow/closed adjacent sector.
    remote_tags = sorted({ld["tag"] for ld in linedefs
                          if ld["special"] in REMOTE_DOOR_SPECIALS and ld["tag"]})
    sector_door_group = {}
    remote_tag_group = {}
    next_door_group = 0
    for tag in remote_tags:
        tagged = [index for index, sector in enumerate(sectors)
                  if sector["tag"] == tag]
        existing = sorted({sector_door_group[index] for index in tagged
                           if index in sector_door_group})
        group = existing[0] if existing else next_door_group
        if not existing:
            next_door_group += 1
        remote_tag_group[tag] = group
        for sector_id in tagged:
            sector_door_group[sector_id] = group

    direct_line_sector = {}
    for line_id, ld in enumerate(linedefs):
        if ld["special"] not in DIRECT_DOOR_SPECIALS:
            continue
        candidates = line_sector_ids(ld)
        if not candidates:
            continue
        sector_id = min(candidates, key=lambda index:
            (sectors[index]["ceiling"] - sectors[index]["floor"], index))
        direct_line_sector[line_id] = sector_id
        if sector_id not in sector_door_group:
            sector_door_group[sector_id] = next_door_group
            next_door_group += 1

    line_door_group = {}
    for line_id, ld in enumerate(linedefs):
        candidates = [sector_id for sector_id in line_sector_ids(ld)
                      if sector_id in sector_door_group]
        group = None
        if candidates and (line_opening(ld) is not None and line_opening(ld) <= 0):
            sector_id = min(candidates, key=lambda index:
                (sectors[index]["ceiling"] - sectors[index]["floor"], index))
            group = sector_door_group[sector_id]
        elif line_id in direct_line_sector:
            group = sector_door_group[direct_line_sector[line_id]]
        if group is not None:
            line_door_group[line_id] = group

    remote_line_group = {
        line_id: remote_tag_group[ld["tag"]]
        for line_id, ld in enumerate(linedefs)
        if ld["special"] in REMOTE_DOOR_SPECIALS and ld["tag"] in remote_tag_group
    }

    if next_door_group > MAX_DOOR_GROUPS:
        raise SystemExit("door group count %d exceeds BSP_MAX_DOORS (%d)" %
                         (next_door_group, MAX_DOOR_GROUPS))

    group_required_key = [KEY_NONE] * next_door_group
    for line_id, group in line_door_group.items():
        group_required_key[group] |= LOCKED_DOOR_KEYS.get(
            linedefs[line_id]["special"], KEY_NONE)

    # --- Classify each linedef in the flattened world. ---------------------- #
    def line_solid(line_id, ld):
        if ld["left"] == 0xFFFF:
            return True
        if ld["flags"] & LINE_FLAG_IMPASSABLE:
            return True
        if (ld["special"] in EXIT_SPECIALS or line_id in line_door_group or
                line_id in remote_line_group):
            return True
        # Every remaining two-sided height transition becomes an open gap. This
        # deliberately removes stairs, lifts, ledges and closed height tricks.
        return False

    def front_side_for(seg):
        ld = linedefs[seg["ld"]]
        front_side = ld["right"] if seg["direction"] == 0 else ld["left"]
        if front_side == 0xFFFF:
            front_side = ld["right"]
        return front_side

    def back_side_for(seg):
        ld = linedefs[seg["ld"]]
        return ld["left"] if seg["direction"] == 0 else ld["right"]

    def front_normal(seg):
        """Normal pointing into the SEG front sector in engine y-down space."""
        ld = linedefs[seg["ld"]]
        lv1 = verts_up[ld["v1"]]
        lv2 = verts_up[ld["v2"]]
        lx = lv2[0] - lv1[0]
        ly = lv2[1] - lv1[1]
        if seg["direction"] == 0:
            nux, nuy = ly, -lx
        else:
            nux, nuy = -ly, lx
        return reduce_normal(nux, -nuy)

    def seg_type_and_visual(seg):
        ld = linedefs[seg["ld"]]
        front_side = front_side_for(seg)
        back_side = back_side_for(seg)
        seg_type = SEG_WALL
        door_group = DOOR_GROUP_NONE
        required_key = KEY_NONE
        flags = 0
        if ld["special"] in EXIT_SPECIALS:
            seg_type = SEG_EXIT
        elif seg["ld"] in remote_line_group:
            structurally_solid = (ld["left"] == 0xFFFF or
                                  bool(ld["flags"] & LINE_FLAG_IMPASSABLE))
            seg_type = SEG_SWITCH if structurally_solid else SEG_TRIGGER
            door_group = remote_line_group[seg["ld"]]
            required_key = group_required_key[door_group]
        elif seg["ld"] in line_door_group:
            seg_type = SEG_DOOR
            door_group = line_door_group[seg["ld"]]
            required_key = group_required_key[door_group]
            if ld["special"] in DIRECT_DOOR_SPECIALS:
                flags |= SEG_FLAG_DIRECT_USE

        name = FALLBACK_TEXTURE
        xoff = seg["offset"]
        yoff = 0
        if front_side != 0xFFFF:
            side = sidedefs[front_side]
            xoff += side["xoff"]
            yoff = side["yoff"]
            side_ids = [front_side]
            if back_side != 0xFFFF:
                side_ids.append(back_side)
            fields = ("middle", "upper", "lower") if seg_type in (SEG_DOOR, SEG_SWITCH) \
                else ("middle", "lower", "upper")
            for side_id in side_ids:
                candidate_side = sidedefs[side_id]
                for field in fields:
                    candidate = candidate_side[field]
                    if candidate and candidate != "-":
                        name = candidate
                        break
                if name != FALLBACK_TEXTURE:
                    break
        return seg_type, name, xoff, yoff, door_group, required_key, flags

    # --- Rebuild subsectors with only flat solid/interactive segs. ---------- #
    out_segs = []       # dicts with geometry, exact texture name, offsets and type
    out_ssectors = []   # (first_seg, count)
    out_ssector_sectors = []
    texture_usage = Counter()

    for (count, first) in ssectors:
        source_seg = segs[first] if count else None
        source_side = front_side_for(source_seg) if source_seg is not None else 0xFFFF
        sector_id = sidedefs[source_side]["sector"] if source_side != 0xFFFF else 0
        out_ssector_sectors.append(sector_id)
        start = len(out_segs)
        for k in range(count):
            seg = segs[first + k]
            if not line_solid(seg["ld"], linedefs[seg["ld"]]):
                continue
            ax, ay = vertices[seg["v1"]]
            bx, by = vertices[seg["v2"]]
            if ax == bx and ay == by:
                continue  # degenerate seg
            nx, ny = front_normal(seg)
            if nx == 0 and ny == 0:
                continue  # degenerate linedef
            stype, texture_name, tex_u_offset, tex_v_offset, door_group, required_key, flags = \
                seg_type_and_visual(seg)
            texture_usage[texture_name] += 1
            out_segs.append(dict(v1=seg["v1"], v2=seg["v2"],
                                nx=nx, ny=ny, texture_name=texture_name,
                                tex_u_offset=tex_u_offset, tex_v_offset=tex_v_offset,
                                type=stype, door_group=door_group,
                                required_key=required_key, flags=flags))
        out_ssectors.append((start, len(out_segs) - start))

    if len(out_segs) > 2048:
        raise SystemExit("solid seg count %d exceeds BSP_MAX_SEGS (2048)"
                         % len(out_segs))
    if len(nodes) > 640:
        raise SystemExit("node count %d exceeds BSP_MAX_NODES (640)" % len(nodes))

    # --- THINGS / Player 1 start. ------------------------------------------- #
    # Convert every THING into engine y-down coordinates.  The runtime owns the
    # deliberately-curated mapping to billboards/items, so unsupported types can
    # be reported and ignored without throwing away source-map fidelity.
    out_things = []
    start_x = start_y = 0
    start_angle_deg = 0
    for i in range(len(things_raw) // 10):
        tx, ty, tang, ttype, tflags = struct.unpack_from("<hhHHH", things_raw, i * 10)
        out_things.append((tx, -ty, ttype, (256 - (round(tang * 256 / 360) & 255)) & 255, tflags))
        if ttype == 1 and start_x == 0 and start_y == 0:
            start_x, start_y, start_angle_deg = tx, ty, tang
    # Y-down flip: negate the start Y and mirror the angle about the x-axis.
    start_y = -start_y
    start_angle = (256 - (round(start_angle_deg * 256 / 360) & 255)) & 255
    supported_things = sum(1 for _, _, thing_type, _, _ in out_things
                           if thing_type in RUNTIME_THING_TYPES)

    try:
        certificate = certify_flat_progression(
            vertices, out_segs, out_things, start_x, start_y)
    except ValueError as error:
        unsupported = sorted({ld["special"] for ld in linedefs
                              if ld["special"] in TELEPORT_SPECIALS})
        mechanics = []
        if unsupported:
            mechanics.append("teleport specials %s" % unsupported)
        if mapn in BOSS_TRIGGER_MAPS:
            mechanics.append("boss-death trigger")
        suffix = "; unsupported mandatory mechanics present: %s" % \
            ", ".join(mechanics) if mechanics else ""
        raise SystemExit("Map %s cannot be certified: %s%s" %
                         (mapn, error, suffix))

    # Resolve any blank face from another face in the same physical door group.
    group_textures = {}
    for seg in out_segs:
        if seg["type"] == SEG_DOOR and seg["texture_name"] != FALLBACK_TEXTURE:
            group_textures.setdefault(seg["door_group"], seg["texture_name"])
    for seg in out_segs:
        if seg["type"] == SEG_DOOR and seg["texture_name"] == FALLBACK_TEXTURE:
            replacement = group_textures.get(seg["door_group"])
            if replacement:
                texture_usage[FALLBACK_TEXTURE] -= 1
                texture_usage[replacement] += 1
                seg["texture_name"] = replacement

    required_key_mask = KEY_NONE
    for key_mask in group_required_key:
        required_key_mask |= key_mask
    door_face_counts = Counter(seg["door_group"] for seg in out_segs
                               if seg["type"] == SEG_DOOR)
    fallback_door_faces = sum(1 for seg in out_segs
                              if seg["type"] == SEG_DOOR and
                              seg["texture_name"] == FALLBACK_TEXTURE)

    if args.certify_only:
        print("Map %s flat progression certified" % mapn)
        print("  doors    : %d groups, faces=%s" %
              (next_door_group, dict(sorted(door_face_counts.items()))))
        print("  keys     : available=0x%02X required=0x%02X reached=%s" %
              (certificate["available_keys"], required_key_mask,
               certificate["reached_masks"]))
        print("  exit     : seg %d with key mask=0x%02X after %d states" %
              (certificate["exit_index"], certificate["key_mask"],
               certificate["states"]))
        print("  textures : %d door faces still require fallback" %
              fallback_door_faces)
        return

    asset_temp = args.assets_out + ".tmp"
    map_temp = out_path + ".tmp"
    for temp_path in (asset_temp, map_temp):
        if os.path.exists(temp_path):
            os.remove(temp_path)
    texture_ids, texture_meta, sector_visuals, palette = emit_world_assets(
        asset_temp, texture_usage, sectors)
    for seg in out_segs:
        name = seg["texture_name"]
        if not -32768 <= seg["tex_u_offset"] <= 32767:
            raise SystemExit("texture U offset out of s16 range: %d" % seg["tex_u_offset"])
        seg["tex"] = texture_ids[name]
        seg["tex_v"] = ((seg["tex_v_offset"] * WALL_TEX_DIM) //
                        texture_meta[name]["height"]) & (WALL_TEX_DIM - 1)

    root = (len(nodes) - 1) if nodes else 0x8000

    # --- Emit C. ------------------------------------------------------------ #
    ts = "source-derived"
    lines = []
    lines.append("// Generated by tools/wad-map-extract.py")
    lines.append("// Source: %s  map: %s" % (os.path.basename(args.wad), mapn))
    lines.append("// Generated at: %s" % ts)
    lines.append("#include \"bsp_map.h\"")
    lines.append("")
    lines.append("#if !BSP_USE_HAND_MAP")
    lines.append("")

    # Vertices (full original array; segs/nodes reference these indices).
    lines.append("const BspVertex bsp_vertices[%d] = {" % len(vertices))
    for (x, y) in vertices:
        lines.append("    {%d, %d}," % (x, y))
    lines.append("};")
    lines.append("")

    lines.append("const BspSeg bsp_segs[%d] = {" % len(out_segs))
    for s in out_segs:
        lines.append("    {%d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d}," % (
            s["v1"], s["v2"], s["nx"], s["ny"], s["tex_u_offset"],
            s["tex_v"], s["tex"], s["type"], s["door_group"], s["required_key"],
            s["flags"]))
    lines.append("};")
    lines.append("")

    # Compact parallel ROM table: precomputed wall length per seg (|bx-ax| +
    # |by-ay|). Camera-independent, stored in ROM to avoid two vertex lookups
    # and two abs calls per seg visit in the renderer hot path.
    lines.append("const u16 bsp_seg_wall_len[%d] = {" % len(out_segs))
    for i, s in enumerate(out_segs):
        ax, ay = vertices[s["v1"]]
        bx, by = vertices[s["v2"]]
        wl = abs(bx - ax) + abs(by - ay)
        if i % 12 == 0:
            # Start a new visual row. The previous row needs a trailing comma
            # so consecutive rows stay valid C (adjacent numeric tokens like
            # "128\n    128" would otherwise be a syntax error). The very first
            # row (i == 0) has no predecessor to terminate.
            if i > 0:
                lines[-1] += ","
            lines.append("    %d" % wl)
        else:
            lines[-1] += ",%d" % wl
    lines[-1] += ","
    lines.append("};")
    lines.append("")

    # Uniform 256-unit blockmap used by runtime collision and LOS.  A segment is
    # referenced by every cell touched by its AABB; the exact segment tests remain
    # authoritative, so this is a conservative broad phase rather than a geometry
    # approximation.  All arrays are const and therefore live in cartridge ROM.
    grid_cell = 256
    grid_min_x = (min(x for x, _ in vertices) // grid_cell) * grid_cell
    grid_min_y = (min(y for _, y in vertices) // grid_cell) * grid_cell
    grid_max_x = (max(x for x, _ in vertices) // grid_cell) * grid_cell
    grid_max_y = (max(y for _, y in vertices) // grid_cell) * grid_cell
    grid_w = ((grid_max_x - grid_min_x) // grid_cell) + 1
    grid_h = ((grid_max_y - grid_min_y) // grid_cell) + 1
    grid_cells = [[] for _ in range(grid_w * grid_h)]
    for seg_index, s in enumerate(out_segs):
        ax, ay = vertices[s["v1"]]
        bx, by = vertices[s["v2"]]
        cx0 = (min(ax, bx) - grid_min_x) // grid_cell
        cx1 = (max(ax, bx) - grid_min_x) // grid_cell
        cy0 = (min(ay, by) - grid_min_y) // grid_cell
        cy1 = (max(ay, by) - grid_min_y) // grid_cell
        for cy in range(cy0, cy1 + 1):
            for cx in range(cx0, cx1 + 1):
                grid_cells[cy * grid_w + cx].append(seg_index)

    grid_offsets = [0]
    grid_indices = []
    for cell in grid_cells:
        grid_indices.extend(cell)
        grid_offsets.append(len(grid_indices))

    spatial_checks = validate_spatial_grid(
        vertices, out_segs, grid_min_x, grid_min_y, grid_w, grid_h,
        grid_cell, grid_cells, start_x, start_y)

    lines.append("const s16 bsp_grid_min_x = %d;" % grid_min_x)
    lines.append("const s16 bsp_grid_min_y = %d;" % grid_min_y)
    lines.append("const u16 bsp_grid_width = %du;" % grid_w)
    lines.append("const u16 bsp_grid_height = %du;" % grid_h)
    lines.append("const u16 bsp_grid_cell_offsets[%d] = {" % len(grid_offsets))
    for i in range(0, len(grid_offsets), 12):
        lines.append("    %s," % ",".join(str(v) for v in grid_offsets[i:i + 12]))
    lines.append("};")
    lines.append("const u16 bsp_grid_seg_indices[%d] = {" % len(grid_indices))
    for i in range(0, len(grid_indices), 12):
        lines.append("    %s," % ",".join(str(v) for v in grid_indices[i:i + 12]))
    lines.append("};")
    lines.append("const s16 bsp_map_min_x = %d;" % min(x for x, _ in vertices))
    lines.append("const s16 bsp_map_min_y = %d;" % min(y for _, y in vertices))
    lines.append("const s16 bsp_map_max_x = %d;" % max(x for x, _ in vertices))
    lines.append("const s16 bsp_map_max_y = %d;" % max(y for _, y in vertices))
    lines.append("")

    lines.append("const BspSubsector bsp_subsectors[%d] = {" % len(out_ssectors))
    for first, count in out_ssectors:
        lines.append("    {%d, %d}," % (first, count))
    lines.append("};")
    lines.append("const u16 bsp_subsector_sector[%d] = {" % len(out_ssector_sectors))
    for i in range(0, len(out_ssector_sectors), 16):
        lines.append("    %s," % ",".join(
            str(value) for value in out_ssector_sectors[i:i + 16]))
    lines.append("};")
    lines.append("")

    lines.append("const BspNode bsp_nodes[%d] = {" % len(nodes))
    for nd in nodes:
        lines.append("    {%d, %d, %d, %d, {%d, %d, %d, %d}, {%d, %d, %d, %d}, %du, %du}," % (
            nd["x"], nd["y"], nd["dx"], nd["dy"],
            *nd["front_box"], *nd["back_box"], nd["front"], nd["back"]))
    lines.append("};")
    lines.append("")

    lines.append("const u16 bsp_root_node = %du;" % root)
    lines.append("const u16 bsp_seg_count = %du;" % len(out_segs))
    lines.append("const u16 bsp_vertex_count = %du;" % len(vertices))
    lines.append("const u16 bsp_subsector_count = %du;" % len(ssectors))
    lines.append("const u16 bsp_node_count = %du;" % len(nodes))
    lines.append("const u16 bsp_door_count = %du;" % next_door_group)
    lines.append("const BspThing bsp_things[%d] = {" % len(out_things))
    for x, y, thing_type, angle, flags in out_things:
        lines.append("    {%d, %d, %du, %du, %du}," %
                     (x, y, thing_type, angle, flags))
    lines.append("};")
    lines.append("const u16 bsp_thing_count = %du;" % len(out_things))
    lines.append("const s32 bsp_player_start_x = %d;" % start_x)
    lines.append("const s32 bsp_player_start_y = %d;" % start_y)
    lines.append("const u16 bsp_player_start_angle = %du;" % start_angle)
    lines.append("")
    lines.append("#endif // !BSP_USE_HAND_MAP")
    lines.append("")

    with open(map_temp, "w", newline="\n") as fh:
        fh.write("\n".join(lines))
    os.replace(asset_temp, args.assets_out)
    os.replace(map_temp, out_path)

    print("Wrote %s" % out_path)
    print("  vertices : %d" % len(vertices))
    print("  segs     : %d flat solid/interactive (of %d source)" %
          (len(out_segs), len(segs)))
    print("  linedefs : %d" % len(linedefs))
    print("  sectors  : %d" % len(sectors))
    print("  textures : %d exact + fallback" % (len(texture_ids) - 1))
    print("  palette  : %s" % " ".join("%02X%02X%02X" % color for color in palette))
    print("  assets   : %s" % args.assets_out)
    print("  subsectors: %d" % len(out_ssectors))
    print("  nodes    : %d (root=%d)" % (len(nodes), root))
    print("  blockmap : %dx%d, %d refs, %d validation queries" %
          (grid_w, grid_h, len(grid_indices), spatial_checks))
    print("  player   : (%d,%d) angle_deg=%d -> %d" % (
        start_x, start_y, start_angle_deg, start_angle))
    print("  things   : %d raw, %d curated, %d skipped" %
          (len(out_things), supported_things, len(out_things) - supported_things))
    print("  doors    : %d groups, faces=%s, fallback faces=%d" %
          (next_door_group, dict(sorted(door_face_counts.items())), fallback_door_faces))
    print("  keys     : available=0x%02X required=0x%02X reached=%s" %
          (certificate["available_keys"], required_key_mask,
           certificate["reached_masks"]))
    print("  certified: exit seg %d reachable with key mask=0x%02X after %d states" %
          (certificate["exit_index"], certificate["key_mask"], certificate["states"]))


if __name__ == "__main__":
    main()
