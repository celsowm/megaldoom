"""Differential contract for the exact packed billboard rasterizer."""

from dataclasses import dataclass
from pathlib import Path
import random
import sys


ROOT = Path(__file__).resolve().parent.parent
# renderer_scene.c was split by SRP into several files; the packer/billboard-
# draw code these checks look for now lives across that set.
SCENE_SPLIT_FILES = [
    "renderer_scene.c", "renderer_pack.c", "renderer_doors.c",
    "renderer_billboard_draw.c", "renderer_frame_overlay.c",
    "renderer_upload.c", "renderer_sparse.c",
    "renderer_flats.c",
]
VIEW_W = 32
VIEW_H = 24
STRIDE = 4
TILE_W = VIEW_W // 8


@dataclass(frozen=True)
class Door:
    depth: int
    top: int
    bottom: int
    # A window is see-through inside [band_top, band_bottom) (clamped to the
    # slab, as window_band_rows does); a door has no band.
    band_top: int | None = None
    band_bottom: int | None = None


@dataclass(frozen=True)
class Sprite:
    left: int
    right: int
    top: int
    bottom: int
    depth: int
    atlas_x: int
    atlas_y: int
    atlas_w: int
    atlas_h: int
    texture_w: int
    texture: tuple[int, ...]
    lut: tuple[int, ...]


def initial_pixels() -> list[list[int]]:
    return [[((x * 3 + y * 5) % 15) + 1 for x in range(VIEW_W)]
            for y in range(VIEW_H)]


def pack_pixels(pixels: list[list[int]]) -> list[list[int]]:
    rows = [[0] * 8 for _ in range((VIEW_H // 8) * TILE_W)]
    for y in range(VIEW_H):
        for tile_x in range(TILE_W):
            word = 0
            for pixel in range(8):
                word |= pixels[y][tile_x * 8 + pixel] << ((7 - pixel) * 4)
            rows[(y // 8) * TILE_W + tile_x][y & 7] = word
    return rows


def unpack_rows(rows: list[list[int]]) -> list[list[int]]:
    pixels = [[0] * VIEW_W for _ in range(VIEW_H)]
    for y in range(VIEW_H):
        for tile_x in range(TILE_W):
            word = rows[(y // 8) * TILE_W + tile_x][y & 7]
            for pixel in range(8):
                pixels[y][tile_x * 8 + pixel] = (
                    word >> ((7 - pixel) * 4)) & 0xF
    return pixels


def door_blocks(doors: dict[int, Door], wall_col: int,
                sprite_depth: int, y: int) -> bool:
    door = doors.get(wall_col)
    if not (door and sprite_depth >= door.depth and door.top <= y < door.bottom):
        return False
    if door.band_top is not None:
        band_top = max(door.band_top, door.top)
        band_bottom = max(min(door.band_bottom, door.bottom), band_top)
        if band_top <= y < band_bottom:
            return False
    return True


def door_blocked_rows(doors: dict[int, Door], wall_col: int,
                      sprite_depth: int) -> list[tuple[int, int]]:
    """Mirror of door_overlay_blocked_rows(): the blocked rows as half-open
    intervals, computed independently of door_blocks() so the fuzz compares the
    two formulations."""
    door = doors.get(wall_col)
    if not (door and sprite_depth >= door.depth):
        return []
    if door.band_top is None:
        return [(door.top, door.bottom)]
    # window_band_rows, then the helper's clamp into the slab.
    band_top = max(door.band_top, door.top)
    band_bottom = min(door.band_bottom, door.bottom)
    if band_bottom < band_top:
        band_bottom = band_top
    band_top = min(band_top, door.bottom)
    band_bottom = max(band_bottom, band_top)
    return [(door.top, band_top), (band_bottom, door.bottom)]


def draw_reference(sprites: list[Sprite], depths: list[int],
                   doors: dict[int, Door]) -> tuple[list[list[int]], set[int]]:
    """Previous column-first renderer: one packed-pixel RMW at a time."""
    pixels = initial_pixels()
    dirty: set[int] = set()
    for sprite in sprites:
        width = sprite.right - sprite.left + 1
        height = sprite.bottom - sprite.top + 1
        if width <= 0 or height <= 0:
            continue
        x_step = (sprite.atlas_w << 8) // width
        y_step = (sprite.atlas_h << 16) // height
        x_acc = sprite.atlas_x << 8
        atlas_x_last = sprite.atlas_x + sprite.atlas_w - 1
        atlas_y_last = sprite.atlas_y + sprite.atlas_h - 1
        y0 = max(sprite.top, 0)
        y1 = min(sprite.bottom, VIEW_H - 1)
        for x in range(sprite.left, sprite.right + 1):
            tex_x = min(x_acc >> 8, atlas_x_last)
            x_acc += x_step
            if x < 0 or x >= VIEW_W or y0 > y1:
                continue
            wall_col = x & ~(STRIDE - 1)
            if sprite.depth >= depths[wall_col]:
                continue
            y_acc = (sprite.atlas_y << 16) + (y0 - sprite.top) * y_step
            for y in range(y0, y1 + 1):
                tex_y = min(y_acc >> 16, atlas_y_last)
                y_acc += y_step
                source = sprite.texture[tex_y * sprite.texture_w + tex_x] & 0xF
                texel = sprite.lut[source]
                if texel and not door_blocks(doors, wall_col, sprite.depth, y):
                    pixels[y][x] = texel
                    dirty.add((y // 8) * TILE_W + (x // 8))
    return pixels, dirty


def draw_packed(sprites: list[Sprite], depths: list[int],
                doors: dict[int, Door]) -> tuple[list[list[int]], set[int]]:
    """Retired row-first renderer (pre-2026-08-03): accumulates a 32-bit tile
    word with variable shifts, one RMW per affected tile row. Kept as a second
    independent model of the same contract."""
    rows = pack_pixels(initial_pixels())
    dirty: set[int] = set()
    for sprite in sprites:
        width = sprite.right - sprite.left + 1
        height = sprite.bottom - sprite.top + 1
        if width <= 0 or height <= 0:
            continue
        x0 = max(sprite.left, 0)
        x1 = min(sprite.right, VIEW_W - 1)
        y0 = max(sprite.top, 0)
        y1 = min(sprite.bottom, VIEW_H - 1)
        if x0 > x1 or y0 > y1:
            continue

        x_step = (sprite.atlas_w << 8) // width
        x_acc = (sprite.atlas_x << 8) + (x0 - sprite.left) * x_step
        atlas_x_last = sprite.atlas_x + sprite.atlas_w - 1
        tex_x_by_col: dict[int, int] = {}
        for x in range(x0, x1 + 1):
            tex_x = min(x_acc >> 8, atlas_x_last)
            x_acc += x_step
            wall_col = x & ~(STRIDE - 1)
            if sprite.depth < depths[wall_col]:
                tex_x_by_col[x] = tex_x

        y_step = (sprite.atlas_h << 16) // height
        y_acc = (sprite.atlas_y << 16) + (y0 - sprite.top) * y_step
        atlas_y_last = sprite.atlas_y + sprite.atlas_h - 1
        for y in range(y0, y1 + 1):
            tex_y = min(y_acc >> 16, atlas_y_last)
            y_acc += y_step
            for tile_x in range(x0 // 8, x1 // 8 + 1):
                clear_mask = 0
                value = 0
                for x in range(max(x0, tile_x * 8), min(x1, tile_x * 8 + 7) + 1):
                    tex_x = tex_x_by_col.get(x)
                    if tex_x is None:
                        continue
                    wall_col = x & ~(STRIDE - 1)
                    source = sprite.texture[
                        tex_y * sprite.texture_w + tex_x] & 0xF
                    texel = sprite.lut[source]
                    if texel and not door_blocks(doors, wall_col, sprite.depth, y):
                        shift = (7 - (x & 7)) * 4
                        clear_mask |= 0xF << shift
                        value |= texel << shift
                if clear_mask:
                    tile_index = (y // 8) * TILE_W + tile_x
                    row_y = y & 7
                    dirty.add(tile_index)
                    rows[tile_index][row_y] = (
                        (rows[tile_index][row_y] & ~clear_mask) | value)
    return unpack_rows(rows), dirty


def draw_bytewise(sprites: list[Sprite], depths: list[int],
                  doors: dict[int, Door]) -> tuple[list[list[int]], set[int]]:
    """Shipping renderer: one byte (two screen pixels) of a packed tile row at a
    time, with out-of-range columns pre-marked in the screen->texel map so the
    inner loop needs no bounds test. Mirrors raster_sprite_row in
    src/renderer/renderer_billboard_draw.c."""
    rows = pack_pixels(initial_pixels())
    dirty: set[int] = set()
    for sprite in sprites:
        width = sprite.right - sprite.left + 1
        height = sprite.bottom - sprite.top + 1
        if width <= 0 or height <= 0:
            continue
        x0 = max(sprite.left, 0)
        x1 = min(sprite.right, VIEW_W - 1)
        y0 = max(sprite.top, 0)
        y1 = min(sprite.bottom, VIEW_H - 1)
        if x0 > x1 or y0 > y1:
            continue

        x_step = (sprite.atlas_w << 8) // width
        x_acc = (sprite.atlas_x << 8) + (x0 - sprite.left) * x_step
        atlas_x_last = sprite.atlas_x + sprite.atlas_w - 1
        # SKIP is the C code's 0xFF sentinel.
        tex_x_by_col: dict[int, int | None] = {}
        for x in range(x0, x1 + 1):
            tex_x = min(x_acc >> 8, atlas_x_last)
            x_acc += x_step
            wall_col = x & ~(STRIDE - 1)
            tex_x_by_col[x] = tex_x if sprite.depth < depths[wall_col] else None
        if x0 & 1:
            tex_x_by_col[x0 - 1] = None
        if (x1 & 1) == 0:
            tex_x_by_col[x1 + 1] = None

        first_byte = x0 >> 1
        last_byte = x1 >> 1
        y_step = (sprite.atlas_h << 16) // height
        y_acc = (sprite.atlas_y << 16) + (y0 - sprite.top) * y_step
        atlas_y_last = sprite.atlas_y + sprite.atlas_h - 1
        for y in range(y0, y1 + 1):
            tex_y = min(y_acc >> 16, atlas_y_last)
            y_acc += y_step
            for b in range(first_byte, last_byte + 1):
                mask = 0
                value = 0
                for half in (0, 1):
                    x = b * 2 + half
                    tex_x = tex_x_by_col.get(x)
                    if tex_x is None:
                        continue
                    source = sprite.texture[
                        tex_y * sprite.texture_w + tex_x] & 0xF
                    texel = sprite.lut[source]
                    if texel:
                        mask |= 0xF0 >> (half * 4)
                        value |= texel << (4 - half * 4)
                if not mask:
                    continue
                wall_col = (b * 2) & ~(STRIDE - 1)
                if door_blocks(doors, wall_col, sprite.depth, y):
                    continue
                tile_x = b // 4
                tile_index = (y // 8) * TILE_W + tile_x
                row_y = y & 7
                lane = b & 3
                shift = (3 - lane) * 8
                dirty.add(tile_index)
                rows[tile_index][row_y] = (
                    (rows[tile_index][row_y] & ~(mask << shift)) |
                    (value << shift))
    return unpack_rows(rows), dirty


def draw_columnwise(sprites: list[Sprite], depths: list[int],
                    doors: dict[int, Door]) -> tuple[list[list[int]], set[int]]:
    """Shipping renderer since 2026-09-18: one BYTE COLUMN at a time, rows
    resolved through a precomputed per-row texel offset, door/window slabs cut
    out as row intervals, and the keep-mask a pure function of the packed byte
    (valid because every remap sends 0 to 0 and non-zero to non-zero). Mirrors
    draw_sprite_columns in src/renderer/renderer_billboard_draw.c."""
    rows = pack_pixels(initial_pixels())
    dirty: set[int] = set()
    for sprite in sprites:
        width = sprite.right - sprite.left + 1
        height = sprite.bottom - sprite.top + 1
        if width <= 0 or height <= 0:
            continue
        x0 = max(sprite.left, 0)
        x1 = min(sprite.right, VIEW_W - 1)
        y0 = max(sprite.top, 0)
        y1 = min(sprite.bottom, VIEW_H - 1)
        if x0 > x1 or y0 > y1:
            continue
        x_step = (sprite.atlas_w << 8) // width
        x_acc = (sprite.atlas_x << 8) + (x0 - sprite.left) * x_step
        atlas_x_last = sprite.atlas_x + sprite.atlas_w - 1
        tex_x_by_col: dict[int, int | None] = {}
        for x in range(x0, x1 + 1):
            tex_x = min(x_acc >> 8, atlas_x_last)
            x_acc += x_step
            wall_col = x & ~(STRIDE - 1)
            tex_x_by_col[x] = tex_x if sprite.depth < depths[wall_col] else None
        y_step = (sprite.atlas_h << 16) // height
        y_acc = (sprite.atlas_y << 16) + (y0 - sprite.top) * y_step
        atlas_y_last = sprite.atlas_y + sprite.atlas_h - 1
        row_offset = []
        for _ in range(y0, y1 + 1):
            row_offset.append(min(y_acc >> 16, atlas_y_last) * sprite.texture_w)
            y_acc += y_step
        hi = [value << 4 for value in sprite.lut]
        clear = [0] * 16
        for b in range(x0 >> 1, (x1 >> 1) + 1):
            tx0 = tex_x_by_col.get(b * 2)
            tx1 = tex_x_by_col.get(b * 2 + 1)
            if tx0 is None and tx1 is None:
                continue
            src_hi = tx0 if tx0 is not None else tx1
            src_lo = tx1 if tx1 is not None else tx0
            tab_hi = hi if tx0 is not None else clear
            tab_lo = sprite.lut if tx1 is not None else clear
            wall_col = (b * 2) & ~(STRIDE - 1)
            segments = []
            start = y0
            for cut_begin, cut_end in door_blocked_rows(doors, wall_col, sprite.depth):
                if min(cut_begin, y1 + 1) > start:
                    segments.append((start, min(cut_begin, y1 + 1)))
                start = max(start, cut_end)
            if start <= y1:
                segments.append((start, y1 + 1))
            tile_x = b // 4
            shift = (3 - (b & 3)) * 8
            for seg_begin, seg_end in segments:
                for y in range(seg_begin, seg_end):
                    offset = row_offset[y - y0]
                    value = (tab_hi[sprite.texture[offset + src_hi] & 0xF] |
                             tab_lo[sprite.texture[offset + src_lo] & 0xF])
                    if not value:
                        continue
                    keep = ((0xF0 if value & 0xF0 else 0) |
                            (0x0F if value & 0x0F else 0)) ^ 0xFF
                    tile_index = (y // 8) * TILE_W + tile_x
                    dirty.add(tile_index)
                    word = rows[tile_index][y & 7]
                    byte = (((word >> shift) & 0xFF) & keep) | value
                    rows[tile_index][y & 7] = (word & ~(0xFF << shift)) | (byte << shift)
    return unpack_rows(rows), dirty


def make_sprite(rng: random.Random, left: int, right: int, top: int,
                bottom: int, depth: int, atlas_w: int, atlas_h: int) -> Sprite:
    texture_w = atlas_w + 3
    texture_h = atlas_h + 2
    texture = tuple(0 if (x + y * 2) % 5 == 0 else
                    ((x * 7 + y * 3) % 15) + 1
                    for y in range(texture_h) for x in range(texture_w))
    # Use a deterministic non-identity remap while preserving transparent zero.
    lut = tuple([0] + [((value + depth) % 15) + 1 for value in range(1, 16)])
    return Sprite(left, right, top, bottom, depth, 1, 1, atlas_w, atlas_h,
                  texture_w, texture, lut)


def assert_equal(sprites: list[Sprite], depths: list[int],
                 doors: dict[int, Door], label: str) -> None:
    reference_pixels, reference_dirty = draw_reference(sprites, depths, doors)
    packed_pixels, packed_dirty = draw_packed(sprites, depths, doors)
    if reference_pixels != packed_pixels:
        raise ValueError(f"packed raster pixels diverged for {label}")
    if reference_dirty != packed_dirty:
        raise ValueError(f"dirty tile accounting diverged for {label}")
    byte_pixels, byte_dirty = draw_bytewise(sprites, depths, doors)
    if reference_pixels != byte_pixels:
        bad = [(y, x, reference_pixels[y][x], byte_pixels[y][x])
               for y in range(VIEW_H) for x in range(VIEW_W)
               if reference_pixels[y][x] != byte_pixels[y][x]]
        raise ValueError(
            f"bytewise raster pixels diverged for {label}: "
            f"{len(bad)} px, first (y,x,ref,got)={bad[0]}")
    if reference_dirty != byte_dirty:
        raise ValueError(
            f"bytewise dirty tile accounting diverged for {label}: "
            f"only-ref={sorted(reference_dirty - byte_dirty)} "
            f"only-byte={sorted(byte_dirty - reference_dirty)}")
    column_pixels, column_dirty = draw_columnwise(sprites, depths, doors)
    if reference_pixels != column_pixels:
        bad = [(y, x, reference_pixels[y][x], column_pixels[y][x])
               for y in range(VIEW_H) for x in range(VIEW_W)
               if reference_pixels[y][x] != column_pixels[y][x]]
        raise ValueError(
            f"column raster pixels diverged for {label}: "
            f"{len(bad)} px, first (y,x,ref,got)={bad[0]}")
    if reference_dirty != column_dirty:
        raise ValueError(f"column dirty tile accounting diverged for {label}")


def main() -> int:
    scene = "\n".join((ROOT / "src" / "renderer" / name).read_text(encoding="utf-8")
                      for name in SCENE_SPLIT_FILES)
    required = [
        "tex_x_by_screen_col[RAY_VIEW_COLS_MAX]", "raster_sprite_row",
        "renderer_mark_overlay_tile(tile_index)",
        "*dst = (u8)((*dst & (u8)~mask) | value)",
        "door_overlay_blocks_pixel", "columns[wall_col].depth",
        # view_tile_index is column-major, so stepping one tile column is
        # += VIEW_TILE_STRIDE, not ++. Getting this wrong wrote into an
        # unrelated tile and is exactly what the on-target differential harness
        # caught. It must be the STRIDE (the allocated RAY_VIEW_TILE_H_MAX) and
        # never the selected VIEW_TILE_H: with a viewport shorter than the
        # maximum those differ, and stepping by the short height walks into the
        # middle of the next column.
        "tile_index + VIEW_TILE_STRIDE",
        # The column rasterizer (default) and what it rests on: the per-byte
        # keep table, the door/window row intervals, and one tile mark before
        # the first write of each tile.
        "draw_sprite_columns", "BB_KEEP_MASK[v]", "door_overlay_blocked_rows",
        "renderer_mark_overlay_tile(tile_index);",
    ]
    if any(token not in scene for token in required):
        raise ValueError("byte-wise packed billboard renderer contract changed")

    rng = random.Random(0x32D00F)
    depths = [0x7FFF] * VIEW_W
    for x in range(8, 16):
        depths[x] = 120
    doors = {16: Door(150, 3, 13), 20: Door(150, 3, 13)}

    # Far-to-near painter order. Together these exercise all four clip edges,
    # transparent texels, wall blocks, a door slab/lower gap, and overlap.
    fixed = [
        make_sprite(rng, -5, 18, -4, 19, 180, 11, 13),
        make_sprite(rng, 12, 35, 2, 27, 140, 13, 15),
        make_sprite(rng, 5, 25, 7, 22, 70, 9, 10),
    ]
    assert_equal(fixed, depths, doors, "clipping/transparency/walls/doors/overlap")

    # Differential fuzz holds painter order stable while varying every bound,
    # depth and source crop; fixed seed keeps failures reproducible.
    for case in range(160):
        case_depths = [0x7FFF] * VIEW_W
        for wall_col in range(0, VIEW_W, STRIDE):
            depth = rng.choice((72, 110, 160, 240, 0x7FFF))
            for x in range(wall_col, wall_col + STRIDE):
                case_depths[x] = depth
        case_doors = {}
        for wall_col in range(0, VIEW_W, STRIDE):
            if rng.randrange(5) != 0:
                continue
            top = rng.randrange(0, 10)
            bottom = rng.randrange(14, VIEW_H + 1)
            if rng.randrange(2):
                # Windows, including bands partly or wholly outside the slab.
                band_top = rng.randrange(-2, VIEW_H + 4)
                band_bottom = band_top + rng.randrange(0, 12)
                case_doors[wall_col] = Door(rng.choice((90, 140, 220)), top,
                                            bottom, band_top, band_bottom)
            else:
                case_doors[wall_col] = Door(rng.choice((90, 140, 220)), top, bottom)
        sprites = []
        for _ in range(rng.randrange(1, 6)):
            left = rng.randrange(-12, VIEW_W + 4)
            top = rng.randrange(-10, VIEW_H + 3)
            sprites.append(make_sprite(
                rng, left, left + rng.randrange(3, 28),
                top, top + rng.randrange(3, 30),
                rng.randrange(40, 260), rng.randrange(2, 15), rng.randrange(2, 17)))
        sprites.sort(key=lambda item: item.depth, reverse=True)
        assert_equal(sprites, case_depths, case_doors, f"fuzz case {case}")

    print("ok    column, byte-wise and row-first rasters all match the pixel "
          "reference across clipping, depth, doors, windows and painter order")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(f"error {error}", file=sys.stderr)
        raise SystemExit(1)
