#!/usr/bin/env python3
"""Deterministic source/generated contract checks for the E1M1 sector MVP."""
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def wad_lumps(path):
    data = path.read_bytes()
    _, count, offset = struct.unpack_from("<4sii", data, 0)
    entries = []
    for i in range(count):
        pos, size, raw = struct.unpack_from("<ii8s", data, offset + i * 16)
        entries.append((raw.rstrip(b"\0").decode("ascii"), data[pos:pos + size]))
    return entries


def main():
    lumps = wad_lumps(ROOT / "DOOM1.WAD")
    marker = next(i for i, (name, _) in enumerate(lumps) if name == "E1M1")
    section = dict(lumps[marker + 1:marker + 12])
    expected = {
        "bsp_sector_count": len(section["SECTORS"]) // 26,
        "bsp_line_count": len(section["LINEDEFS"]) // 14,
        "bsp_render_seg_count": len(section["SEGS"]) // 12,
        "bsp_subsector_count": len(section["SSECTORS"]) // 4,
    }
    assert expected == {
        "bsp_sector_count": 85,
        "bsp_line_count": 475,
        "bsp_render_seg_count": 732,
        "bsp_subsector_count": 237,
    }, expected

    sectors = [struct.unpack_from("<hh8s8shhh", section["SECTORS"], i)
               for i in range(0, len(section["SECTORS"]), 26)]
    sides = [struct.unpack_from("<hh8s8s8sH", section["SIDEDEFS"], i)
             for i in range(0, len(section["SIDEDEFS"]), 30)]
    lines = [struct.unpack_from("<HHHHHHH", section["LINEDEFS"], i)
             for i in range(0, len(section["LINEDEFS"]), 14)]
    initial_step_heights = set()
    for line_id in (39, 40, 41, 50, 51, 52, 53, 54):
        _, _, _, _, _, right, left = lines[line_id]
        for side_id in (right, left):
            if side_id == 0xFFFF:
                continue
            _, _, _, _, _, sector_id = sides[side_id]
            initial_step_heights.add(sectors[sector_id][0])
    assert {-16, -8, 0}.issubset(initial_step_heights), initial_step_heights

    generated = (ROOT / "src/generated_e1m1_map.c").read_text()
    for symbol, value in expected.items():
        match = re.search(rf"const u16 {symbol} = (\d+)u;", generated)
        assert match and int(match.group(1)) == value, (symbol, match)
    assert generated.count("const BspLine bsp_lines[") == 1
    assert generated.count("const BspRenderSeg bsp_render_segs[") == 1
    assert generated.count("const BspRenderSubsector bsp_render_subsectors[") == 1
    assert generated.count("const BspSector bsp_sectors[") == 1
    render_subsectors_match = re.search(
        r"const BspRenderSubsector bsp_render_subsectors\[\d+\] = \{(.*?)\};",
        generated, re.S)
    assert render_subsectors_match
    render_subsectors = [tuple(map(int, values)) for values in re.findall(
        r"\{(\d+), (\d+), (\d+)\}", render_subsectors_match.group(1))]
    assert len(render_subsectors) == expected["bsp_subsector_count"]
    cursor = 0
    for first, count, sector_id in render_subsectors:
        assert first == cursor, (first, cursor)
        assert 0 <= sector_id < expected["bsp_sector_count"], sector_id
        cursor += count
    assert cursor == expected["bsp_render_seg_count"], cursor

    raycast_header = (ROOT / "src/raycast.h").read_text()
    extractor = (ROOT / "tools/wad-map-extract.py").read_text()
    player_height = int(re.search(r"#define PLAYER_HEIGHT (\d+)", raycast_header).group(1))
    player_max_step = int(re.search(r"#define PLAYER_MAX_STEP (\d+)", raycast_header).group(1))
    assert int(re.search(r"^PLAYER_HEIGHT = (\d+)$", extractor, re.M).group(1)) == player_height
    assert int(re.search(r"^PLAYER_MAX_STEP = (\d+)$", extractor, re.M).group(1)) == player_max_step

    render_segs_match = re.search(
        r"const BspRenderSeg bsp_render_segs\[\d+\] = \{(.*?)\};",
        generated, re.S)
    assert render_segs_match
    render_rows = []
    for row in re.findall(r"\{([^{}]+)\}", render_segs_match.group(1)):
        values = [int(value) for value in re.findall(r"-?\d+", row)]
        assert len(values) == 13, values
        render_rows.append(values)
    flat_riser_count = 0
    retained_high_riser = False
    for row in render_rows:
        line_id, front_sector, back_sector = row[2], row[3], row[4]
        lower_texture, side_flags = row[10], row[12]
        should_be_flat = False
        floor_delta = 0
        if back_sector != 0xFFFF:
            front_floor, front_ceiling = sectors[front_sector][:2]
            back_floor, back_ceiling = sectors[back_sector][:2]
            floor_delta = abs(back_floor - front_floor)
            opening = min(front_ceiling, back_ceiling) - max(front_floor, back_floor)
            should_be_flat = (
                not (lines[line_id][2] & 0x0001)
                and opening >= player_height
                and 1 <= floor_delta <= player_max_step
            )
        assert bool(side_flags & 0x02) == should_be_flat, (line_id, row)
        if should_be_flat:
            flat_riser_count += 1
            assert lower_texture == 0xFF, (line_id, row)
        elif floor_delta > player_max_step and lower_texture != 0xFF:
            retained_high_riser = True
    assert flat_riser_count > 0
    assert retained_high_riser
    hand_map = (ROOT / "src/bsp_map_test.c").read_text()
    assert re.search(
        r"const BspRenderSubsector bsp_render_subsectors\[\] = \{\s*"
        r"\{0, 6, 0\}.*?\{6, 4, 1\}.*?\};", hand_map, re.S)

    sector_renderer = (ROOT / "src/sector_render.c").read_text()
    assert "render_planes" not in sector_renderer
    assert "bsp_find_sector" not in sector_renderer
    assert "i < bsp_render_seg_count" not in sector_renderer
    column_loop = re.search(
        r"for \(u16 sx = first_sample; sx <= last_sample; sx\+\+\) \{(.*?)\n    \}",
        sector_renderer, re.S)
    assert column_loop and "/" not in column_loop.group(1)
    assert "g_ceiling_clip[sx] = (s16)(bc - 1)" in sector_renderer
    assert "g_floor_clip[sx] = bf" in sector_renderer
    assert "g_closed_count++" in sector_renderer
    range_closed = re.search(
        r"static bool sector_range_closed\(.*?\n\}", sector_renderer, re.S)
    all_closed = re.search(
        r"static bool sector_all_closed\(.*?\n\}", sector_renderer, re.S)
    assert range_closed and "g_column_closed" in range_closed.group(0)
    assert all_closed and "g_closed_count >= RAY_SAMPLE_COLS" in all_closed.group(0)

    renderer_scene = (ROOT / "src/renderer_scene.c").read_text()
    sector_tile = re.search(
        r"static void build_sector_tile\(u16 tile\) \{(.*?)\n\}",
        renderer_scene, re.S)
    assert sector_tile
    assert "REP4[left[py] & 15]" in sector_tile.group(1)
    assert "REP4[right[py] & 15]" in sector_tile.group(1)
    assert "for (u16 px" not in sector_tile.group(1)
    restore = re.search(
        r"static void restore_previous_overlay_tiles\(void\) \{(.*?)\n\}",
        renderer_scene, re.S)
    assert restore and "overlay_previously_touched(tile)" in restore.group(1)
    assert "build_sector_tilemap();" not in restore.group(1)

    main_source = (ROOT / "src/main.c").read_text()
    exclusive = re.search(
        r"if \(base_dirty\) \{\s*#if BSP_SECTOR_RENDERER\s*"
        r"bsp_sector_cast_frame\(&g_player\);\s*#else\s*"
        r"bsp_cast_frame\(&g_player, g_ray_columns, &g_scene_colors\);\s*#endif",
        main_source, re.S)
    assert exclusive
    print("sector map contract: 85 sectors, 475 lines, 732 segs, 237 subsectors, "
          f"{flat_riser_count} flat risers")


if __name__ == "__main__":
    main()
