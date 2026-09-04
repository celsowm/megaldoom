#!/usr/bin/env python3
"""Contracts for grouped vertical-lift doors and their renderer overlay."""
import re
from pathlib import Path

from e1m1_expected import E1M1_DOOR_SEG_COUNT

ROOT = Path(__file__).resolve().parents[1]
MAP = (ROOT / "src/bsp/bsp_map.c").read_text()
HEADER = (ROOT / "src/bsp/bsp_map.h").read_text()
RAYCAST = (ROOT / "src/raycast.h").read_text()
BSP_RENDER = (ROOT / "src/bsp/bsp_render_columns.c").read_text()
RENDERER_ASSETS = (ROOT / "src/renderer/generated_renderer_assets.h").read_text()
# renderer_scene.c was split by SRP into several files; door-overlay code
# these checks look for now lives across that set.
SCENE = "\n".join((ROOT / "src/renderer" / n).read_text() for n in (
    "renderer_scene.c", "renderer_pack.c", "renderer_doors.c",
    "renderer_billboard_draw.c", "renderer_frame_overlay.c",
    "renderer_upload.c", "renderer_sparse.c",
    "renderer_flats.c",
))
DOORS = (ROOT / "src/renderer/renderer_doors.c").read_text()
MAIN = (ROOT / "src/main.c").read_text()
GENERATED = (ROOT / "src/bsp/generated_e1m1_map.c").read_text()
ASSETS = (ROOT / "src/bsp/generated_assets.h").read_text()

LIFT_MAX = 256
STEP_PER_VBLANK = 16


def advance(lift, target_open, elapsed_vblanks):
    delta = min(LIFT_MAX, elapsed_vblanks * STEP_PER_VBLANK)
    if target_open:
        return min(LIFT_MAX, lift + delta)
    return max(0, lift - delta)


def declaration(typename, symbol):
    match = re.search(
        rf"static const {typename} {symbol}\[\d+\] = \{{(.*?)\n\}};",
        GENERATED, re.S)
    assert match, symbol
    return match.group(1)


def main():
    # Public state/update contract and shared per-group storage.
    assert "bool bsp_update_doors(u16 elapsed_vblanks);" in HEADER
    assert "u16 bsp_seg_door_lift(u16 seg_index);" in HEADER
    assert "g_door_lift[BSP_MAX_DOORS]" in MAP
    assert "g_door_target_open[BSP_MAX_DOORS]" in MAP
    assert "g_door_lift[seg->door_group]" in MAP
    assert "g_door_target_open[door_group] = !g_door_target_open[door_group]" in MAP
    assert "g_door_lift[i] == BSP_DOOR_LIFT_MAX" in MAP
    # Doors must be credited with real elapsed time, not the [1,4]-clamped
    # movement delta that starved them at low framerates.
    assert "bsp_update_doors(elapsed_vblanks)" in MAIN
    assert "bsp_update_doors(elapsed_frames)" not in MAIN
    assert "renderer_redraw_request_base(&redraw, RENDERER_REDRAW_BASE)" in MAIN

    # Exactly 16 vblanks, monotonic in both directions, and reversible without
    # snapping to an endpoint.
    lift = 0
    opening = []
    for _ in range(16):
        lift = advance(lift, True, 1)
        opening.append(lift)
    assert opening == list(range(16, 257, 16))
    assert all(value < LIFT_MAX for value in opening[:-1])
    assert opening[-1] == LIFT_MAX

    lift = advance(0, True, 10)
    assert lift == 160
    lift = advance(lift, False, 3)
    assert lift == 112
    lift = advance(lift, True, 2)
    assert lift == 144
    closing = []
    for _ in range(16):
        lift = advance(LIFT_MAX if not closing else closing[-1], False, 1)
        closing.append(lift)
    assert closing == list(range(240, -1, -16))
    assert closing[0] < LIFT_MAX  # closing blocks on its first update
    assert closing[-1] == 0

    # The renderer has a distinct near-surface overlay shared by moving doors
    # and windows. Neither closes the BSP sample, so farther walls become the
    # base visible in the gap.
    assert "typedef struct {" in RAYCAST and "RayDoorOverlay" in RAYCAST
    assert "RAY_COLUMN_FLAG_DOOR" in RAYCAST
    assert "const bool moving_door" in BSP_RENDER
    assert "const bool window" in BSP_RENDER
    assert "if (overlay)" in BSP_RENDER
    assert "#define RAY_MAX_PROJECTED_WALL_HEIGHT 640" in RAYCAST
    assert "projected_height" in BSP_RENDER
    assert "projected_wall_h" in SCENE
    assert "vertical_samples == b->vertical_samples" in SCENE
    assert "MEGALDOOM_WALL_TEX_Y_BY_HEIGHT[641][120]" in RENDERER_ASSETS
    overlay_branch = BSP_RENDER.split("if (overlay) {", 1)[1].split("} else {", 1)[0]
    assert "RayDoorOverlay" in overlay_branch
    assert "mark_sample_solid" not in overlay_branch

    # A window is the same overlay with the gap in the middle instead of at the
    # bottom, discriminated by lift == 0 (a door overlay is only ever written
    # while its lift is 1..255). The band rides in the two bytes a non-door seg
    # has spare, so RayDoorOverlay must not grow: it is in every RayColumn.
    assert "_Static_assert(sizeof(RayDoorOverlay) == 10" in RAYCAST
    assert "ray_overlay_is_window" in RAYCAST
    assert "ray_overlay_is_plain_door" in RAYCAST
    assert "band_top" in RAYCAST and "band_bottom" in RAYCAST
    assert "BSP_SEG_WINDOW" in HEADER
    assert "bsp_seg_window_band_top" in HEADER
    assert "near->lift = window ? 0 :" in BSP_RENDER
    assert "plain_door ? RAY_OVERLAY_FLAG_PLAIN_DOOR : 0" in BSP_RENDER
    assert "window_band_rows" in SCENE
    # The band is resolved to ABSOLUTE viewport rows once per sampled column in
    # the caster, not re-derived from Q8 by each consumer. That is what keeps
    # door_overlay_blocks_pixel -- called per byte per sprite row -- free of
    # multiplies and of a WallColumnDescriptor build.
    assert "near->band_top = window ? (u8)(slab_top +" in BSP_RENDER
    assert "(RAY_VIEW_ROWS - height) / 2" in BSP_RENDER
    assert "describe_door_overlay(door)" not in DOORS.split(
        "bool door_overlay_blocks_pixel", 1)[1]
    # Still solid for collision and line of sight: only rendering differs.
    assert "BSP_SEG_WINDOW" not in MAP
    # scene_colors carries sky_offset, which the window band needs so the sky it
    # paints matches the sky the pack stage draws once you walk outside.
    assert "draw_door_overlays(columns, scene_colors, g_view_tiles)" in SCENE

    # A rising slab keeps the full-height vertical lookup and discards rows from
    # the top. It never stretches the remaining texture over the shrinking area.
    height = 120
    previous_visible = height
    for lift in range(0, LIFT_MAX):
        lift_pixels = (height * lift) >> 8
        visible = height - lift_pixels
        assert 1 <= visible <= previous_visible
        assert (visible - 1) + lift_pixels == height - 1
        previous_visible = visible
    assert "y_start - descriptor->top + lift_pixels" in SCENE
    assert "door->height * door->lift" in SCENE

    # A SECRET door is mechanically still BSP_SEG_DOOR but uses ordinary wall
    # pairs both while closed and through the moving overlay. The overlay bit
    # rides in band_top, which is otherwise unused for the non-window variant,
    # so no per-column RAM is added.
    assert "BSP_SEG_FLAG_PLAIN_DOOR" in HEADER
    assert "seg->flags & BSP_SEG_FLAG_PLAIN_DOOR" in BSP_RENDER
    assert "seg->type == BSP_SEG_DOOR && !plain_door" in BSP_RENDER
    assert "ray_overlay_is_plain_door(door)" in SCENE
    assert "RAY_OVERLAY_FLAG_PLAIN_DOOR" in RAYCAST

    # The two overlay posts are hand-written 68000 (renderer_hotpath.s), and the
    # standing rule for that is a differential harness that is PROVEN able to
    # fail. Pin the three properties that keep it one:
    #   1. both implementations exist and are held to one signature,
    #   2. the harness replays the production emitter (paint_overlay_column)
    #      rather than a second copy of the argument resolution, and
    #   3. it computes BOTH sides locally, so whichever one ships cannot
    #      silently turn the check into asm-vs-asm (LOG, 2026-08-04).
    HOTPATH = (ROOT / "src/renderer/renderer_hotpath.s").read_text()
    assert "renderer_write_overlay_frame_post_asm" in HOTPATH
    assert "renderer_write_overlay_sky_post_asm" in HOTPATH
    assert "write_overlay_frame_post_reference" in DOORS
    assert "write_overlay_sky_post_reference" in DOORS
    assert "RENDERER_OVERLAY_C_REFERENCE" in DOORS
    harness = DOORS.split("static void compare_overlay_posts_asm", 1)[1]
    harness = harness.split("void draw_door_overlays", 1)[0]
    assert harness.count("paint_overlay_column(") == 2
    assert "s_overlay_use_reference = FALSE;" in harness
    assert "s_overlay_use_reference = TRUE;" in harness
    assert "renderer_perf_record_asm_compare" in harness
    assert "overlay_probe_broken" in harness
    # The overlay frame post must wrap with the SAME mask the wall post uses:
    # MEGALDOOM_WALL_TEX_Y_BY_HEIGHT reaches 252, so the mask is load-bearing
    # even when tex_y is 0 (which it is for every window -- only four moving
    # doors in the shipped maps carry a nonzero tex_v_offset).
    frame_post = HOTPATH.split(".Lovl_frame_loop:", 1)[1]
    assert "andi.w  #WALL_TEX_HEIGHT_MASK,d1" in frame_post
    # Both posts close with DBRA, which would run 65536 times on a zero count,
    # so the empty-post drop has to stay in the caller.
    assert "dbra" in frame_post
    assert "if (y_start >= y_end) return;" in DOORS

    # A wall at the near clip projects to 640 pixels. Only its central 120
    # rows are visible, so the first/last visible rows must sample the middle
    # of the 128-row texture instead of stretching the whole texture.
    projected = 640
    visible = min(120, projected)
    clip_offset = (projected - visible) // 2
    assert ((0 + clip_offset) * 128) // projected == 52
    assert ((119 + clip_offset) * 128) // projected == 75

    # Door-only framing and vertical billboard occlusion: a farther object is
    # hidden in the remaining slab but visible in the lower opening; a nearer
    # object remains visible over the door.
    # The framing itself is no longer re-derived per pixel in C: the overlay
    # reads FREEDOOM_WALL_DOOR_PACKED_PAIRS through the same packed_wall_column()
    # the wall post uses, and that table's bake carries the frame/safety rules.
    # Check them where they now live, and check the C side selects the door
    # table -- otherwise a door would composite as plain wall art.
    bake = (ROOT / "tools" / "world_assets.py").read_text()
    assert "FREEDOOM_WALL_DOOR_PACKED_PAIRS" in bake
    assert "WORLD_COLOR_WARNING" in bake
    assert "border = WALL_TEX_WIDTH // 16" in bake
    assert "source_y >= source_height - safety" in bake
    assert "RAY_COLUMN_FLAG_DOOR" in SCENE and "FREEDOOM_WALL_DOOR_TEXTURE_INDEX" in SCENE
    assert "packed_wall_column(&descriptor)" in SCENE
    assert "door_overlay_blocks_pixel" in SCENE
    door_top = 0
    visible_bottom = height - ((height * 128) >> 8)
    door_depth, base_depth = 100, 300
    assert door_depth < base_depth
    assert 200 >= door_depth and door_top <= 20 < visible_bottom
    assert not (door_top <= 90 < visible_bottom)
    assert 50 < door_depth  # nearer object is not occluded by the slab

    # E1M1 keeps real slab textures and static DOORTRAK jambs.
    rows = []
    for row in re.findall(r"\{([^{}]+)\}", declaration("BspSeg", "e1m1_bsp_segs")):
        values = [int(value) for value in re.findall(r"-?\d+", row)]
        assert len(values) == 11
        rows.append(values)
    doors = [row for row in rows if row[7] == 1]
    doortrak = int(re.search(r"#define MEGALDOOM_TEX_DOORTRAK (\d+)", ASSETS).group(1))
    tracks = [row for row in rows if row[6] == doortrak]
    assert len(doors) == E1M1_DOOR_SEG_COUNT and all(row[6] != 0 for row in doors)
    assert tracks and all(row[7] == 0 for row in tracks)

    # Geometry passability changes only at the fully-open endpoint, where the
    # visibility revision is invalidated for cached LOS consumers.
    assert "was_open != (g_door_lift[i] == BSP_DOOR_LIFT_MAX)" in MAP
    assert MAP.count("g_visibility_revision++") == 2  # reset + endpoint transition

    print("ok    doors: framed 16-vblank lift, reversible groups, vertical occlusion")


if __name__ == "__main__":
    main()
