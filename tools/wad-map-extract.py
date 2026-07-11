#!/usr/bin/env python3
"""Extract a Doom map's BSP geometry from DOOM1.WAD and emit C arrays for the
megaldoom lean BSP engine.

Doom's prebuilt VERTEXES/SEGS/SSECTORS/NODES map almost 1:1 onto our Bsp*
structs (even the 0x8000 subsector-leaf bit is identical), so we convert them
directly -- no on-device node builder needed.

The renderer is uniform-height: a line is either a full-height solid wall or an
open gap. We therefore emit a SEG only when its linedef is "solid":
  - one-sided (faces the void), OR
  - two-sided but IMPASSABLE-flagged, OR
  - two-sided with a closed opening (min ceiling <= max floor -> shut door / solid).
Everything else (normal doorways, steps, ledges, drop-offs) is skipped -> a
passable gap. Same set drives render and collision, so no clip-through / no
soft-locks.

Usage: python tools/wad-map-extract.py [--map E1M1] [--wad DOOM1.WAD]
"""

import argparse
from collections import Counter
import math
import os
import re
import struct

from PIL import Image

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_WAD = os.path.join(PROJECT_ROOT, "DOOM1.WAD")
DEFAULT_ASSET_OUT = os.path.join(PROJECT_ROOT, "src", "generated_assets.h")
ASSET_ROOT = os.path.join(PROJECT_ROOT, "res", "originaldoom")
WALL_TEX_DIM = 32
FALLBACK_TEXTURE = "__FALLBACK__"
FALLBACK_TEXTURE_SOURCE = "GRAY7"
WORLD_COLOR_DAMAGE = 14
WORLD_COLOR_WARNING = 15


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
    return tuple(int(round((c * 7) / 255)) * 255 // 7 for c in rgb)


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
                histogram[md_color((r, g, b))] += weight


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
    indices = range(len(palette)) if allowed is None else allowed
    return min(indices, key=lambda i: (
        sum((rgb[ch] - palette[i][ch]) ** 2 for ch in range(3)), i))


def build_world_palette(texture_names, texture_usage, sectors):
    histogram = Counter()
    for name in texture_names:
        weight = max(1, min(16, texture_usage.get(name, 1)))
        add_image_histogram(histogram, texture_path(name), (WALL_TEX_DIM, WALL_TEX_DIM), weight)

    sprite_inputs = [
        ("sprites/PISGA0.png", (72, 54)), ("sprites/PISGB0.png", (72, 54)),
        ("sprites/BON1A0.png", (16, 16)), ("sprites/BKEYA0.png", (16, 16)),
        ("sprites/BAR1A0.png", (16, 16)),
    ]
    sprite_inputs.extend(("sprites/POSS%s.png" % frame, (24, 48))
                         for frame in ("A1", "B1", "C1", "D1", "F1", "H0", "I0", "J0", "K0", "L0"))
    for relative, size in sprite_inputs:
        add_image_histogram(histogram, os.path.join(ASSET_ROOT, relative), size, 4)

    flat_usage = Counter()
    for sector in sectors:
        flat_usage[sector["floor_name"]] += 1
        flat_usage[sector["ceiling_name"]] += 1
    for name, count in flat_usage.items():
        average = md_color(image_average(flat_path(name)))
        histogram[average] += max(512, count * 128)

    damage = md_color((0xD8, 0x28, 0x18))
    warning = md_color((0xD8, 0xB0, 0x48))
    reserved = {(0, 0, 0), damage, warning}
    adaptive = []
    for color in median_cut_colors(histogram, 16):
        if color not in reserved and color not in adaptive:
            adaptive.append(color)
    for color, _ in histogram.most_common():
        if color not in reserved and color not in adaptive:
            adaptive.append(color)
        if len(adaptive) >= 13:
            break
    fallback_colors = [md_color((v, v, v)) for v in (32, 64, 96, 128, 160, 192, 224)]
    for color in fallback_colors:
        if color not in reserved and color not in adaptive:
            adaptive.append(color)
    adaptive = adaptive[:13]
    adaptive.sort(key=lambda c: (c[0] * 30 + c[1] * 59 + c[2] * 11, c))
    return [(0, 0, 0)] + adaptive + [damage, warning]


def build_shade_map(palette):
    result = []
    for index, color in enumerate(palette):
        if index == 0:
            result.append(0)
            continue
        luminance = color[0] * 30 + color[1] * 59 + color[2] * 11
        darker = [i for i, candidate in enumerate(palette)
                  if (candidate[0] * 30 + candidate[1] * 59 + candidate[2] * 11) < luminance]
        target = tuple(channel * 2 // 3 for channel in color)
        result.append(nearest_palette_index(target, palette, darker or [0]))
    return result


def convert_texture(path, palette):
    with Image.open(path) as image:
        resized = image.convert("RGB").resize((WALL_TEX_DIM, WALL_TEX_DIM), Image.Resampling.BOX)
        return [[nearest_palette_index(resized.getpixel((x, y)), palette)
                 for x in range(WALL_TEX_DIM)] for y in range(WALL_TEX_DIM)]


def texture_shift(size):
    if size >= WALL_TEX_DIM:
        ratio = size // WALL_TEX_DIM
        sign = 1
        exact = ratio * WALL_TEX_DIM == size
    else:
        ratio = WALL_TEX_DIM // size
        sign = -1
        exact = ratio * size == WALL_TEX_DIM
    if not exact or ratio & (ratio - 1):
        raise ValueError("Texture dimension %d is not WALL_TEX_DIM times a power of two" % size)
    return sign * (ratio.bit_length() - 1)


def emit_world_assets(path, texture_usage, sectors):
    texture_names = [FALLBACK_TEXTURE] + sorted(
        name for name in texture_usage if name != FALLBACK_TEXTURE)
    for name in texture_names:
        if not os.path.isfile(texture_path(name)):
            raise FileNotFoundError("Wall texture source not found: %s" % texture_path(name))

    palette = build_world_palette(texture_names, texture_usage, sectors)
    if len(palette) != 16:
        raise RuntimeError("World palette must contain exactly 16 colors")
    shade_map = build_shade_map(palette)
    texture_ids = {name: index for index, name in enumerate(texture_names)}
    texture_meta = {}
    converted = []
    for name in texture_names:
        source = texture_path(name)
        with Image.open(source) as image:
            width, height = image.size
        texture_meta[name] = dict(width=width, height=height, ushift=texture_shift(width))
        converted.append(convert_texture(source, palette))

    sector_visuals = []
    for sector in sectors:
        colors = []
        for field in ("ceiling_name", "floor_name"):
            name = sector[field]
            average = image_average(flat_path(name))
            if name != "F_SKY1":
                average = tuple(channel * sector["light"] // 255 for channel in average)
            colors.append(nearest_palette_index(md_color(average), palette, range(0, WORLD_COLOR_DAMAGE)))
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
        "static const u8 FREEDOOM_WORLD_SHADE_MAP[16] = {",
        "    " + ", ".join(str(value) for value in shade_map),
        "};",
        "",
        "static const s8 FREEDOOM_WALL_TEXTURE_USHIFT[FREEDOOM_WALL_TEXTURE_COUNT] = {",
        "    " + ", ".join(str(texture_meta[name]["ushift"]) for name in texture_names),
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


# BspSegType (must match src/bsp_map.h)
SEG_WALL = 0
SEG_DOOR = 1
SEG_LOCKED_DOOR = 2
SEG_EXIT = 3

DOOR_SPECIALS = {1, 26, 27, 28, 31, 32, 33, 34, 117, 118}
LOCKED_DOOR_SPECIALS = {26, 27, 28, 32, 33, 34}
EXIT_SPECIALS = {11, 51}

LINE_FLAG_IMPASSABLE = 0x0001


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
            light=max(0, min(255, light)),
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
                             right=right, left=left))

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

    # --- Classify each linedef as solid (wall) or open (gap). --------------- #
    def line_solid(ld):
        if ld["left"] == 0xFFFF:
            return True  # one-sided
        if ld["flags"] & LINE_FLAG_IMPASSABLE:
            return True
        rs = sidedefs[ld["right"]]["sector"] if ld["right"] != 0xFFFF else None
        ls = sidedefs[ld["left"]]["sector"] if ld["left"] != 0xFFFF else None
        if rs is None or ls is None:
            return True
        rf, rc = sectors[rs]["floor"], sectors[rs]["ceiling"]
        lf, lc = sectors[ls]["floor"], sectors[ls]["ceiling"]
        opening = min(rc, lc) - max(rf, lf)
        return opening <= 0

    def front_side_for(seg):
        ld = linedefs[seg["ld"]]
        front_side = ld["right"] if seg["direction"] == 0 else ld["left"]
        if front_side == 0xFFFF:
            front_side = ld["right"]
        return front_side

    def seg_type_and_visual(seg):
        ld = linedefs[seg["ld"]]
        front_side = front_side_for(seg)
        seg_type = SEG_WALL
        if ld["special"] in EXIT_SPECIALS:
            seg_type = SEG_EXIT
        elif ld["special"] in DOOR_SPECIALS:
            if ld["special"] in LOCKED_DOOR_SPECIALS:
                seg_type = SEG_LOCKED_DOOR
            else:
                seg_type = SEG_DOOR

        name = FALLBACK_TEXTURE
        xoff = seg["offset"]
        yoff = 0
        if front_side != 0xFFFF:
            side = sidedefs[front_side]
            xoff += side["xoff"]
            yoff = side["yoff"]
            for candidate in (side["middle"], side["lower"], side["upper"]):
                if candidate and candidate != "-":
                    name = candidate
                    break
        return seg_type, name, xoff, yoff

    # --- Rebuild subsectors with only solid segs, preserving grouping. ------ #
    out_segs = []       # dicts with geometry, exact texture name, offsets and type
    out_ssectors = []   # (first_seg, count, sector_id)
    texture_usage = Counter()

    def front_normal(seg):
        """Normal pointing into the seg's FRONT sector, in y-down space.

        Deterministic from Doom's linedef direction + seg side (NOT a centroid
        guess): in y-up, the normal into the right sector is (Ly,-Lx); into the
        left sector it is (-Ly,Lx). We then negate the y component to match the
        engine's y-down space.
        """
        ld = linedefs[seg["ld"]]
        lv1 = verts_up[ld["v1"]]
        lv2 = verts_up[ld["v2"]]
        lx = lv2[0] - lv1[0]
        ly = lv2[1] - lv1[1]
        if seg["direction"] == 0:
            nux, nuy = ly, -lx   # into right sector (y-up)
        else:
            nux, nuy = -ly, lx   # into left sector (y-up)
        return reduce_normal(nux, -nuy)  # flip y to y-down

    for (count, first) in ssectors:
        start = len(out_segs)
        sector_id = 0
        if count:
            source_side = front_side_for(segs[first])
            if source_side != 0xFFFF:
                sector_id = sidedefs[source_side]["sector"]
        for k in range(count):
            seg = segs[first + k]
            if not line_solid(linedefs[seg["ld"]]):
                continue
            ax, ay = vertices[seg["v1"]]
            bx, by = vertices[seg["v2"]]
            if ax == bx and ay == by:
                continue  # degenerate seg
            nx, ny = front_normal(seg)
            if nx == 0 and ny == 0:
                continue  # degenerate linedef
            stype, texture_name, tex_u_offset, tex_v_offset = seg_type_and_visual(seg)
            texture_usage[texture_name] += 1
            out_segs.append(dict(v1=seg["v1"], v2=seg["v2"],
                                nx=nx, ny=ny, texture_name=texture_name,
                                tex_u_offset=tex_u_offset, tex_v_offset=tex_v_offset,
                                type=stype))
        out_ssectors.append((start, len(out_segs) - start, sector_id))

    if len(out_segs) > 1024:
        raise SystemExit("solid seg count %d exceeds BSP_MAX_SEGS (1024)"
                         % len(out_segs))

    texture_ids, texture_meta, sector_visuals, palette = emit_world_assets(
        args.assets_out, texture_usage, sectors)
    for seg in out_segs:
        name = seg["texture_name"]
        if not -32768 <= seg["tex_u_offset"] <= 32767:
            raise SystemExit("texture U offset out of s16 range: %d" % seg["tex_u_offset"])
        seg["tex"] = texture_ids[name]
        seg["tex_v"] = ((seg["tex_v_offset"] * WALL_TEX_DIM) //
                        texture_meta[name]["height"]) & (WALL_TEX_DIM - 1)

    # --- Player 1 start from THINGS (type 1). ------------------------------- #
    start_x = start_y = 0
    start_angle_deg = 0
    for i in range(len(things_raw) // 10):
        tx, ty, tang, ttype, tflags = struct.unpack_from("<hhHHH", things_raw, i * 10)
        if ttype == 1:
            start_x, start_y, start_angle_deg = tx, ty, tang
            break
    # Y-down flip: negate the start Y and mirror the angle about the x-axis.
    start_y = -start_y
    start_angle = (256 - (round(start_angle_deg * 256 / 360) & 255)) & 255

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
        lines.append("    {%d, %d, %d, %d, %d, %d, %d, %d}," % (
            s["v1"], s["v2"], s["nx"], s["ny"], s["tex_u_offset"],
            s["tex_v"], s["tex"], s["type"]))
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
    lines.append("")

    lines.append("const BspSubsector bsp_subsectors[%d] = {" % len(out_ssectors))
    for (first, count, sector_id) in out_ssectors:
        lines.append("    {%d, %d, %d}," % (first, count, sector_id))
    lines.append("};")
    lines.append("")

    lines.append("const BspSectorVisual bsp_sector_visuals[%d] = {" % len(sector_visuals))
    for ceiling_color, floor_color in sector_visuals:
        lines.append("    {%d, %d}," % (ceiling_color, floor_color))
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
    lines.append("const u16 bsp_node_count = %du;" % len(nodes))
    lines.append("const s32 bsp_player_start_x = %d;" % start_x)
    lines.append("const s32 bsp_player_start_y = %d;" % start_y)
    lines.append("const u16 bsp_player_start_angle = %du;" % start_angle)
    lines.append("")
    lines.append("#endif // !BSP_USE_HAND_MAP")
    lines.append("")

    with open(out_path, "w", newline="\n") as fh:
        fh.write("\n".join(lines))

    print("Wrote %s" % out_path)
    print("  vertices : %d" % len(vertices))
    print("  segs     : %d solid (of %d total)" % (len(out_segs), len(segs)))
    print("  textures : %d exact + fallback" % (len(texture_ids) - 1))
    print("  palette  : %s" % " ".join("%02X%02X%02X" % color for color in palette))
    print("  assets   : %s" % args.assets_out)
    print("  subsectors: %d" % len(out_ssectors))
    print("  nodes    : %d (root=%d)" % (len(nodes), root))
    print("  blockmap : %dx%d, %d refs, %d validation queries" %
          (grid_w, grid_h, len(grid_indices), spatial_checks))
    print("  player   : (%d,%d) angle_deg=%d -> %d" % (
        start_x, start_y, start_angle_deg, start_angle))


if __name__ == "__main__":
    main()
