#!/usr/bin/env python3
"""Contracts for the single-level textured BSP map and persistent keys."""
import importlib.util
import re
from collections import Counter
from pathlib import Path

from e1m1_expected import (E1M1_HEADER_ROW, E1M1_SEG_COUNT,
                           E1M1_WALL_SEG_COUNT, E1M1_DOOR_SEG_COUNT,
                           E1M1_EXIT_SEG_COUNT, E1M1_DOOR_GROUP_COUNT,
                           E1M1_PLAIN_DOOR_SEG_COUNT,
                           E1M1_WINDOW_SEG_COUNT, E1M1_WINDOW_LINEDEF_COUNT,
                           E1M1_SKY_WALL_SEG_COUNT, E1M1_SKY_WALL_LINEDEF_COUNT)

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
    wad_reader = load_module("wad_reader_sector", "tools/wad_reader.py")
    campaign_wad = str(ROOT / "DOOM1.WAD")
    world_assets = load_module("world_assets_sector", "tools/world_assets.py")
    bsp_emit_source = (ROOT / "tools/bsp_emit.py").read_text()
    world_assets_source = (ROOT / "tools/world_assets.py").read_text()

    assert re.search(
        r"const BspMapData g_e1m1_map = \{.*?" + re.escape(E1M1_HEADER_ROW),
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
    assert "_Static_assert(sizeof(BspSeg) == 16" in header

    rows = []
    for row in re.findall(r"\{([^{}]+)\}", declaration(
            generated, "BspSeg", "e1m1_bsp_segs")):
        values = [int(value) for value in re.findall(r"-?\d+", row)]
        assert len(values) == 11, values
        rows.append(values)
    types = Counter(row[7] for row in rows)
    # This WAD's E1M1 has no remote-trigger door lines (no SEG_TRIGGER segs):
    # every door group's own linedef carries its direct-use special.
    assert types == {doom_map.SEG_WALL: E1M1_WALL_SEG_COUNT,
                     doom_map.SEG_DOOR: E1M1_DOOR_SEG_COUNT,
                     doom_map.SEG_EXIT: E1M1_EXIT_SEG_COUNT,
                     doom_map.SEG_WINDOW: E1M1_WINDOW_SEG_COUNT,
                     doom_map.SEG_SKY_WALL: E1M1_SKY_WALL_SEG_COUNT}, types

    # Windows: reclassified from SEG_WALL, never newly emitted. Re-running the
    # converter with the window rule disabled has to produce the SAME segs in
    # the SAME order with the same geometry -- that is what lets a window skip
    # bsp_mark_sample_solid without touching collision, LOS, the blockmap or
    # the navigation certificate. Proved here rather than asserted in a comment.
    window_rows = [row for row in rows if row[7] == doom_map.SEG_WINDOW]
    assert len(window_rows) == E1M1_WINDOW_SEG_COUNT
    plain = doom_map.load_map(wad_reader.WadFile(campaign_wad), "E1M1",
                              apply_windows=False)
    assert len(plain.out_segs) == len(rows), (len(plain.out_segs), len(rows))
    for index, (row, seg) in enumerate(zip(rows, plain.out_segs)):
        geometry = (seg["v1"], seg["v2"], seg["nx"], seg["ny"],
                    seg["tex_u_offset"])
        assert tuple(row[0:5]) == geometry, (index, row[0:5], geometry)
        if row[7] == doom_map.SEG_WINDOW:
            assert seg["type"] == doom_map.SEG_WALL, index
        else:
            assert seg["type"] == row[7], (index, seg["type"], row[7])
    assert len({seg["source_linedef"] for seg in
                (s for s in doom_map.load_map(
                    wad_reader.WadFile(campaign_wad), "E1M1").out_segs
                 if s["type"] == doom_map.SEG_WINDOW)}) == E1M1_WINDOW_LINEDEF_COUNT

    # Sky walls: same proof, same reason -- a one-sided line is already forced
    # solid unconditionally (line_solid_without_recipe), so this reclassification
    # can only ever change the type byte, never geometry, collision or the
    # blockmap.
    sky_wall_rows = [row for row in rows if row[7] == doom_map.SEG_SKY_WALL]
    assert len(sky_wall_rows) == E1M1_SKY_WALL_SEG_COUNT
    plain_sky = doom_map.load_map(wad_reader.WadFile(campaign_wad), "E1M1",
                                  apply_sky_walls=False)
    assert len(plain_sky.out_segs) == len(rows), (len(plain_sky.out_segs), len(rows))
    for index, (row, seg) in enumerate(zip(rows, plain_sky.out_segs)):
        geometry = (seg["v1"], seg["v2"], seg["nx"], seg["ny"],
                    seg["tex_u_offset"])
        assert tuple(row[0:5]) == geometry, (index, row[0:5], geometry)
        if row[7] == doom_map.SEG_SKY_WALL:
            assert seg["type"] == doom_map.SEG_WALL, index
        else:
            assert seg["type"] == row[7], (index, seg["type"], row[7])
    assert len({seg["source_linedef"] for seg in
                (s for s in doom_map.load_map(
                    wad_reader.WadFile(campaign_wad), "E1M1").out_segs
                 if s["type"] == doom_map.SEG_SKY_WALL)}) == E1M1_SKY_WALL_LINEDEF_COUNT
    assert all(row[8] == doom_map.DOOR_GROUP_NONE for row in sky_wall_rows)
    assert all(row[9] == doom_map.KEY_NONE for row in sky_wall_rows)
    assert all(row[10] == 0 for row in sky_wall_rows), (
        "a sky wall carries no door_group/required_key/interaction flags")

    # The band rides in door_group/required_key (see BspSeg in bsp_map.h) and
    # must describe a real opening strictly inside the drawn slab.
    for row in window_rows:
        band_top, band_bottom = row[8], row[9]
        assert 0 <= band_top < band_bottom <= 255, (band_top, band_bottom)
        assert row[10] == 0, "a window carries no interaction flags"
    door_rows = [row for row in rows if row[7] == doom_map.SEG_DOOR]
    assert Counter(row[8] for row in door_rows) == {
        group: 4 for group in range(E1M1_DOOR_GROUP_COUNT)}
    assert all(row[6] != 0 for row in door_rows), "E1M1 door used fallback texture"
    assert all(row[9] == doom_map.KEY_NONE for row in door_rows)
    assert all(row[10] & doom_map.SEG_FLAG_DIRECT_USE for row in door_rows), (
        "every E1M1 door group's own linedef is directly usable")

    # A SECRET flag on either physical linedef makes the whole grouped door a
    # camouflaged wall. In E1M1 that is exactly group 1 (BROWN96): both source
    # lines and all four BSP faces must agree even though only linedef 247 owns
    # Doom's flag. Ordinary BIGDOOR/EXITDOOR groups retain the framed style.
    plain_door_rows = [row for row in door_rows
                       if row[10] & doom_map.SEG_FLAG_PLAIN_DOOR]
    assert len(plain_door_rows) == E1M1_PLAIN_DOOR_SEG_COUNT
    assert {row[8] for row in plain_door_rows} == {1}
    assert all((row[10] & doom_map.SEG_FLAG_PLAIN_DOOR) == 0
               for row in door_rows if row[8] != 1)

    # Negative control: classification changes only the new flag bit. It must
    # not perturb ordering, geometry, material, grouping, interaction or the
    # navigation certificate. E1M2 is checked too so this cannot regress into
    # an E1M1-specific exception.
    for map_name, expected_groups, expected_faces in (
            ("E1M1", {1}, E1M1_PLAIN_DOOR_SEG_COUNT),
            ("E1M2", {0, 1, 2, 6, 8, 10, 11}, 26)):
        classified = doom_map.load_map(
            wad_reader.WadFile(campaign_wad), map_name)
        control = doom_map.load_map(
            wad_reader.WadFile(campaign_wad), map_name,
            apply_plain_doors=False)
        assert classified.certificate == control.certificate
        assert len(classified.out_segs) == len(control.out_segs)
        changed = 0
        changed_groups = set()
        for candidate, baseline in zip(classified.out_segs, control.out_segs):
            candidate_without_style = dict(candidate)
            candidate_without_style["flags"] &= ~doom_map.SEG_FLAG_PLAIN_DOOR
            assert candidate_without_style == baseline
            if candidate["flags"] != baseline["flags"]:
                assert candidate["type"] == doom_map.SEG_DOOR
                assert candidate["flags"] == (
                    baseline["flags"] | doom_map.SEG_FLAG_PLAIN_DOOR)
                changed += 1
                changed_groups.add(candidate["door_group"])
        assert changed == expected_faces, (map_name, changed)
        assert changed_groups == expected_groups, (map_name, changed_groups)

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
    assert "[FREEDOOM_WORLD_SHADE_LEVELS][FREEDOOM_WALL_TEXTURE_COUNT]" in assets
    assert "FREEDOOM_WALL_DOOR_PACKED_PAIRS" in assets
    assert "FREEDOOM_WALL_PACKED_PAIRS[" in renderer_scene
    assert "packed_wall_column" in renderer_scene
    assert "FREEDOOM_WALL_DOOR_TEXTURE_INDEX" in renderer_scene
    assert "FREEDOOM_WALL_DOOR_TEXTURE_COUNT" in world_assets_source
    # Both packed tables go through packed_pair_byte, and a byte carries the
    # two pixels of one stride-2 sample as independent nibbles (high = even
    # pixel). Checked functionally rather than by matching source text.
    assert "packed_pair_byte" in world_assets_source
    assert "* 0x11" not in world_assets_source, (
        "packed bytes must carry two texels, not one index twice")
    identity = list(range(16))
    assert world_assets.packed_pair_byte(identity, 0xA, 0x3) == 0xA3
    assert world_assets.packed_pair_byte(identity, 0x7, 0x7) == 0x77
    darker = [0] * 16
    assert world_assets.packed_pair_byte(darker, 0xA, 0x3) == 0x00
    assert world_assets.texture_u_scale_q12(24) > 0
    assert "FREEDOOM_WALL_TEXTURE_USCALE_Q12[tid]" in (
        ROOT / "src/bsp/bsp_render_columns.c").read_text()

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

    print("ok    flat E1M1: %d textured segs, %d grouped doors, persistent RGB keys" %
          (E1M1_SEG_COUNT, E1M1_DOOR_GROUP_COUNT))


if __name__ == "__main__":
    main()
