#!/usr/bin/env python3
"""Generate the tiled rock backdrop that fills the margins around the 3D
viewport (see AGENTS.md's viewport presentation pass, 2026-07-30).

Source: res/originaldoom/flats/FLOOR7_2.png, the Doom 1 border flat (64x64).
Box-downsampled to 32x32 (4x4 tiles of 8x8 px) so the seamless-tiling edge
correspondence of the original 64x64 flat survives the resize, then quantized
to PAL0 -- the palette already resident for the bezel/HUD panel (see
src/renderer.c's load_game_palettes(), PAL_setColor(0..15)). PAL0 already
contains an olive/rock ramp (indices 2-8, 10, 14, 15), so no palette change is
needed and the backdrop shares VRAM-free palette slots with nothing else.

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

TILE_DIM = 4          # 4x4 tiles = 32x32 px
TILE_PX = TILE_DIM * 8  # 32

# PAL0, must stay byte-identical to src/renderer.c's load_game_palettes()
# PAL_setColor(0..15) calls.
PAL0 = [
    0x000000, 0xD8D8D8, 0x181410, 0x383030,
    0x585048, 0x888078, 0xB4ACA0, 0xE8E0D0,
    0x301E10, 0x4878A8, 0x78502C, 0xD8B048,
    0x982818, 0xA86838, 0x484038, 0x4C6028,
]
PAL0_RGB = [((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF) for c in PAL0]

BEGIN_MARKER = "// BEGIN generate-backdrop-tiles.py output"
END_MARKER = "// END generate-backdrop-tiles.py output"


def nearest_pal0_index(rgb):
    r, g, b = rgb
    best_index, best_dist = 0, None
    for index, (pr, pg, pb) in enumerate(PAL0_RGB):
        dist = (r - pr) ** 2 + (g - pg) ** 2 + (b - pb) ** 2
        if best_dist is None or dist < best_dist:
            best_dist, best_index = dist, index
    return best_index


def box_downsample(image, dst_w, dst_h):
    src_w, src_h = image.size
    assert src_w % dst_w == 0 and src_h % dst_h == 0, (
        "expects an integer downsample factor")
    fx, fy = src_w // dst_w, src_h // dst_h
    px = image.load()
    out = [[None] * dst_w for _ in range(dst_h)]
    for y in range(dst_h):
        for x in range(dst_w):
            rs = gs = bs = 0
            for sy in range(y * fy, y * fy + fy):
                for sx in range(x * fx, x * fx + fx):
                    r, g, b = px[sx, sy][:3]
                    rs += r
                    gs += g
                    bs += b
            n = fx * fy
            out[y][x] = (rs // n, gs // n, bs // n)
    return out


def main() -> int:
    image = Image.open(SOURCE_PNG).convert("RGB")
    assert image.size == (64, 64), image.size
    downsampled = box_downsample(image, TILE_PX, TILE_PX)
    indices = [[nearest_pal0_index(downsampled[y][x]) for x in range(TILE_PX)]
               for y in range(TILE_PX)]

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
