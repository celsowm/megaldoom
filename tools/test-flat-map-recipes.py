#!/usr/bin/env python3
"""Contracts for structurally curated strict-2D material transfers."""

from collections import Counter
from dataclasses import replace
from pathlib import Path
import tempfile

import flat_map_preview
from flat_map_recipes import (FLAT_MATERIAL_TRANSFER_RECIPES,
                              resolve_flat_material_transfers)
from wad_reader import WadFile
import doom_map
import world_assets


ROOT = Path(__file__).resolve().parents[1]
WAD_PATH = ROOT / "DOOM1.WAD"
EXPECTED_SHA256 = "77CD3852B5F7114EC64A07A1B1EF1F734736A13BBD186477C9111A7DD8C55F82"
EXPECTED_PALETTE = (
    (0x00, 0x00, 0x00), (0x24, 0x00, 0x00),
    (0x00, 0x00, 0x6D), (0x24, 0x24, 0x24),
    (0x48, 0x24, 0x24), (0x48, 0x48, 0x48),
    (0x6D, 0x48, 0x24), (0x6D, 0x6D, 0x6D),
    (0x91, 0x6D, 0x48), (0x6D, 0x91, 0x6D),
    (0x91, 0x91, 0x91), (0xFF, 0x48, 0x48),
    (0xB6, 0x91, 0x6D), (0xB6, 0xB6, 0xB6),
    (0xDA, 0x24, 0x24), (0xDA, 0xB6, 0x48),
)


def expect_failure(fragment, callback):
    try:
        callback()
    except ValueError as error:
        assert fragment in str(error), error
    else:
        raise AssertionError("expected recipe failure containing %r" % fragment)


def synthetic_recipe_fixture():
    recipe = FLAT_MATERIAL_TRANSFER_RECIPES["E1M1"][0]
    target = recipe.targets[0]
    recipe = replace(recipe, targets=(target,))
    vertices = [recipe.source.endpoints[0], recipe.source.endpoints[1],
                target.endpoints[0], target.endpoints[1]]
    linedefs = [
        dict(v1=0, v2=1, right=0, left=1),
        dict(v1=2, v2=3, right=2, left=0xFFFF),
    ]
    sidedefs = [
        dict(sector=0, upper="COMPUTE2", lower="STEP6", middle="-"),
        dict(sector=1, upper="-", lower="-", middle="-"),
        dict(sector=2, upper="-", lower="-", middle="STARTAN3"),
    ]
    sectors = [
        dict(floor=-8, ceiling=120),
        dict(floor=0, ceiling=72),
        dict(floor=0, ceiling=72),
    ]
    return recipe, vertices, linedefs, sidedefs, sectors


def test_signature_failures():
    recipe, vertices, linedefs, sidedefs, sectors = synthetic_recipe_fixture()
    resolved = resolve_flat_material_transfers(
        "E1M1", vertices, linedefs, sidedefs, sectors, (recipe,))
    assert list(resolved) == [1]
    assert resolved[1].source_linedef == 0
    assert resolved[1].target_side_id == 2

    expect_failure("expected one linedef", lambda: resolve_flat_material_transfers(
        "E1M1", [(895, 3360)] + vertices[1:], linedefs, sidedefs,
        sectors, (recipe,)))
    drifted_sectors = [dict(sector) for sector in sectors]
    drifted_sectors[0]["ceiling"] = 119
    expect_failure("sector heights drifted", lambda: resolve_flat_material_transfers(
        "E1M1", vertices, linedefs, sidedefs, drifted_sectors, (recipe,)))
    drifted_sides = [dict(side) for side in sidedefs]
    drifted_sides[0]["upper"] = "STEP6"
    expect_failure("expected right.upper=COMPUTE2", lambda:
        resolve_flat_material_transfers(
            "E1M1", vertices, linedefs, drifted_sides, sectors, (recipe,)))
    target_drifted = [dict(sector) for sector in sectors]
    target_drifted[2]["ceiling"] = 71
    expect_failure("material target", lambda: resolve_flat_material_transfers(
        "E1M1", vertices, linedefs, sidedefs, target_drifted, (recipe,)))
    target_sides = [dict(side) for side in sidedefs]
    target_sides[2]["middle"] = "STONE2"
    expect_failure("expected right.middle=STARTAN3", lambda:
        resolve_flat_material_transfers(
            "E1M1", vertices, linedefs, target_sides, sectors, (recipe,)))


def geometry_signature(seg):
    return tuple(seg[key] for key in (
        "v1", "v2", "nx", "ny", "tex_u_offset", "tex_v_offset",
        "type", "door_group", "required_key", "flags", "source_linedef"))


def test_current_wad_contract():
    wad = WadFile(str(WAD_PATH))
    baseline = doom_map.load_map(wad, "E1M1", apply_recipes=False)
    candidate = doom_map.load_map(wad, "E1M1", apply_recipes=True)
    assert baseline.wad_sha256 == EXPECTED_SHA256
    assert candidate.wad_sha256 == EXPECTED_SHA256
    assert len(baseline.out_segs) == len(candidate.out_segs) == 394
    assert candidate.baseline_seg_count == 394
    assert candidate.curated_material_linedefs == [40, 41, 42, 52, 53]
    assert candidate.curated_material_segs == 5
    assert [geometry_signature(seg) for seg in baseline.out_segs] == \
        [geometry_signature(seg) for seg in candidate.out_segs]
    assert candidate.out_ssectors == baseline.out_ssectors
    assert candidate.nodes == baseline.nodes
    assert candidate.certificate == baseline.certificate
    assert candidate.certificate["exit_index"] == 309
    assert candidate.certificate["states"] == 164

    changed = [candidate_seg for baseline_seg, candidate_seg in
               zip(baseline.out_segs, candidate.out_segs)
               if baseline_seg["texture_name"] != candidate_seg["texture_name"]]
    assert len(changed) == 5
    assert {seg["source_linedef"] for seg in changed} == {40, 41, 42, 52, 53}
    assert {seg["texture_name"] for seg in changed} == {"COMPUTE2"}
    assert all(seg["curated_material"] for seg in changed)
    emitted_lines = {seg["source_linedef"] for seg in candidate.out_segs}
    assert not ({37, 48, 49, 50, 51, 54} & emitted_lines)
    assert candidate.curated_material_reports == [{
        "name": "start-room-computer-bank",
        "source_linedef": 50,
        "source_linedef_hint": 50,
        "target_linedefs": [40, 41, 42, 52, 53],
        "target_linedef_hints": [40, 41, 42, 52, 53],
        "texture": "COMPUTE2",
        "retextured_segs": 5,
        "added_segs": 0,
    }]

    map_text = (ROOT / "src/bsp/generated_e1m1_map.c").read_text()
    asset_text = (ROOT / "src/bsp/generated_assets.h").read_text()
    for text in (map_text, asset_text):
        assert "// Source SHA-256: %s" % EXPECTED_SHA256 in text
        assert ("// Flat baseline/final: 394/394 SEGs; curated material: "
                "5 linedefs / 5 SEGs") in text
        assert "// Certified: exit SEG 309 reachable after 164 states" in text
    assert ("// Curated material: start-room-computer-bank source linedef 50 "
            "(hint 50) -> targets 40,41,42,52,53, COMPUTE2, "
            "5 retextured, +0 SEGs") in map_text


def test_palette_texture_window_and_preview_contract():
    assert world_assets.FROZEN_WORLD_PALETTE == EXPECTED_PALETTE
    assert world_assets.build_world_palette([], {}, []) == list(EXPECTED_PALETTE)
    assert world_assets.GLOBAL_FLOOR_INDEX == 7
    floor = EXPECTED_PALETTE[world_assets.GLOBAL_FLOOR_INDEX]
    assert floor == (0x6D, 0x6D, 0x6D)
    assert floor[0] == floor[1] == floor[2]
    assert world_assets.CURATED_TEXTURE_WINDOWS["COMPUTE2"] == (128, 0, 128, 56)
    assert world_assets.sampled_texture_dimensions("COMPUTE2", 256, 56) == (128, 56)
    converted = world_assets.convert_texture(
        world_assets.texture_path("COMPUTE2"), list(EXPECTED_PALETTE))
    counts = Counter(index for column in converted for index in column)
    assert counts[9] >= 80, counts

    with tempfile.TemporaryDirectory() as temp_dir:
        clean, candidate, paths = flat_map_preview.build_preview(
            output_dir=Path(temp_dir))
        assert len(clean.out_segs) == len(candidate.out_segs) == 394
        assert len(paths) == 6
        assert all(path.is_file() for path in paths)


def main():
    test_signature_failures()
    test_current_wad_contract()
    test_palette_texture_window_and_preview_contract()
    print("flat map material-transfer tests passed")


if __name__ == "__main__":
    main()
