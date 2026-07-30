#!/usr/bin/env python3
"""Generate the tiled rock backdrop that fills the margins around the 3D
viewport (see AGENTS.md's viewport presentation pass, 2026-07-30).

Source: res/originaldoom/flats/FLOOR7_2.png, the Doom 1 border flat (64x64).
PAL0 (the bezel/HUD palette, src/renderer.c's load_game_palettes()) has
exactly ONE genuinely green swatch (index 15, 0x4C6028) among its 16 entries
-- it was built for UI panels, not a photographic rock texture. A per-pixel
nearest-RGB-distance quantization of the flat therefore almost never picks
index 15 (most of the flat's blended tones are numerically closer to the
neutral greys/browns at indices 3/14), producing a muddy grey-brown result
that doesn't read as green at all. See AGENTS.md for the before/after.

Fix: follow the SAME convention this renderer already uses for every floor
and ceiling (src/renderer_flats.c, FREEDOOM_SECTOR_VISUALS) -- reduce the flat
to a few deliberate tones by luminance threshold, not full-palette nearest
match, so the chosen colours (not RGB-distance accidents) decide the look.
The flat's own blotch SHAPE survives (from its luminance, blurred to remove
dither noise); the COLOUR is picked to guarantee index 15 dominates.

Usage: python tools/generate-backdrop-tiles.py
Writes the MEGALDOOM_BACKDROP_TILE_DIM / MEGALDOOM_BACKDROP_TILES block into
src/generated_renderer_assets.h, replacing any previous block bounded by the
BEGIN/END markers below.
"""
import re
from pathlib import Path

from PIL import Image, ImageFilter

ROOT = Path(__file__).resolve().parents[1]
SOURCE_PNG = ROOT / "res/originaldoom/flats/FLOOR7_2.png"
OUTPUT_HEADER = ROOT / "src/generated_renderer_assets.h"

TILE_DIM = 4          # 4x4 tiles = 32x32 px
TILE_PX = TILE_DIM * 8  # 32
BLUR_RADIUS = 1.5    # removes FLOOR7_2's fine per-pixel dither before thresholding

# PAL0, must stay byte-identical to src/renderer.c's load_game_palettes()
# PAL_setColor(0..15) calls.
PAL0 = [
    0x000000, 0xD8D8D8, 0x181410, 0x383030,
    0x585048, 0x888078, 0xB4ACA0, 0xE8E0D0,
    0x301E10, 0x4878A8, 0x78502C, 0xD8B048,
    0x982818, 0xA86838, 0x484038, 0x4C6028,
]
PAL0_RGB = [((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF) for c in PAL0]

# Three-tone luminance threshold: bright blotches -> green (15, the moss
# highlight), midtones -> dark olive-brown (14), darkest crevices -> near-
# black (2). Thresholds are luminance-histogram percentiles of the blurred,
# downsampled flat, so they adapt to whatever source image is used.
PRIMARY_INDEX = 15    # bright blotches (moss highlight)
SECONDARY_INDEX = 14  # midtones (dark olive-brown)
DEEP_INDEX = 2         # darkest crevices (near-black)
DEEP_PERCENTILE = 0.30
PRIMARY_PERCENTILE = 0.70

BEGIN_MARKER = "// BEGIN generate-backdrop-tiles.py output"
END_MARKER = "// END generate-backdrop-tiles.py output"


def box_downsample_luminance(image, dst_w, dst_h):
    src_w, src_h = image.size
    assert src_w % dst_w == 0 and src_h % dst_h == 0, (
        "expects an integer downsample factor")
    fx, fy = src_w // dst_w, src_h // dst_h
    px = image.load()
    out = [[0] * dst_w for _ in range(dst_h)]
    for y in range(dst_h):
        for x in range(dst_w):
            total = 0
            for sy in range(y * fy, y * fy + fy):
                for sx in range(x * fx, x * fx + fx):
                    total += px[sx, sy]
            out[y][x] = total // (fx * fy)
    return out


def main() -> int:
    image = Image.open(SOURCE_PNG).convert("L")
    assert image.size == (64, 64), image.size
    image = image.filter(ImageFilter.GaussianBlur(BLUR_RADIUS))
    luminance = box_downsample_luminance(image, TILE_PX, TILE_PX)

    flat_values = sorted(v for row in luminance for v in row)
    n = len(flat_values)
    deep_threshold = flat_values[int(n * DEEP_PERCENTILE)]
    primary_threshold = flat_values[int(n * PRIMARY_PERCENTILE)]

    indices = [[None] * TILE_PX for _ in range(TILE_PX)]
    for y in range(TILE_PX):
        for x in range(TILE_PX):
            v = luminance[y][x]
            if v <= deep_threshold:
                indices[y][x] = DEEP_INDEX
            elif v >= primary_threshold:
                indices[y][x] = PRIMARY_INDEX
            else:
                indices[y][x] = SECONDARY_INDEX

    tiles = []
    for tile_y in range(TILE_DIM):
        for tile_x in range(TILE_DIM):
            rows = []
            for row in range(8):
                packed = 0
                for col in range(8):
                    px_index = indices[tile_y * 8 + row][tile_x * 8 + col]
                    packed = (packed << 4) | (px_index & 0x0F)
                rows.append(f"0x{packed:08X}")
            tiles.append("    {" + ", ".join(rows) + "},")

    lines = [
        BEGIN_MARKER,
        "// Tiled rock backdrop for the margins around the 3D viewport (see",
        "// tools/generate-backdrop-tiles.py; source res/originaldoom/flats/FLOOR7_2.png,",
        "// box-downsampled 64x64 -> 32x32 and quantized to PAL0). Anchor tile index for",
        "// screen tile (x,y) is ((y & (DIM-1)) * DIM) + (x & (DIM-1)), so the 4x4 block",
        "// repeats seamlessly across the margins (see renderer_draw_backdrop in",
        "// renderer_hud.c).",
        f"#define MEGALDOOM_BACKDROP_TILE_DIM {TILE_DIM}",
        f"#define MEGALDOOM_BACKDROP_TILE_COUNT (MEGALDOOM_BACKDROP_TILE_DIM * MEGALDOOM_BACKDROP_TILE_DIM)",
        "static const u32 MEGALDOOM_BACKDROP_TILES[MEGALDOOM_BACKDROP_TILE_COUNT][8] = {",
        *tiles,
        "};",
        END_MARKER,
    ]
    block = "\n".join(lines)

    text = OUTPUT_HEADER.read_text()
    pattern = re.compile(
        re.escape(BEGIN_MARKER) + r".*?" + re.escape(END_MARKER), re.S)
    if pattern.search(text):
        text = pattern.sub(block, text)
    else:
        text = text.replace("\n#endif", f"\n{block}\n\n#endif")
    OUTPUT_HEADER.write_text(text)
    print(f"ok    backdrop: {TILE_DIM}x{TILE_DIM} tiles from {SOURCE_PNG.name}, "
          f"written to {OUTPUT_HEADER.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
