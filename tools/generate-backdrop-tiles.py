#!/usr/bin/env python3
"""Generate the tiled rock backdrop that fills the margins around the 3D
viewport (see AGENTS.md's viewport presentation pass, 2026-07-30).

Source: res/originaldoom/flats/FLOOR7_2.png, the Doom 1 border flat (64x64).

The reference look is a DESATURATED, low-contrast, fine-grained grey-green --
the source flat is genuinely that subtle: 80% of its pixels sit in luminance
37..73, and four near-identical green tones ((47,55,31), (55,63,39),
(63,71,43), (71,79,51)) account for 80% of the image. There is essentially no
black in it.

PAL0 (the bezel/HUD palette, src/renderer.c's load_game_palettes()) cannot
represent that directly: it holds exactly ONE green, index 15 (0x4C6028), and
that green is SATURATED. Two approaches were tried and both failed for the
same underlying reason -- any scheme that assigns one flat colour per pixel
must pick either the saturated green or a neutral grey, so the result reads as
loud green patches on grey, never as muted grey-green:

  * nearest-RGB-distance over all 16 entries -> almost never picks index 15
    (most source tones are numerically closer to the neutrals at 3/14), giving
    a muddy grey-brown that isn't green at all;
  * luminance thresholding to 3 flat tones -> forced index 15 onto ~30% of
    pixels and near-black (index 2) onto another 30%, giving high-contrast
    green blotches. Worse than the first attempt.

Fix: get the missing colour by OPTICAL MIXING rather than by picking it, using
the same ordered-dither idiom this renderer already applies to every floor and
ceiling (src/renderer_flats.c). Index 15 (76,96,40) dithered against index 14
(72,64,56) averages to roughly (74,80,48) -- a muted olive that exists nowhere
in PAL0 as a swatch. Source luminance modulates the mix ratio, so the flat's
own grain survives while the perceived colour lands on the reference.

Two further details that matter for the look:
  * downsample 64x64 -> 32x32 by box average, do NOT crop. A crop keeps the
    source's large coherent blotches, which tile visibly; the box average
    breaks them into fine grain.
  * do NOT pre-blur. Blurring removes exactly the high-frequency grain that
    makes this read as texture instead of as shapes.

Usage: python tools/generate-backdrop-tiles.py
Writes the MEGALDOOM_BACKDROP_TILE_DIM / MEGALDOOM_BACKDROP_TILES block into
src/generated_renderer_assets.h, replacing any previous block bounded by the
BEGIN/END markers below.
"""
import re
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
SOURCE_PNG = ROOT / "res/originaldoom/flats/FLOOR7_2.png"
OUTPUT_HEADER = ROOT / "src/generated_renderer_assets.h"

TILE_DIM = 4            # 4x4 tiles = 32x32 px
TILE_PX = TILE_DIM * 8  # 32

# Ordered tone ramp, dark -> light. Each pixel dithers between the two ramp
# entries bracketing its normalised luminance. Both entries are PAL0 indices
# and must stay in luminance order.
#   14 = 0x484038 (72,64,56) neutral dark olive-grey
#   15 = 0x4C6028 (76,96,40) the one green
# Mixed ~50/50 these read as (74,80,48), the muted grey-green of the reference.
TONE_RAMP = [14, 15]

# Clip the luminance histogram at these percentiles before normalising, so a
# handful of outlier pixels don't compress the whole mix range.
CLIP_LO = 0.02
CLIP_HI = 0.98

# 4x4 ordered (Bayer) dither matrix -- same family the flat renderer uses.
BAYER4 = [
    [0, 8, 2, 10],
    [12, 4, 14, 6],
    [3, 11, 1, 9],
    [15, 7, 13, 5],
]

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


def dither_to_indices(luminance):
    flat = sorted(v for row in luminance for v in row)
    n = len(flat)
    lo = flat[int(n * CLIP_LO)]
    hi = flat[min(n - 1, int(n * CLIP_HI))]
    span = max(1, hi - lo)
    steps = len(TONE_RAMP) - 1

    indices = [[0] * TILE_PX for _ in range(TILE_PX)]
    for y in range(TILE_PX):
        for x in range(TILE_PX):
            t = (luminance[y][x] - lo) / span
            t = 0.0 if t < 0.0 else (1.0 if t > 1.0 else t)
            pos = t * steps
            base = int(pos)
            if base >= steps:
                base, frac = steps - 1, 1.0
            else:
                frac = pos - base
            threshold = (BAYER4[y & 3][x & 3] + 0.5) / 16.0
            indices[y][x] = TONE_RAMP[base + (1 if frac > threshold else 0)]
    return indices


def main() -> int:
    image = Image.open(SOURCE_PNG).convert("L")
    assert image.size == (64, 64), image.size
    luminance = box_downsample_luminance(image, TILE_PX, TILE_PX)
    indices = dither_to_indices(luminance)

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
        "// box-downsampled 64x64 -> 32x32, then Bayer-dithered between PAL0 indices 14",
        "// and 15 so the two optically mix into a muted grey-green PAL0 has no swatch",
        "// for). Anchor tile index for screen tile (x,y) is",
        "// ((y & (DIM-1)) * DIM) + (x & (DIM-1)), so the 4x4 block repeats seamlessly",
        "// across the margins (see renderer_draw_backdrop in renderer_hud.c).",
        f"#define MEGALDOOM_BACKDROP_TILE_DIM {TILE_DIM}",
        "#define MEGALDOOM_BACKDROP_TILE_COUNT (MEGALDOOM_BACKDROP_TILE_DIM * MEGALDOOM_BACKDROP_TILE_DIM)",
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
          f"dithered over PAL0 {TONE_RAMP}, written to "
          f"{OUTPUT_HEADER.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
