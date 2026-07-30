#!/usr/bin/env python3
"""Contracts for the single-level textured BSP map and persistent keys."""
import importlib.util
import re
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def load_extractor():
    path = ROOT / "tools/wad-map-extract.py"
    spec = importlib.util.spec_from_file_location("wad_map_extract", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def declaration(source, typename, symbol):
    match = re.search(
        rf"const {typename} {symbol}\[\d+\] = \{{(.*?)\n\}};", source, re.S)
    assert match, symbol
    return match.group(1)


def main():
    generated = (ROOT / "src/generated_e1m1_map.c").read_text()
    assets = (ROOT / "src/generated_assets.h").read_text()
    header = (ROOT / "src/bsp_map.h").read_text()
    runtime = (ROOT / "src/bsp_map.c").read_text()
    main_source = (ROOT / "src/main.c").read_text()
    raycast = (ROOT / "src/raycast.h").read_text()
    extractor = load_extractor()

    expected = {
        "bsp_vertex_count": 467,
        "bsp_seg_count": 386,
        "bsp_subsector_count": 237,
        "bsp_node_count": 236,
        "bsp_door_count": 4,
    }
    for symbol, value in expected.items():
        match = re.search(rf"const u16 {symbol} = (\d+)u;", generated)
        assert match and int(match.group(1)) == value, (symbol, match)

    vertices = [tuple(map(int, values)) for values in re.findall(
        r"\{\s*(-?\d+),\s*(-?\d+)\s*\}",
        declaration(generated, "BspVertex", "bsp_vertices"))]
    expected_bounds = {
        "bsp_map_min_x": min(x for x, _ in vertices),
        "bsp_map_min_y": min(y for _, y in vertices),
        "bsp_map_max_x": max(x for x, _ in vertices),
        "bsp_map_max_y": max(y for _, y in vertices),
    }
    for symbol, value in expected_bounds.items():
        assert re.search(rf"const s16 {symbol} = {value};", generated), symbol
        assert f'lines.append("const s16 {symbol}' in Path(
            extractor.__file__).read_text()

    rows = []
    for row in re.findall(r"\{([^{}]+)\}", declaration(
            generated, "BspSeg", "bsp_segs")):
        values = [int(value) for value in re.findall(r"-?\d+", row)]
        assert len(values) == 11, values
        rows.append(values)
    types = Counter(row[7] for row in rows)
    assert types == {extractor.SEG_WALL: 369,
                     extractor.SEG_DOOR: 16,
                     extractor.SEG_EXIT: 1}, types
    door_rows = [row for row in rows if row[7] == extractor.SEG_DOOR]
    assert Counter(row[8] for row in door_rows) == {0: 4, 1: 4, 2: 4, 3: 4}
    assert all(row[6] != 0 for row in door_rows), "E1M1 door used fallback texture"
    assert all(row[9] == extractor.KEY_NONE for row in door_rows)
    assert all(row[10] & extractor.SEG_FLAG_DIRECT_USE for row in door_rows)

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
    renderer_scene = "\n".join((ROOT / "src" / n).read_text() for n in (
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
    extractor_source = Path(extractor.__file__).read_text()
    assert "for door_style in range(2)" in extractor_source
    assert "level[texel] * 0x11" in extractor_source
    assert extractor.texture_u_scale_q12(24) > 0
    assert "FREEDOOM_WALL_TEXTURE_USCALE_Q12[tid]" in (
        ROOT / "src/bsp_render.c").read_text()

    # Runtime and proof use the same canonical Doom collision radius.
    radius = int(re.search(
        r"#define PLAYER_COLLISION_RADIUS (\d+)", raycast).group(1))
    assert radius == extractor.PLAYER_RADIUS == 16

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

    print("ok    flat E1M1: 386 textured segs, 4 grouped doors, persistent RGB keys")


if __name__ == "__main__":
    main()
