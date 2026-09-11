#!/usr/bin/env python3
"""Deterministic contract checks for view-bank DMA upload selection."""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import raycast_constants

ROOT = Path(__file__).resolve().parents[1]


def define(source, name):
    match = re.search(rf"#define {name} (\d+)", source)
    assert match, name
    return int(match.group(1))


def count_runs(dirty):
    return sum(tile == 0 or tile - 1 not in dirty for tile in dirty)


# The viewport is runtime-selectable; the DEFAULT preset is what a fresh boot
# uploads, and VIEW_TILE_STRIDE (the allocated maximum height) is the column
# pitch every tile index is built from.
VIEW_TILE_W, VIEW_TILE_H = raycast_constants.view_tiles()
VIEW_TILE_STRIDE = raycast_constants.view_tiles_max()[1]


def check_damage_overlay_frame(assets):
    """The damage/low-health ops must land on the viewport edges, per preset.

    Same failure mode as check_overlay_column_mask above, one layout change
    later. The generator built each op's destination as
    `tile_x * VIEW_TILE_H + tile_y`, which was the real layout until 4459217
    made the viewport resizable and pinned the column pitch at
    VIEW_TILE_STRIDE == RAY_VIEW_TILE_H_MAX. After that every op past column 0
    was written one tile per column too low: the intended border frame rendered
    as a red staircase across the scene, 1728 of its 2784 pixels in the wrong
    place, and nothing failed because no test read this table.

    So re-derive the frame from the geometry -- 3px top/bottom, 8px left/right
    on the viewport's own edges -- and require the baked ops to paint exactly
    that set under the runtime's own index rule, for every preset. The op sets
    are per-preset because they carry absolute g_view_tiles offsets.
    """
    size_count = raycast_constants.view_size_count()
    assert define(assets, "MEGALDOOM_OVERLAY_SIZE_COUNT") == size_count
    counts = re.search(
        r"MEGALDOOM_OVERLAY_OP_COUNT\[MEGALDOOM_OVERLAY_SIZE_COUNT\]\[2\]"
        r"\s*=\s*\{(.*?)\};", assets, re.S)
    assert counts, "per-preset overlay op counts"
    counts = [(int(d), int(l)) for d, l in
              re.findall(r"\{(\d+),\s*(\d+)\}", counts.group(1))]
    assert len(counts) == size_count, counts

    for name, kind in (("MEGALDOOM_DAMAGE_OVERLAY_OPS", 0),
                       ("MEGALDOOM_LOW_HEALTH_OVERLAY_OPS", 1)):
        body = assets[assets.index(name + "[MEGALDOOM_OVERLAY_SIZE_COUNT]"):]
        body = body[:body.index("\n};")]
        groups = re.findall(r"\{\n(.*?)\n  \},", body, re.S)
        assert len(groups) == size_count, (name, len(groups))
        for size_index, group in enumerate(groups):
            ops = [(int(d), int(m, 16)) for d, m in
                   re.findall(r"\{\s*(\d+),\s*0x([0-9A-Fa-f]+),", group)]
            ops = ops[:counts[size_index][kind]]
            width, height = raycast_constants.view_pixels(size_index)
            painted = set()
            for dst, mask in ops:
                tile, row = divmod(dst, 8)
                tile_x, tile_y = divmod(tile, VIEW_TILE_STRIDE)
                y = tile_y * 8 + row
                for pixel in range(8):
                    if (mask >> ((7 - pixel) * 4)) & 0xF:
                        painted.add((tile_x * 8 + pixel, y))
            if kind == 0:
                expected = {(x, edge) for x in range(width) for t in range(3)
                            for edge in (t, height - 1 - t)}
                expected |= {(edge, y) for y in range(height) for t in range(8)
                             for edge in (t, width - 1 - t)}
            else:
                expected = {(x, y)
                            for y in (1, 2, height - 3, height - 2)
                            for left in range(16, 33)
                            for x in (left, width - 1 - left)}
            assert painted == expected, (
                name, size_index, len(painted - expected), len(expected - painted))


def check_overlay_column_mask(overlay, mark_overlay):
    """The overlay column mask must follow the COLUMN-major g_view_tiles layout.

    build_bsp_tilemap() force-repacks any column that carried an overlay last
    frame; a coherence-skipped column otherwise keeps the sprite's pixels baked
    into the framebuffer after the sprite moved -- a ghost clone that only
    clears once that column's wall descriptors happen to change. The mask was
    written when g_view_tiles was row-major (tile_index % VIEW_TILE_W == the
    column); the 2026-07-19 column-major conversion did not touch this file, so
    the mask indexed the wrong column for two weeks. Re-derive it here.
    """
    if "tile_index % VIEW_TILE_W" in mark_overlay:
        raise AssertionError(
            "overlay column mask is using row-major arithmetic on a "
            "column-major tile index (this is the ghost-clone bug)")

    # The 300-byte s_tile_column run table is gone: the column pitch is now the
    # power-of-two VIEW_TILE_STRIDE, so tile -> column is a shift and the table
    # it was bought to avoid a divide-by-15 for is no longer worth its bytes.
    # What still has to hold is that the mask divides by the COLUMN PITCH, and
    # not by the currently selected height nor by the width.
    if "s_tile_column" in overlay:
        raise AssertionError(
            "s_tile_column is back; the column mask should divide by "
            "VIEW_TILE_STRIDE instead")
    if ("#define OVERLAY_TILE_COLUMN(tile_index) ((tile_index) / VIEW_TILE_STRIDE)"
            not in overlay):
        raise AssertionError(
            "overlay column mask must map a tile index to its column by "
            "dividing by VIEW_TILE_STRIDE")
    if ("s_cur_overlay_columns |= (u32)1u << OVERLAY_TILE_COLUMN(tile_index);"
            not in mark_overlay):
        raise AssertionError(
            "the overlay column mask no longer uses OVERLAY_TILE_COLUMN")
    # The pitch must be the ALLOCATED maximum height. If it were the selected
    # height, a viewport shorter than the maximum would map tiles to the wrong
    # column -- the same class of bug as the row-major mask above.
    if VIEW_TILE_STRIDE != raycast_constants.view_tiles_max()[1]:
        raise AssertionError(
            "column pitch must be RAY_VIEW_TILE_H_MAX, not the selected height")


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


def compose_overlay(previous, current):
    """Model CPU tile writes after the redundant per-touch restore is removed."""
    restored = set(previous)
    touched = set(current)
    overlap = restored & touched
    return restored, touched, overlap


def main():
    header = (ROOT / "src/renderer/renderer_internal.h").read_text()
    hud_assets = (ROOT / "src/renderer/generated_hud_assets.h").read_text()
    raycast = (ROOT / "src/raycast.h").read_text()
    # renderer_scene.c was split by SRP into several files; the upload
    # scheduler code these checks look for now lives across that set.
    scene = "\n".join((ROOT / "src/renderer" / n).read_text() for n in (
        "renderer_scene.c", "renderer_pack.c", "renderer_doors.c",
        "renderer_billboard_draw.c", "renderer_frame_overlay.c",
        "renderer_upload.c", "renderer_sparse.c",
        "renderer_flats.c",
    ))
    overlay = (ROOT / "src/renderer/renderer_overlay.c").read_text()
    renderer = (ROOT / "src/renderer/renderer.c").read_text()
    perf = (ROOT / "src/renderer/renderer_perf.c").read_text()
    tile_w, tile_h = raycast_constants.view_tiles()
    # A base upload ships the LIVE tiles only. Each column is allocated
    # VIEW_TILE_STRIDE slots but shows only its first tile_h, and the padding is
    # never displayed -- shipping it would cost the default viewport 320 tiles
    # instead of 300 and push its base upload from two vblank steps to three.
    # The full path therefore emits one run per column and skips each tail.
    assert tile_h <= VIEW_TILE_STRIDE
    tile_count = tile_w * tile_h
    upload = (ROOT / "src/renderer/renderer_upload.c").read_text()
    if "view_tile_next_live(g_view_upload.cursor)" not in upload:
        raise AssertionError(
            "the full-upload path must skip each column's padding tail")
    if "if (!view_tile_is_live(tile)) continue;" not in renderer:
        raise AssertionError("padding tiles must never be marked dirty")
    batch_limit = define(header, "VIEW_DMA_TILES_PER_VBLANK")
    full_threshold = define(header, "VIEW_DIRTY_FULL_THRESHOLD")
    max_runs = define(header, "VIEW_DIRTY_MAX_RUNS")
    assert define(hud_assets, "FREEDOOM_FACE_TILE_W") == 4
    assert "HUD_FACE_TILE_X ((SCREEN_TILE_W - FREEDOOM_FACE_TILE_W) / 2)" in header

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

    # The overlay-only render restores the complete previous footprint once.
    # Current touches then draw on that clean buffer without a second base copy.
    restored, touched, overlap = compose_overlay({7, 8, 41}, {8, 9})
    assert restored == {7, 8, 41}
    assert touched == {8, 9}
    assert overlap == {8}
    restored, touched, overlap = compose_overlay({7, 8}, set())
    assert restored == {7, 8} and not touched and not overlap
    restored, touched, overlap = compose_overlay(set(), {12, 13})
    assert not restored and touched == {12, 13} and not overlap

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
    # The dirty COUNT is the on-screen tile count, not the padded index span:
    # VIEW_TILE_COUNT would overstate the workload by one tile per column at any
    # viewport shorter than VIEW_TILE_STRIDE.
    assert "g_view_bank_dirty_count[target_bank] = VIEW_TILE_LIVE_COUNT" in renderer
    assert "if ((g_view_dirty_bank_mask & (1u << bank)) == 0) continue" in renderer
    assert "difference |= (base_rows[row] ^ row_data)" not in scene
    assert "store_base_tile" not in scene
    assert "OVERLAY_SNAPSHOT_TILE_LIMIT 128" in overlay
    assert "s_snapshot_rows[slot][row] = g_view_tiles[tile_index][row]" in overlay
    assert "renderer_overlay_requires_base_rebuild" in scene + overlay
    assert "g_base_built_this_frame" not in scene + overlay
    mark_overlay = overlay[overlay.index("void renderer_mark_overlay_tile"):]
    assert "g_view_tiles[tile_index][row] = s_snapshot_rows" not in mark_overlay
    check_overlay_column_mask(overlay, mark_overlay)
    check_damage_overlay_frame(
        (ROOT / "src/renderer/generated_renderer_assets.h").read_text())
    # The runtime must pick the set for the preset actually in use.
    assert "MEGALDOOM_DAMAGE_OVERLAY_OPS[view_size]" in scene
    assert "MEGALDOOM_OVERLAY_OP_COUNT[view_size][0]" in scene
    assert "MEGALDOOM_LOW_HEALTH_OVERLAY_OPS[view_size]" in scene
    assert scene.index("renderer_overlay_restore_previous();") < scene.index(
        "draw_projected_billboards(columns, objects, object_count);")
    assert scene.index("renderer_overlay_base_rebuilt();") < scene.index(
        "build_bsp_tilemap(columns, scene_colors, g_view_tiles)")
    assert "void renderer_queue_scene_upload" in scene
    assert "void renderer_upload_scene_step" in scene
    assert "bool renderer_scene_upload_pending" in scene
    assert "u16 budget = VIEW_DMA_TILES_PER_VBLANK" in scene
    assert not re.search(r"(?m)^\s*VDP_waitVSync\s*\(", scene)
    assert scene.index("finish_view_upload();") > scene.index("dbg_wait_dma();")
    assert "RendererPerfSnapshot s_perf" in perf
    assert "void renderer_debug_set_total_vblanks" in perf
    assert "renderer_perf_begin_upload" in scene
    assert "renderer_debug_set_total_vblanks" not in scene
    assert "void renderer_debug_set_total_vblanks" not in renderer

    print("ok    renderer upload policy: full base swap plus active-bank overlays")


if __name__ == "__main__":
    main()
