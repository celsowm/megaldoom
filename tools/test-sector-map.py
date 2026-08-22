#!/usr/bin/env python3
"""Contracts for the single-level textured BSP map and persistent keys."""
import importlib.util
import re
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def load_module(name, relative_path):
    path = ROOT / relative_path
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def declaration(source, typename, symbol):
    match = re.search(
        rf"(?:static )?const {typename} {symbol}\[\d+\] = \{{(.*?)\n\}};", source, re.S)
    assert match, symbol
    return match.group(1)


def main():
    generated = (ROOT / "src/bsp/generated_e1m1_map.c").read_text()
    assets = (ROOT / "src/bsp/generated_assets.h").read_text()
    header = (ROOT / "src/bsp/bsp_map.h").read_text()
    runtime = (ROOT / "src/bsp/bsp_map.c").read_text()
    main_source = (ROOT / "src/main.c").read_text()
    raycast = (ROOT / "src/raycast.h").read_text()
    doom_map = load_module("doom_map_sector", "tools/doom_map.py")
    world_assets = load_module("world_assets_sector", "tools/world_assets.py")
    bsp_emit_source = (ROOT / "tools/bsp_emit.py").read_text()
    world_assets_source = (ROOT / "tools/world_assets.py").read_text()

    assert re.search(
        r"const BspMapData g_e1m1_map = \{.*?"
        r"237u, 394u, 470u, 239u, 238u, 5u, 143u, 88u, 3u,",
        generated, re.S)

    vertices = [tuple(map(int, values)) for values in re.findall(
        r"\{\s*(-?\d+),\s*(-?\d+)\s*\}",
        declaration(generated, "BspVertex", "e1m1_bsp_vertices"))]
    expected_bounds = {
        "bsp_map_min_x": min(x for x, _ in vertices),
        "bsp_map_min_y": min(y for _, y in vertices),
        "bsp_map_max_x": max(x for x, _ in vertices),
        "bsp_map_max_y": max(y for _, y in vertices),
    }
    bounds = tuple(expected_bounds[name] for name in (
        "bsp_map_min_x", "bsp_map_min_y", "bsp_map_max_x", "bsp_map_max_y"))
    assert re.search(r"\s%d, %d, %d, %d," % bounds, generated)
    assert "const BspMapData g_%s_map" in bsp_emit_source

    rows = []
    for row in re.findall(r"\{([^{}]+)\}", declaration(
            generated, "BspSeg", "e1m1_bsp_segs")):
        values = [int(value) for value in re.findall(r"-?\d+", row)]
        assert len(values) == 11, values
        rows.append(values)
    types = Counter(row[7] for row in rows)
    assert types == {doom_map.SEG_WALL: 371,
                     doom_map.SEG_DOOR: 20,
                     doom_map.SEG_EXIT: 1,
                     doom_map.SEG_TRIGGER: 2}, types
    door_rows = [row for row in rows if row[7] == doom_map.SEG_DOOR]
    assert Counter(row[8] for row in door_rows) == {
        0: 4, 1: 4, 2: 4, 3: 4, 4: 4}
    assert all(row[6] != 0 for row in door_rows), "E1M1 door used fallback texture"
    assert all(row[9] == doom_map.KEY_NONE for row in door_rows)
    assert all(not (row[10] & doom_map.SEG_FLAG_DIRECT_USE)
               for row in door_rows if row[8] == 0)
    assert all(row[10] & doom_map.SEG_FLAG_DIRECT_USE
               for row in door_rows if row[8] != 0)

    forbidden = ("BspLine", "BspRenderSeg", "BspSector", "portal",
                 "floor_height", "ceiling_height", "BSP_SECTOR_RENDERER")
    assert all(token not in generated for token in forbidden), forbidden
    assert not (ROOT / "src/sector_render.c").exists()
    assert "sector_render" not in (ROOT / "tools/build-windows.ps1").read_text()

    # There is one production renderer: the direct BSP-to-tile packer.  Do not
    # retain a compiled-out reference/strip implementation that can become an
    # accidental fallback in a performance build.
    # renderer_scene.c was split by SRP into several files; the pack-stage
    # code these checks look for now lives across that set.
    renderer_scene = "\n".join((ROOT / "src/renderer" / n).read_text() for n in (
        "renderer_scene.c", "renderer_pack.c", "renderer_doors.c",
        "renderer_billboard_draw.c", "renderer_frame_overlay.c",
        "renderer_upload.c", "renderer_sparse.c",
        "renderer_flats.c",
    ))
    forbidden_renderer = ("RENDERER_REFERENCE_PACKER", "build_column_strip_reference",
                          "build_raycast_tilemap_reference", "g_reference_view_tiles",
                          "build_raycast_tilemap")
    assert all(token not in renderer_scene for token in forbidden_renderer), forbidden_renderer
    assert "build_bsp_tilemap" in renderer_scene

    # Textured rendering remains the only path, including arbitrary Doom widths.
    assert "FREEDOOM_WALL_TEXTURES" in assets
    assert "FREEDOOM_WALL_TEXTURE_USCALE_Q12" in assets
    assert "FREEDOOM_WALL_PACKED_PAIRS" in assets
    assert "[2][FREEDOOM_WORLD_SHADE_LEVELS][FREEDOOM_WALL_TEXTURE_COUNT]" in assets
    assert "FREEDOOM_WALL_PACKED_PAIRS[" in renderer_scene
    assert "descriptors[0].flags & RAY_COLUMN_FLAG_DOOR" in renderer_scene
    assert "for door_style in range(2)" in world_assets_source
    assert "level[texel] * 0x11" in world_assets_source
    assert world_assets.texture_u_scale_q12(24) > 0
    assert "FREEDOOM_WALL_TEXTURE_USCALE_Q12[tid]" in (
        ROOT / "src/bsp/bsp_render.c").read_text()

    # Runtime and proof use the same canonical Doom collision radius.
    radius = int(re.search(
        r"#define PLAYER_COLLISION_RADIUS (\d+)", raycast).group(1))
    assert radius == doom_map.PLAYER_RADIUS == 16

    # Colored keys are persistent bits and every door face shares group state.
    for name, value in (("BSP_KEY_BLUE", 1), ("BSP_KEY_YELLOW", 2),
                        ("BSP_KEY_RED", 4)):
        assert re.search(rf"#define {name} 0x0{value}u", header)
    assert "g_door_lift[BSP_MAX_DOORS]" in runtime
    assert "g_door_lift[seg->door_group]" in runtime
    assert "owned_keys & s->required_key" in runtime
    assert all("billboard_consume_key" not in path.read_text()
               for path in (ROOT / "src").glob("*.[ch]"))
    assert "player_keys | pickup.key_mask" in main_source
    assert "target_count > 0" not in main_source
    assert "billboard_consume_key" not in main_source

    print("ok    flat E1M1: 394 textured segs, 5 grouped doors, persistent RGB keys")


if __name__ == "__main__":
    main()
