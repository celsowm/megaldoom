#!/usr/bin/env python3
"""
wad-extract.py - Extract Doom WAD assets to PNG files.

Inspired by tools/wad-extract.js (a Scala.js application that parses a Doom
WAD and renders its graphic lumps to PNG images in a browser).

This Python equivalent reads the IWAD/PWAD located at the project root
(DOOM1.WAD by default) and writes every graphic asset out as a PNG under
res/originaldoom/, divided into four categories that mirror the original
application:

    res/originaldoom/sprites/   objects that appear inside a map
    res/originaldoom/flats/     used on ceilings and floors
    res/originaldoom/textures/  composited wall textures (TEXTURE1/2 + PNAMES)
    res/originaldoom/graphics/  UI elements and miscellaneous other images

Dependencies: Pillow (PIL).  It is already present in this project's
environment; if missing, install with `pip install Pillow`.
"""

import argparse
import os
import struct
import sys
import time

from PIL import Image

# --------------------------------------------------------------------------- #
# Configuration / constants
# --------------------------------------------------------------------------- #

# Project root is the parent of the tools/ directory.
PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

DEFAULT_WAD = os.path.join(PROJECT_ROOT, "DOOM1.WAD")
DEFAULT_OUT = os.path.join(PROJECT_ROOT, "res", "originaldoom")

# WAD header / directory layout (all little-endian).
WAD_HEADER_FMT = "<4sii"          # signature, numLumps, dirOffset
WAD_HEADER_SIZE = struct.calcsize(WAD_HEADER_FMT)
LUMP_ENTRY_FMT = "<ii8s"          # filepos, size, name
LUMP_ENTRY_SIZE = struct.calcsize(LUMP_ENTRY_FMT)

# Doom picture (column) format header.
PIC_HEADER_FMT = "<HHhh"          # width, height, leftOffset, topOffset
PIC_HEADER_SIZE = struct.calcsize(PIC_HEADER_FMT)

# Sanity bounds so a corrupted/truncated lump cannot produce absurd images.
MAX_PIC_WIDTH = 4096
MAX_PIC_HEIGHT = 4096
MAX_TEXTURE_DIM = 4096

# 8-char lump names that must never be treated as graphic data.  This covers
# map lumps, sound/music lumps, palettes, colormaps and metadata lumps that are
# not column-format pictures nor raw flats.
NON_GRAPHIC_LUMPS = {
    "THINGS", "LINEDEFS", "SIDEDEFS", "VERTEXES", "SEGS", "SSECTORS",
    "NODES", "SECTORS", "REJECT", "BLOCKMAP", "BEHAVIOR",
    "PLAYPAL", "COLORMAP", "ENDOOM",
    "DEMO1", "DEMO2", "DEMO3", "DEMO4",
    "GENMIDI", "DMXGUS", "DPBODY", "DPPAUSE", "DPPIANO", "DPSG", "DPSAW",
    "DPSQ", "DPSTR", "DPSTRNG", "DPVOID",
    "PNAMES", "TEXTURE1", "TEXTURE2",
}

# Marker lump names (boundaries inside the directory).
# Flats live between F_START/F_END (and F1/F2/F3 variant markers).
# Sprites live between S_START/S_END.
# Patches live between P_START/P_END (and P1/P2/P3 variant markers).
# Other graphics are standalone column-format lumps that are not inside any of
# the marker ranges and are not maps/sound/etc.


# --------------------------------------------------------------------------- #
# WAD data structures
# --------------------------------------------------------------------------- #

class Lump:
    """A single directory entry from a WAD file."""

    __slots__ = ("name", "filepos", "size", "raw_name")

    def __init__(self, name, filepos, size, raw_name):
        self.name = name          # str, uppercased, null-trimmed
        self.filepos = filepos    # int, absolute offset into the WAD
        self.size = size          # int, byte length of the lump data
        self.raw_name = raw_name  # bytes, original 8-byte field

WAD_HEADER_FMT = "<4sii"          # signature, numLumps, dirOffset
WAD_HEADER_SIZE = struct.calcsize(WAD_HEADER_FMT)

class WadFile:
    """Minimal Doom WAD reader."""

    def __init__(self, path):
        self.path = path
        with open(path, "rb") as fh:
            self.data = fh.read()
        self.lumps = []
        self._parse_header()

    def _parse_header(self):
        if len(self.data) < WAD_HEADER_SIZE:
            raise ValueError("File too small to be a WAD.")
        signature, num_lumps, dir_offset = struct.unpack_from(
            WAD_HEADER_FMT, self.data, 0
        )
        signature = signature.decode("ascii", "ignore")
        if signature not in ("IWAD", "PWAD"):
            raise ValueError(
                "Not a WAD file (signature=%r). Expected 'IWAD' or 'PWAD'."
                % signature
            )
        self.signature = signature
        self.num_lumps = num_lumps
        self.dir_offset = dir_offset

        end = dir_offset + num_lumps * LUMP_ENTRY_SIZE
        if end > len(self.data):
            raise ValueError("WAD directory extends past end of file.")

        for i in range(num_lumps):
            off = dir_offset + i * LUMP_ENTRY_SIZE
            filepos, size, raw_name = struct.unpack_from(
                LUMP_ENTRY_FMT, self.data, off
            )
            name = raw_name.split(b"\x00", 1)[0].decode("ascii", "ignore")
            name = name.upper()
            self.lumps.append(Lump(name, filepos, size, raw_name))

    # -- lump access helpers ------------------------------------------------ #

    def lump_data(self, lump):
        return self.data[lump.filepos:lump.filepos + lump.size]

    def find(self, name):
        """Return the first Lump with the given name, or None."""
        for lump in self.lumps:
            if lump.name == name:
                return lump
        return None


# --------------------------------------------------------------------------- #
# Palette
# --------------------------------------------------------------------------- #

def extract_palette(wad):
    """Return the first 256-colour palette from PLAYPAL (list of (r,g,b))."""
    playpal = wad.find("PLAYPAL")
    if playpal is None:
        raise ValueError("PLAYPAL lump not found in WAD.")
    data = wad.lump_data(playpal)
    palette = []
    for i in range(256):
        r = data[i * 3]
        g = data[i * 3 + 1]
        b = data[i * 3 + 2]
        palette.append((r, g, b))
    return palette


def save_palette_png(palette, out_path):
    """Write a 16x16 reference PNG of the palette."""
    img = Image.new("RGB", (16, 16))
    for idx, color in enumerate(palette):
        x = idx % 16
        y = idx // 16
        img.putpixel((x, y), color)
    img.save(out_path)


# --------------------------------------------------------------------------- #
# Lump categorisation
# --------------------------------------------------------------------------- #

def _starts_any(name, prefixes):
    return any(name.startswith(p) for p in prefixes)


def _is_map_header(name):
    # ExMy (Doom 1) or MAPxx (Doom 2).
    if len(name) == 4 and name[0] == "E" and name[2] == "M":
        try:
            int(name[1])
            int(name[3])
            return True
        except ValueError:
            return False
    if len(name) == 5 and name.startswith("MAP"):
        try:
            int(name[3:5])
            return True
        except ValueError:
            return False
    return False


def categorize_lumps(wad):
    """Walk the WAD directory and sort every lump into a category.

    Returns a dict with keys: flats, sprites, patches, graphics, texture_meta
    where texture_meta holds the raw TEXTURE1/TEXTURE2/PNAMES lumps for later
    compositing.
    """
    FLAT_MARKERS = {
        "F_START", "F_END",
        "F1_START", "F1_END",
        "F2_START", "F2_END",
        "F3_START", "F3_END",
    }
    SPRITE_MARKERS = {"S_START", "S_END", "SS_START", "SS_END"}
    PATCH_MARKERS = {
        "P_START", "P_END",
        "P1_START", "P1_END",
        "P2_START", "P2_END",
        "P3_START", "P3_END",
        "PP_START", "PP_END",
    }

    flats = []
    sprites = []
    patches = []
    graphics = []
    texture_meta = {"TEXTURE1": None, "TEXTURE2": None, "PNAMES": None}

    section = None  # one of: "flat", "sprite", "patch"

    for lump in wad.lumps:
        name = lump.name

        if name in FLAT_MARKERS:
            section = "flat" if name.endswith("_START") else None
            continue
        if name in SPRITE_MARKERS:
            section = "sprite" if name.endswith("_START") else None
            continue
        if name in PATCH_MARKERS:
            section = "patch" if name.endswith("_START") else None
            continue

        # Grab metadata lumps regardless of section.
        if name == "TEXTURE1":
            texture_meta["TEXTURE1"] = lump
            continue
        if name == "TEXTURE2":
            texture_meta["TEXTURE2"] = lump
            continue
        if name == "PNAMES":
            texture_meta["PNAMES"] = lump
            continue

        if name in NON_GRAPHIC_LUMPS:
            continue

        if _is_map_header(name):
            continue

        # Skip sound/music lumps (often start with DS, DP, D_).
        if name.startswith(("DS", "DP", "D_")):
            continue

        if section == "flat":
            if lump.size >= 4096:
                flats.append(lump)
            continue
        if section == "sprite":
            if lump.size > 0:
                sprites.append(lump)
            continue
        if section == "patch":
            if lump.size >= PIC_HEADER_SIZE:
                patches.append(lump)
            continue

        # Outside any marker section: treat as a standalone graphic if it has
        # a plausible picture header.
        if lump.size >= PIC_HEADER_SIZE and _looks_like_picture(wad, lump):
            graphics.append(lump)

    return {
        "flats": flats,
        "sprites": sprites,
        "patches": patches,
        "graphics": graphics,
        "texture_meta": texture_meta,
    }


def _looks_like_picture(wad, lump):
    """Heuristic: the picture header's width/height are within sane bounds."""
    try:
        width, height, _, _ = struct.unpack_from(
            PIC_HEADER_FMT, wad.data, lump.filepos
        )
    except struct.error:
        return False
    return 0 < width <= MAX_PIC_WIDTH and 0 < height <= MAX_PIC_HEIGHT



# --------------------------------------------------------------------------- #
# Picture (column) decoding
# --------------------------------------------------------------------------- #

def decode_picture(wad, lump, palette):
    """Decode a Doom column-format picture lump into an RGBA PIL Image.

    Colour index 0 is treated as transparent.  Returns None if the lump cannot
    be decoded (e.g. zero width/height or corrupt column data).
    """
    data = wad.data
    base = lump.filepos
    size = lump.size
    if size < PIC_HEADER_SIZE:
        return None

    width, height, left_offset, top_offset = struct.unpack_from(
        PIC_HEADER_FMT, data, base
    )
    if width <= 0 or height <= 0:
        return None
    if width > MAX_PIC_WIDTH or height > MAX_PIC_HEIGHT:
        return None

    # Column offset table: width * 4 bytes, relative to the lump start.
    if PIC_HEADER_SIZE + width * 4 > size:
        return None
    column_offsets = struct.unpack_from("<%dI" % width, data,
                                        base + PIC_HEADER_SIZE)

    img = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    px = img.load()

    for x in range(width):
        col_off = column_offsets[x]
        if col_off == 0 or col_off >= size:
            continue
        cursor = base + col_off
        guard = 0
        while True:
            guard += 1
            if guard > 65536:
                break  # runaway guard
            if cursor >= base + size:
                break
            top_delta = data[cursor]
            cursor += 1
            if top_delta == 0xFF:
                break  # end-of-column marker
            if cursor >= base + size:
                break
            length = data[cursor]
            cursor += 1
            # Skip the unused padding byte before the pixel run.
            cursor += 1
            for i in range(length):
                if cursor >= base + size:
                    break
                if top_delta + i < height:
                    color_index = data[cursor]
                    r, g, b = palette[color_index]
                    alpha = 0 if color_index == 0 else 255
                    px[x, top_delta + i] = (r, g, b, alpha)
                cursor += 1
            # Skip the trailing unused padding byte.
            cursor += 1

    return img


# --------------------------------------------------------------------------- #
# Flat decoding
# --------------------------------------------------------------------------- #

def decode_flat(wad, lump, palette):
    """Decode a raw 64x64 flat lump into an RGB PIL Image.

    Flats are opaque (index 0 is a real colour, not transparent).  Returns
    None if the lump is too small.
    """
    if lump.size < 4096:
        return None
    data = wad.lump_data(lump)[:4096]
    img = Image.new("RGB", (64, 64))
    px = img.load()
    for y in range(64):
        for x in range(64):
            idx = data[y * 64 + x]
            px[x, y] = palette[idx]
    return img


# --------------------------------------------------------------------------- #
# Texture compositing (TEXTURE1 / TEXTURE2 + PNAMES)
# --------------------------------------------------------------------------- #

def parse_pnames(wad, pnames_lump):
    """Return a list of patch names (uppercase strings) from PNAMES."""
    if pnames_lump is None:
        return []
    data = wad.lump_data(pnames_lump)
    if len(data) < 4:
        return []
    (count,) = struct.unpack_from("<I", data, 0)
    names = []
    off = 4
    for _ in range(count):
        if off + 8 > len(data):
            break
        raw = data[off:off + 8]
        name = raw.split(b"\x00", 1)[0].decode("ascii", "ignore").upper()
        names.append(name)
        off += 8
    return names


def parse_texture_lump(wad, tex_lump):
    """Parse a TEXTURE1/TEXTURE2 lump into a list of texture definitions.

    Each entry is a dict: {name, width, height, patches:[{x,y,index}]}.
    """
    if tex_lump is None:
        return []
    data = wad.lump_data(tex_lump)
    if len(data) < 4:
        return []
    (num_textures,) = struct.unpack_from("<I", data, 0)
    offsets = struct.unpack_from("<%dI" % num_textures, data, 4)

    textures = []
    for i in range(num_textures):
        off = offsets[i]
        if off + 22 > len(data):
            continue
        raw_name = data[off:off + 8]
        name = raw_name.split(b"\x00", 1)[0].decode("ascii", "ignore").upper()
        width, height = struct.unpack_from("<HH", data, off + 12)
        (patch_count,) = struct.unpack_from("<H", data, off + 20)
        patches = []
        poff = off + 22
        for _ in range(patch_count):
            if poff + 10 > len(data):
                break
            origin_x, origin_y, patch_index = struct.unpack_from("<hhh",
                                                                 data, poff)
            patches.append({
                "x": origin_x,
                "y": origin_y,
                "index": patch_index,
            })
            poff += 10
        textures.append({
            "name": name,
            "width": width,
            "height": height,
            "patches": patches,
        })
    return textures


def composite_textures(wad, categories, palette):
    """Build composited RGBA images for every TEXTURE1/TEXTURE2 texture.

    Returns a list of (name, PIL.Image).
    """
    tex_meta = categories["texture_meta"]
    pnames = parse_pnames(wad, tex_meta["PNAMES"])

    # Build a lookup: patch name -> decoded RGBA image.
    patch_by_name = {}
    for lump in categories["patches"]:
        img = decode_picture(wad, lump, palette)
        if img is not None:
            patch_by_name[lump.name] = img

    results = []
    for tex_lump_name in ("TEXTURE1", "TEXTURE2"):
        tex_defs = parse_texture_lump(wad, tex_meta[tex_lump_name])
        for tdef in tex_defs:
            width = tdef["width"]
            height = tdef["height"]
            if width <= 0 or height <= 0:
                continue
            if width > MAX_TEXTURE_DIM or height > MAX_TEXTURE_DIM:
                continue
            canvas = Image.new("RGBA", (width, height), (0, 0, 0, 0))
            for patch in tdef["patches"]:
                idx = patch["index"]
                if idx < 0 or idx >= len(pnames):
                    continue
                pname = pnames[idx]
                pimg = patch_by_name.get(pname)
                if pimg is None:
                    continue
                canvas.alpha_composite(pimg, (patch["x"], patch["y"]))
            results.append((tdef["name"], canvas))
    return results


# --------------------------------------------------------------------------- #
# Output helpers
# --------------------------------------------------------------------------- #

def safe_filename(name):
    """Make a lump name safe for the filesystem (lump names are already tame,
    but guard against empty strings)."""
    if not name:
        return "UNNAMED"
    return name


def write_category(out_root, subfolder, items, render_fn, label):
    """Render and save a list of lumps.

    render_fn(lump) -> PIL.Image or None.
    """
    folder = os.path.join(out_root, subfolder)
    os.makedirs(folder, exist_ok=True)
    count = 0
    for lump in items:
        try:
            img = render_fn(lump)
        except Exception as exc:  # noqa: BLE001 - keep extracting on bad lump
            print("  ! %s/%s failed: %s" % (subfolder, lump.name, exc),
                  file=sys.stderr)
            continue
        if img is None:
            continue
        out_path = os.path.join(folder, safe_filename(lump.name) + ".png")
        img.save(out_path)
        count += 1
    print("  %-10s %4d images -> %s" % (label, count, subfolder))
    return count


def write_textures(out_root, textures):
    folder = os.path.join(out_root, "textures")
    os.makedirs(folder, exist_ok=True)
    count = 0
    for name, img in textures:
        out_path = os.path.join(folder, safe_filename(name) + ".png")
        img.save(out_path)
        count += 1
    print("  %-10s %4d images -> textures" % ("textures", count))
    return count

# --------------------------------------------------------------------------- #
# Main
# --------------------------------------------------------------------------- #

def main():
    parser = argparse.ArgumentParser(
        description="Extract Doom WAD graphic assets to PNG files."
    )
    parser.add_argument(
        "--wad", default=DEFAULT_WAD,
        help="Path to the WAD file (default: DOOM1.WAD in project root)."
    )
    parser.add_argument(
        "--out", default=DEFAULT_OUT,
        help="Output directory (default: res/originaldoom)."
    )
    args = parser.parse_args()

    wad_path = os.path.abspath(args.wad)
    out_root = os.path.abspath(args.out)

    if not os.path.isfile(wad_path):
        print("Error: WAD file not found: %s" % wad_path, file=sys.stderr)
        return 1

    print("Reading WAD: %s" % wad_path)
    t0 = time.time()
    wad = WadFile(wad_path)
    print("  signature=%s  lumps=%d  dir_offset=%d"
          % (wad.signature, wad.num_lumps, wad.dir_offset))

    palette = extract_palette(wad)
    print("  PLAYPAL palette loaded (%d colours)." % len(palette))

    categories = categorize_lumps(wad)
    t_parse = time.time() - t0
    print("  categorised: flats=%d sprites=%d patches=%d graphics=%d"
          % (len(categories["flats"]), len(categories["sprites"]),
             len(categories["patches"]), len(categories["graphics"])))

    os.makedirs(out_root, exist_ok=True)
    print("Writing to: %s" % out_root)

    t1 = time.time()
    total = 0

    # Palette reference image.
    save_palette_png(palette, os.path.join(out_root, "palette.png"))
    print("  %-10s %4d images -> palette.png" % ("palette", 1))

    total += write_category(
        out_root, "flats", categories["flats"],
        lambda lump: decode_flat(wad, lump, palette),
        "flats",
    )
    total += write_category(
        out_root, "sprites", categories["sprites"],
        lambda lump: decode_picture(wad, lump, palette),
        "sprites",
    )
    total += write_category(
        out_root, "graphics", categories["graphics"],
        lambda lump: decode_picture(wad, lump, palette),
        "graphics",
    )
    textures = composite_textures(wad, categories, palette)
    total += write_textures(out_root, textures)

    t_build = time.time() - t1
    t_total = time.time() - t0

    print("-" * 52)
    print("Total PNGs written: %d" % total)
    print("Parse time:      %.2f s" % t_parse)
    print("Build time:      %.2f s" % t_build)
    print("Total time:      %.2f s" % t_total)
    return 0


if __name__ == "__main__":
    sys.exit(main())
