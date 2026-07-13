#!/usr/bin/env python3
"""Deterministic contract checks for view-bank DMA upload selection."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def define(source, name):
    match = re.search(rf"#define {name} (\d+)", source)
    assert match, name
    return int(match.group(1))


def count_runs(dirty):
    return sum(tile == 0 or tile - 1 not in dirty for tile in dirty)


def count_partial_commands(dirty, tile_count, batch_limit, split):
    commands = 0
    batch_tiles = 0
    tile = 0
    while tile < tile_count:
        if tile not in dirty:
            tile += 1
            continue
        first = tile
        while tile < tile_count and tile in dirty:
            tile += 1
        remaining = tile - first
        while remaining:
            if split and batch_tiles == batch_limit:
                batch_tiles = 0
            count = remaining
            if split:
                count = min(count, batch_limit - batch_tiles)
            commands += 1
            remaining -= count
            batch_tiles += count
    return commands


def choose_full(dirty, tile_count, batch_limit, full_threshold,
                max_runs, split):
    if not dirty:
        return False
    if split:
        partial = count_partial_commands(dirty, tile_count, batch_limit, True)
        full = 2 if tile_count > batch_limit else 1
        return full < partial
    return len(dirty) >= full_threshold or count_runs(dirty) > max_runs


def main():
    header = (ROOT / "src/renderer_internal.h").read_text()
    raycast = (ROOT / "src/raycast.h").read_text()
    scene = (ROOT / "src/renderer_scene.c").read_text()
    renderer = (ROOT / "src/renderer.c").read_text()
    tile_w = define(raycast, "RAY_VIEW_TILE_W")
    tile_h = define(raycast, "RAY_VIEW_TILE_H")
    tile_count = tile_w * tile_h
    batch_limit = define(header, "VIEW_DMA_TILES_PER_VBLANK")
    full_threshold = define(header, "VIEW_DIRTY_FULL_THRESHOLD")
    max_runs = define(header, "VIEW_DIRTY_MAX_RUNS")

    empty = set()
    contiguous_below = set(range(20, 100))
    fragmented_below = set(range(0, 80, 2))
    contiguous_above = set(range(0, 180))
    contiguous_over_legacy_threshold = set(range(0, 240))
    fragmented_above = {
        start + offset
        for start in range(0, 195, 13)
        for offset in range(12)
    }

    assert not choose_full(empty, tile_count, batch_limit,
                           full_threshold, max_runs, True)
    assert not choose_full(contiguous_below, tile_count, batch_limit,
                           full_threshold, max_runs, True)
    assert choose_full(fragmented_below, tile_count, batch_limit,
                       full_threshold, max_runs, True)
    assert not choose_full(contiguous_above, tile_count, batch_limit,
                           full_threshold, max_runs, True)
    assert not choose_full(contiguous_over_legacy_threshold, tile_count,
                           batch_limit, full_threshold, max_runs, True)
    assert len(fragmented_above) == 180
    assert count_runs(fragmented_above) == 15
    assert choose_full(fragmented_above, tile_count, batch_limit,
                       full_threshold, max_runs, True)
    # Active-bank uploads retain the existing thresholds and never use the new
    # inactive-bank two-vblank optimization.
    assert not choose_full(fragmented_above, tile_count, batch_limit,
                           full_threshold, max_runs, False)
    assert choose_full(contiguous_over_legacy_threshold, tile_count,
                       batch_limit, full_threshold, max_runs, False)

    assert "count_partial_view_bank_commands" in scene
    assert "full_commands < partial_commands" in scene
    assert "swap && dirty_count > 0" in scene
    assert "void renderer_queue_scene_upload" in scene
    assert "void renderer_upload_scene_step" in scene
    assert "bool renderer_scene_upload_pending" in scene
    assert "u16 budget = VIEW_DMA_TILES_PER_VBLANK" in scene
    assert not re.search(r"(?m)^\s*VDP_waitVSync\s*\(", scene)
    assert scene.index("finish_view_upload();") > scene.index("dbg_wait_dma();")
    assert scene.count("static u16 s_debug_total_vblanks;") == 1
    assert "static u16 s_debug_total_vblanks;" not in renderer
    assert "void renderer_debug_set_total_vblanks" in scene
    assert "void renderer_debug_set_total_vblanks" not in renderer

    print("ok    renderer upload policy: VBlank-stepped DMA and deferred bank swap")


if __name__ == "__main__":
    main()
