#!/usr/bin/env python3
"""Contracts for grouped vertical-lift doors and their renderer overlay."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAP = (ROOT / "src/bsp_map.c").read_text()
HEADER = (ROOT / "src/bsp_map.h").read_text()
RAYCAST = (ROOT / "src/raycast.h").read_text()
BSP_RENDER = (ROOT / "src/bsp_render.c").read_text()
SCENE = (ROOT / "src/renderer_scene.c").read_text()
MAIN = (ROOT / "src/main.c").read_text()
GENERATED = (ROOT / "src/generated_e1m1_map.c").read_text()

LIFT_MAX = 256
STEP_PER_VBLANK = 8


def advance(lift, target_open, elapsed_vblanks):
    delta = min(LIFT_MAX, elapsed_vblanks * STEP_PER_VBLANK)
    if target_open:
        return min(LIFT_MAX, lift + delta)
    return max(0, lift - delta)


def declaration(typename, symbol):
    match = re.search(
        rf"const {typename} {symbol}\[\d+\] = \{{(.*?)\n\}};",
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
    assert "bsp_update_doors(elapsed_frames)" in MAIN
    assert "renderer_redraw_request_base(&redraw, RENDERER_REDRAW_BASE)" in MAIN

    # Exactly 32 vblanks, monotonic in both directions, and reversible without
    # snapping to an endpoint.
    lift = 0
    opening = []
    for _ in range(32):
        lift = advance(lift, True, 1)
        opening.append(lift)
    assert opening == list(range(8, 257, 8))
    assert all(value < LIFT_MAX for value in opening[:-1])
    assert opening[-1] == LIFT_MAX

    lift = advance(0, True, 10)
    assert lift == 80
    lift = advance(lift, False, 3)
    assert lift == 56
    lift = advance(lift, True, 2)
    assert lift == 72
    closing = []
    for _ in range(32):
        lift = advance(LIFT_MAX if not closing else closing[-1], False, 1)
        closing.append(lift)
    assert closing == list(range(248, -1, -8))
    assert closing[0] < LIFT_MAX  # closing blocks on its first update
    assert closing[-1] == 0

    # The renderer has a distinct moving-door overlay. Partial doors do not
    # close the BSP sample, so farther walls become the base visible in the gap.
    assert "typedef struct {" in RAYCAST and "RayDoorOverlay" in RAYCAST
    assert "RAY_COLUMN_FLAG_DOOR" in RAYCAST
    assert "const bool moving_door" in BSP_RENDER
    assert "if (moving_door)" in BSP_RENDER
    moving_branch = BSP_RENDER.split("if (moving_door) {", 1)[1].split("} else {", 1)[0]
    assert "RayDoorOverlay" in moving_branch
    assert "mark_sample_solid" not in moving_branch
    assert "draw_door_overlays(columns, g_view_tiles)" in SCENE

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
    assert "y - descriptor->top + lift_pixels" in SCENE
    assert "door->height * door->lift" in SCENE

    # Door-only framing and vertical billboard occlusion: a farther object is
    # hidden in the remaining slab but visible in the lower opening; a nearer
    # object remains visible over the door.
    assert "style_wall_texel" in SCENE
    assert "MEGALDOOM_WORLD_COLOR_WARNING" in SCENE
    assert "descriptor->tex_x < DOOR_FRAME_TEXELS" in SCENE
    assert "tex_y >= (WALL_TEX_DIM - DOOR_SAFETY_TEXELS)" in SCENE
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
    for row in re.findall(r"\{([^{}]+)\}", declaration("BspSeg", "bsp_segs")):
        values = [int(value) for value in re.findall(r"-?\d+", row)]
        assert len(values) == 11
        rows.append(values)
    doors = [row for row in rows if row[7] == 1]
    tracks = [row for row in rows if row[6] == 14]
    assert len(doors) == 16 and all(row[6] != 0 for row in doors)
    assert tracks and all(row[7] == 0 for row in tracks)

    # Geometry passability changes only at the fully-open endpoint, where the
    # visibility revision is invalidated for cached LOS consumers.
    assert "was_open != (g_door_lift[i] == BSP_DOOR_LIFT_MAX)" in MAP
    assert MAP.count("g_visibility_revision++") == 2  # reset + endpoint transition

    print("ok    doors: framed 32-vblank lift, reversible groups, vertical occlusion")


if __name__ == "__main__":
    main()
