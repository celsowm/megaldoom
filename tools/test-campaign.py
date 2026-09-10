#!/usr/bin/env python3
"""Contracts for the two-map campaign, results and classic intermission."""

import hashlib
import re
from pathlib import Path

from wad_source import EXPECTED_CAMPAIGN_WAD_SHA256 as EXPECTED_SHA256
from e1m1_expected import E1M1_HEADER_ROW


ROOT = Path(__file__).resolve().parents[1]


def generated(name):
    return (ROOT / "src" / "bsp" / f"generated_{name}_map.c").read_text()


def secret_bits(source, name):
    match = re.search(
        rf"static const u8 {name}_bsp_secret_sector_bits\[\d+\] = \{{(.*?)\}};",
        source, re.S)
    assert match
    return [int(value) for value in re.findall(r"\d+", match.group(1))]


def percent(value, total):
    return 0 if total == 0 else value * 100 // total


def main():
    assert hashlib.sha256((ROOT / "DOOM1.WAD").read_bytes()).hexdigest().upper() == EXPECTED_SHA256
    e1m1 = generated("e1m1")
    e1m2 = generated("e1m2")
    e1m3 = generated("e1m3")
    e1m4 = generated("e1m4")
    limits = (ROOT / "src" / "bsp" / "generated_map_limits.h").read_text()
    header = (ROOT / "src" / "bsp" / "bsp_map.h").read_text()
    runtime = (ROOT / "src" / "bsp" / "bsp_map.c").read_text()
    main_source = (ROOT / "src" / "main.c").read_text()
    billboard = (ROOT / "src" / "billboard" / "billboard.c").read_text()
    frontend = (ROOT / "src" / "frontend.c").read_text()
    generator = (ROOT / "tools" / "wad-map-extract.py").read_text()
    rom_header = (ROOT / "src" / "boot" / "rom_head.c").read_text()
    bsp_render = (ROOT / "src" / "bsp" / "bsp_render.c").read_text()
    bsp_render_internal = (ROOT / "src" / "bsp" / "bsp_render_internal.h").read_text()

    for token in (
        "MEGALDOOM_MAP_COUNT 4", "MEGALDOOM_MAP_MAX_SEGS 968",
        "MEGALDOOM_MAP_MAX_VERTICES 946", "MEGALDOOM_MAP_MAX_SUBSECTORS 461",
        "MEGALDOOM_MAP_MAX_NODES 460", "MEGALDOOM_MAP_MAX_SECTORS 200",
        "MEGALDOOM_MAP_MAX_ACTIVE_THINGS 317",
    ):
        assert token in limits
    assert "const BspMapData g_e1m1_map" in e1m1
    assert "const BspMapData g_e1m2_map" in e1m2
    assert "const BspMapData g_e1m3_map" in e1m3
    # E1M4 is smaller than E1M2/E1M3 in every dimension, so it rode in under
    # the existing MAX_* ceilings and cost no work RAM at all -- only
    # MAP_COUNT moved. Pin its header row so a regeneration that quietly
    # resized it shows up here rather than as a boot-time heap panic.
    assert "const BspMapData g_e1m4_map" in e1m4
    assert "771u, 780u, 355u, 354u, 8u, 254u, 139u, 3u" in e1m4
    assert "961u, 942u, 448u, 447u, 12u, 262u, 200u, 6u" in e1m2
    assert E1M1_HEADER_ROW in e1m1
    assert "typedef struct {" in header and "BspMapData" in header
    assert "bsp_select_map(u16 level_index)" in runtime
    assert "static const BspMapData *const maps[MEGALDOOM_MAP_COUNT]" in runtime
    assert "&g_e1m1_map, &g_e1m2_map, &g_e1m3_map, &g_e1m4_map," in runtime
    assert "level_index >= MEGALDOOM_MAP_COUNT) return FALSE" in runtime

    bits1 = secret_bits(e1m1, "e1m1")
    bits2 = secret_bits(e1m2, "e1m2")
    assert sum(value.bit_count() for value in bits1) == 3
    assert sum(value.bit_count() for value in bits2) == 6
    # Repeated visits do not change a bitset population.
    visited = 0
    for sector in (5, 5, 17, 17, 5):
        visited |= 1 << sector
    assert visited.bit_count() == 2

    assert percent(0, 0) == 0
    assert percent(1, 3) == 33
    assert percent(3, 3) == 100
    for token in (
        "g_level_kill_total", "billboard_get_kill_count",
        "BILLBOARD_TYPE_BONUS", "BILLBOARD_TYPE_ARMOR_BONUS",
        "g_level_item_count",
    ):
        assert token in billboard
    for token in (
        "enter_level", "pistol_start", "level_progress_reset",
        "level_progress_visit",
        # Par time and music are per-level rows in one table, and the advance
        # is bounded by the campaign length, so a new level needs no new branch.
        "stats.par_seconds = CAMPAIGN[phase_index].par_seconds",
        "static const CampaignLevel CAMPAIGN[MEGALDOOM_MAP_COUNT]",
        "phase_index + 1 < MEGALDOOM_MAP_COUNT", "phase_index++;",
        "enter_level(phase_index, skill, FALSE",
        "*player_keys = BSP_KEY_NONE",
        "game_audio_play_music(CAMPAIGN[phase_index].music)",
    ):
        assert token in main_source
    for token in (
        "FrontendIntermissionStats", "intermission_percent", "kills + 2",
        "items + 2", "secrets + 2", "time_seconds + 3", "par_seconds + 3",
        "INTERMISSION_INPUT", "SYS_doVBlankProcess();",
        "frontend_intermission_splat", "frontend_intermission_pointer0",
        "frontend_intermission_entering_e1m2",
        "frontend_intermission_entering_e1m3",
        "frontend_intermission_stats_e1m3",
        "frontend_intermission_entering_e1m4",
        "frontend_intermission_stats_e1m4",
        "INTERMISSION_NODES[MEGALDOOM_MAP_COUNT]",
    ):
        assert token in frontend
    assert "ensure_extracted_assets" in generator
    assert "os.replace(staged, asset_root)" in generator
    # The pinned hash lives in one place (tools/wad_source.py), imported here
    # and by every other tool that must verify the campaign's source WAD --
    # not copy-pasted per file.
    assert "from wad_source import EXPECTED_CAMPAIGN_WAD_SHA256" in generator
    wad_source = (ROOT / "tools" / "wad_source.py").read_text()
    assert EXPECTED_SHA256 in wad_source
    assert rom_header.count("0x003FFFFF") >= 2
    assert "static u8 g_query_seen_generation[BSP_MAX_SEGS]" in runtime
    assert "u8 g_node_side_generation[BSP_MAX_NODES]" in bsp_render
    assert "extern u8 g_node_side_generation[BSP_MAX_NODES]" in bsp_render_internal
    assert "DEBUG_CHECKPOINT_KEY" in main_source
    assert "DEBUG_CHECKPOINT_EXIT" in main_source

    print("ok    campaign: E1M1..E1M4 descriptors, carry/rebirth, stats and secrets")


if __name__ == "__main__":
    main()
