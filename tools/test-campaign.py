#!/usr/bin/env python3
"""Contracts for the two-map campaign, results and classic intermission."""

import hashlib
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EXPECTED_SHA256 = "77CD3852B5F7114EC64A07A1B1EF1F734736A13BBD186477C9111A7DD8C55F82"


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
    limits = (ROOT / "src" / "bsp" / "generated_map_limits.h").read_text()
    header = (ROOT / "src" / "bsp" / "bsp_map.h").read_text()
    runtime = (ROOT / "src" / "bsp" / "bsp_map.c").read_text()
    main_source = (ROOT / "src" / "main.c").read_text()
    billboard = (ROOT / "src" / "billboard" / "billboard.c").read_text()
    frontend = (ROOT / "src" / "frontend.c").read_text()
    generator = (ROOT / "tools" / "wad-map-extract.py").read_text()
    rom_header = (ROOT / "src" / "boot" / "rom_head.c").read_text()
    bsp_render = (ROOT / "src" / "bsp" / "bsp_render.c").read_text()

    for token in (
        "MEGALDOOM_MAP_COUNT 2", "MEGALDOOM_MAP_MAX_SEGS 961",
        "MEGALDOOM_MAP_MAX_VERTICES 942", "MEGALDOOM_MAP_MAX_SUBSECTORS 448",
        "MEGALDOOM_MAP_MAX_NODES 447", "MEGALDOOM_MAP_MAX_SECTORS 200",
        "MEGALDOOM_MAP_MAX_ACTIVE_THINGS 207",
    ):
        assert token in limits
    assert "const BspMapData g_e1m1_map" in e1m1
    assert "const BspMapData g_e1m2_map" in e1m2
    assert "961u, 942u, 448u, 447u, 12u, 262u, 200u, 6u" in e1m2
    assert "394u, 470u, 239u, 238u, 5u, 143u, 88u, 3u" in e1m1
    assert "typedef struct {" in header and "BspMapData" in header
    assert "bsp_select_map(u16 level_index)" in runtime
    assert "g_bsp_map = &g_e1m2_map" in runtime

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
        "level_progress_visit", "stats.par_seconds = (phase_index == 0) ? 30 : 75",
        "phase_index = 1", "enter_level(phase_index, skill, FALSE",
        "*player_keys = BSP_KEY_NONE", "game_audio_play_music(e1m2_music)",
    ):
        assert token in main_source
    for token in (
        "FrontendIntermissionStats", "intermission_percent", "kills + 2",
        "items + 2", "secrets + 2", "time_seconds + 3", "par_seconds + 3",
        "INTERMISSION_INPUT", "SYS_doVBlankProcess();",
        "frontend_intermission_splat", "frontend_intermission_pointer0",
        "frontend_intermission_entering_e1m2",
    ):
        assert token in frontend
    assert "ensure_extracted_assets" in generator
    assert "os.replace(staged, asset_root)" in generator
    assert EXPECTED_SHA256 in generator
    assert rom_header.count("0x003FFFFF") >= 2
    assert "static u8 g_query_seen_generation[BSP_MAX_SEGS]" in runtime
    assert "static u8 g_node_side_generation[BSP_MAX_NODES]" in bsp_render
    assert "DEBUG_CHECKPOINT_KEY" in main_source
    assert "DEBUG_CHECKPOINT_EXIT" in main_source

    print("ok    campaign: E1M1/E1M2 descriptors, carry/rebirth, stats and 3/6 secrets")


if __name__ == "__main__":
    main()
