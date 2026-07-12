#!/usr/bin/env python3
"""Deterministic contracts for the tile-native sector frame pipeline."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCENE_FAR = 0xFFFF


def define(source, name):
    match = re.search(rf"#define {name} (\d+)", source)
    assert match, name
    return int(match.group(1))


def rep4(color):
    return (color & 15) * 0x1111


def put_sample(row, sample_x, color):
    half = rep4(color)
    return ((half << 16) | (row & 0xFFFF)) if sample_x % 2 == 0 \
        else ((row & 0xFFFF0000) | half)


def signed_divide(numerator, denominator):
    magnitude = abs(numerator) // abs(denominator)
    return -magnitude if (numerator < 0) != (denominator < 0) else magnitude


class LazyDepth:
    def __init__(self, sample_cols, tile_h):
        self.depth = [[[SCENE_FAR] * 8 for _ in range(tile_h)]
                      for _ in range(sample_cols)]
        self.stamp = [[0] * tile_h for _ in range(sample_cols)]
        self.generation = 1

    def block(self, sample_x, tile_y):
        if self.stamp[sample_x][tile_y] != self.generation:
            return [SCENE_FAR] * 8
        return self.depth[sample_x][tile_y]

    def ensure(self, sample_x, tile_y):
        if self.stamp[sample_x][tile_y] != self.generation:
            self.depth[sample_x][tile_y] = [SCENE_FAR] * 8
            self.stamp[sample_x][tile_y] = self.generation
        return self.depth[sample_x][tile_y]

    def next_frame(self):
        self.generation = (self.generation + 1) & 0xFF
        if self.generation == 0:
            self.stamp = [[0] * len(self.stamp[0]) for _ in self.stamp]
            self.generation = 1


def main():
    raycast = (ROOT / "src/raycast.h").read_text()
    sector = (ROOT / "src/sector_render.c").read_text()
    renderer = (ROOT / "src/renderer_scene.c").read_text()
    generated = (ROOT / "src/generated_e1m1_map.c").read_text()
    extractor = (ROOT / "tools/wad-map-extract.py").read_text()
    tile_w = define(raycast, "RAY_VIEW_TILE_W")
    tile_h = define(raycast, "RAY_VIEW_TILE_H")
    stride = define(raycast, "RAY_COL_STRIDE")
    rows = tile_h * 8
    sample_cols = tile_w * 8 // stride

    ceiling, floor = 3, 12
    flat_rows = [rep4(ceiling if y < rows // 2 else floor) * 0x10001
                 for y in range(rows)]
    assert flat_rows[rows // 2 - 1] == 0x33333333
    assert flat_rows[rows // 2] == 0xCCCCCCCC

    row = 0
    row = put_sample(row, 0, 2)
    row = put_sample(row, 1, 13)
    assert row == 0x2222DDDD

    for numerator, denominator in (
            (0, 1), (32767, 7), (-32767, 7), (32767, -7),
            (0x12345678, 255), (-0x1234567, 4096), (-0x80000000, 1)):
        assert signed_divide(numerator, denominator) == int(
            abs(numerator) // abs(denominator) *
            (-1 if (numerator < 0) != (denominator < 0) else 1))

    laterals = [-300, 120, -40, 250]
    depths = [700, 300, 200, 900]
    min_index = min(range(4), key=lambda i: laterals[i] / depths[i])
    max_index = max(range(4), key=lambda i: laterals[i] / depths[i])
    selected_min = 0
    selected_max = 0
    for i in range(1, 4):
        if laterals[i] * depths[selected_min] < laterals[selected_min] * depths[i]:
            selected_min = i
        if laterals[i] * depths[selected_max] > laterals[selected_max] * depths[i]:
            selected_max = i
    assert (selected_min, selected_max) == (min_index, max_index)

    # The runtime clips first and indexes the generated table with the clipped
    # height, exactly matching floor(relative_y * 32 / height).
    clipped_top, clipped_bottom = 5, 11
    height = clipped_bottom - clipped_top + 1
    assert [rel * 32 // height for rel in range(height)] == [0, 4, 9, 13, 18, 22, 27]

    lazy = LazyDepth(sample_cols, tile_h)
    assert lazy.block(3, 4) == [SCENE_FAR] * 8
    lazy.ensure(3, 4)[2] = 77
    assert lazy.block(3, 4)[2] == 77
    lazy.next_frame()
    assert lazy.block(3, 4)[2] == SCENE_FAR
    for _ in range(254):
        lazy.next_frame()
    assert lazy.generation == 1
    assert lazy.block(3, 4)[2] == SCENE_FAR

    cache_generation = [0]
    generation = 1
    computations = 0
    if cache_generation[0] != generation:
        computations += 1
        cache_generation[0] = generation
    if cache_generation[0] != generation:
        computations += 1
    generation += 1
    if cache_generation[0] != generation:
        computations += 1
    assert computations == 2

    assert "g_scene_color" not in sector
    assert "memset(g_scene_depth" not in sector
    assert "MEGALDOOM_WALL_TEX_Y_BY_HEIGHT[height]" in sector
    assert "color_rows[row * 2] = SECTOR_REP4[color]" in sector
    assert "g_depth_block_generation" in sector
    assert "g_vertex_generation" in sector
    assert "sector_divu32_16_exact" in sector
    assert "build_sector_tilemap" not in renderer
    assert "memcpy(g_view_tiles, g_base_view_tiles, sizeof(g_view_tiles))" not in renderer
    assert "g_base_view_tiles[tile_index][row] = g_view_tiles[tile_index][row]" in renderer
    assert "const u16 bsp_vertex_count = 467u;" in generated
    assert 'lines.append("const u16 bsp_vertex_count = %du;" % len(vertices))' in extractor

    print("ok    tile-native sector packing, lazy depth, and vertex-cache contracts")


if __name__ == "__main__":
    main()
