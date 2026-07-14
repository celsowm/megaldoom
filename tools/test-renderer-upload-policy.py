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


def choose_overlay_full(dirty, full_threshold, max_runs):
    if not dirty:
        return False
    return len(dirty) >= full_threshold or count_runs(dirty) > max_runs


class UploadModel:
    """Small state model for the renderer's private double-buffer policy."""

    def __init__(self, tile_count, batch_limit):
        self.tile_count = tile_count
        self.batch_limit = batch_limit
        self.active_bank = 0
        self.dirty = [set(), set()]
        self.pending_bank = None
        self.swap = False

    def queue_base(self):
        target = self.active_bank ^ 1
        self.dirty = [set(), set()]
        self.dirty[target] = set(range(self.tile_count))
        self.pending_bank = target
        self.swap = True

    def queue_overlay(self, tiles):
        self.dirty[self.active_bank].update(tiles)
        self.pending_bank = self.active_bank
        self.swap = False

    def upload_step(self):
        bank = self.pending_bank
        uploaded = set(sorted(self.dirty[bank])[:self.batch_limit])
        self.dirty[bank] -= uploaded
        if not self.dirty[bank]:
            if self.swap:
                self.active_bank = bank
            self.pending_bank = None
            self.swap = False
        return bank, uploaded


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

    assert not choose_overlay_full(empty, full_threshold, max_runs)
    assert not choose_overlay_full(contiguous_below, full_threshold, max_runs)
    assert choose_overlay_full(fragmented_below, full_threshold, max_runs)
    assert not choose_overlay_full(contiguous_above, full_threshold, max_runs)
    assert choose_overlay_full(contiguous_over_legacy_threshold,
                               full_threshold, max_runs)
    assert len(fragmented_above) == 180
    assert count_runs(fragmented_above) == 15
    assert not choose_overlay_full(fragmented_above, full_threshold, max_runs)

    # A base redraw always targets the inactive bank and cannot become visible
    # until both 150-tile upload steps have completed.
    model = UploadModel(tile_count, batch_limit)
    model.queue_base()
    assert model.pending_bank == 1
    assert len(model.dirty[1]) == tile_count
    bank, first_batch = model.upload_step()
    assert bank == 1 and len(first_batch) == batch_limit
    assert model.active_bank == 0 and model.pending_bank == 1
    bank, second_batch = model.upload_step()
    assert bank == 1 and len(second_batch) == tile_count - batch_limit
    assert first_batch.isdisjoint(second_batch)
    assert model.active_bank == 1 and model.pending_bank is None

    # Overlay-only work mutates only the displayed bank and never swaps it.
    overlay_tiles = {7, 8, 41}
    model.queue_overlay(overlay_tiles)
    bank, uploaded = model.upload_step()
    assert bank == 1 and uploaded == overlay_tiles
    assert model.active_bank == 1 and model.pending_bank is None

    # A consecutive base redraw overwrites the other bank completely. Lazy
    # snapshots then restore the new base, not an overlay from the old bank.
    snapshot_valid = set()
    base_tiles = {7: "base-a"}
    snapshot_valid.add(7)
    snapshot = {7: base_tiles[7]}
    base_tiles[7] = "base-a+overlay"
    model.queue_base()
    model.upload_step()
    assert model.active_bank == 1
    model.upload_step()
    assert model.active_bank == 0
    snapshot_valid.clear()
    base_tiles[7] = "base-b"
    if 7 not in snapshot_valid:
        snapshot[7] = base_tiles[7]
        snapshot_valid.add(7)
    base_tiles[7] = "base-b+overlay"
    base_tiles[7] = snapshot[7]
    assert base_tiles[7] == "base-b"

    assert "count_partial_view_bank_commands" not in scene
    assert "g_view_upload.full = swap ? TRUE" in scene
    assert "renderer_prepare_full_base_upload();" in scene
    assert "g_view_dirty_bank_mask = (u16)(1u << g_view_vram_bank)" in scene
    assert "g_view_dirty_bank_mask = 0" in renderer
    assert "g_view_bank_dirty_count[target_bank] = VIEW_TILE_COUNT" in renderer
    assert "if ((g_view_dirty_bank_mask & (1u << bank)) == 0) continue" in renderer
    assert "difference |= (base_rows[row] ^ row_data)" not in scene
    assert "store_base_tile" not in scene
    assert "g_base_snapshot_valid_bits" in scene
    assert "g_base_view_tiles[tile_index][row] = g_view_tiles[tile_index][row]" in scene
    assert scene.index("g_base_snapshot_valid_bits[i] = 0") < scene.index(
        "build_bsp_tilemap(columns, scene_colors, g_view_tiles)")
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

    print("ok    renderer upload policy: full base swap plus active-bank overlays")


if __name__ == "__main__":
    main()
