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
from wad_source import EXPECTED_CAMPAIGN_WAD_SHA256 as EXPECTED_SHA256
from e1m1_expected import (E1M1_SEG_COUNT, E1M1_EXIT_SEG_INDEX,
                           E1M1_CERTIFICATE_STATES, E1M1_CURATED_SOURCE_LINEDEF,
                           E1M1_CURATED_TARGET_LINEDEFS)
import doom_map
import world_assets


ROOT = Path(__file__).resolve().parents[1]
WAD_PATH = ROOT / "DOOM1.WAD"
EXPECTED_PALETTE = (
    (0x00, 0x00, 0x00), (0x24, 0x00, 0x00),
    (0x00, 0x00, 0x6D), (0x24, 0x24, 0x24),
    (0x48, 0x24, 0x24), (0x48, 0x48, 0x48),
    (0x6D, 0x48, 0x24), (0x6D, 0x6D, 0x6D),
    (0x91, 0x6D, 0x48), (0x48, 0x48, 0x24),
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
    assert len(baseline.out_segs) == len(candidate.out_segs) == E1M1_SEG_COUNT
    assert candidate.baseline_seg_count == E1M1_SEG_COUNT
    assert candidate.curated_material_linedefs == list(E1M1_CURATED_TARGET_LINEDEFS)
    assert candidate.curated_material_segs == len(E1M1_CURATED_TARGET_LINEDEFS)
    assert [geometry_signature(seg) for seg in baseline.out_segs] == \
        [geometry_signature(seg) for seg in candidate.out_segs]
    assert candidate.out_ssectors == baseline.out_ssectors
    assert candidate.nodes == baseline.nodes
    assert candidate.certificate == baseline.certificate
    assert candidate.certificate["exit_index"] == E1M1_EXIT_SEG_INDEX
    assert candidate.certificate["states"] == E1M1_CERTIFICATE_STATES

    changed = [candidate_seg for baseline_seg, candidate_seg in
               zip(baseline.out_segs, candidate.out_segs)
               if baseline_seg["texture_name"] != candidate_seg["texture_name"]]
    assert len(changed) == len(E1M1_CURATED_TARGET_LINEDEFS)
    assert {seg["source_linedef"] for seg in changed} == set(E1M1_CURATED_TARGET_LINEDEFS)
    assert {seg["texture_name"] for seg in changed} == {"COMPUTE2"}
    assert all(seg["curated_material"] for seg in changed)
    # The recipe matches by geometry (endpoints/heights), not by linedef id, so
    # renumbering (a different, correctly-verified source WAD) shifts which
    # ids these are -- see tools/wad_source.py. What must hold regardless of
    # numbering: the erased portal source line never becomes an output wall,
    # and every retextured target line does.
    emitted_lines = {seg["source_linedef"] for seg in candidate.out_segs}
    assert candidate.curated_material_reports[0]["source_linedef"] not in emitted_lines
    assert set(E1M1_CURATED_TARGET_LINEDEFS) <= emitted_lines
    assert candidate.curated_material_reports == [{
        "name": "start-room-computer-bank",
        "source_linedef": E1M1_CURATED_SOURCE_LINEDEF,
        "source_linedef_hint": 50,
        "target_linedefs": list(E1M1_CURATED_TARGET_LINEDEFS),
        "target_linedef_hints": [40, 41, 42, 52, 53],
        "texture": "COMPUTE2",
        "retextured_segs": len(E1M1_CURATED_TARGET_LINEDEFS),
        "added_segs": 0,
    }]

    map_text = (ROOT / "src/bsp/generated_e1m1_map.c").read_text()
    targets_csv = ",".join(str(v) for v in E1M1_CURATED_TARGET_LINEDEFS)
    # Per-map provenance lives in the map descriptor alone. It used to be
    # asserted of generated_assets.h too, from when that header was emitted per
    # map; the atlas is now shared across E1M1 and E1M2 (one 53-texture catalog
    # for both, see world_assets.emit_world_assets), so it cannot carry any one
    # map's SHA, SEG counts or reachability certificate.
    assert "// Source SHA-256: %s" % EXPECTED_SHA256 in map_text
    assert ("// Flat baseline/final: %d/%d SEGs; curated material: "
            "%d linedefs / %d SEGs" % (
                E1M1_SEG_COUNT, E1M1_SEG_COUNT,
                len(E1M1_CURATED_TARGET_LINEDEFS), len(E1M1_CURATED_TARGET_LINEDEFS))) in map_text
    assert ("// Certified: exit SEG %d reachable after %d states" %
            (E1M1_EXIT_SEG_INDEX, E1M1_CERTIFICATE_STATES)) in map_text
    assert ("// Curated material: start-room-computer-bank source linedef %d "
            "(hint 50) -> targets %s, COMPUTE2, "
            "%d retextured, +0 SEGs" % (
                E1M1_CURATED_SOURCE_LINEDEF, targets_csv,
                len(E1M1_CURATED_TARGET_LINEDEFS))) in map_text


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
        assert len(clean.out_segs) == len(candidate.out_segs) == E1M1_SEG_COUNT
        assert len(paths) == 6
        assert all(path.is_file() for path in paths)


def main():
    test_signature_failures()
    test_current_wad_contract()
    test_palette_texture_window_and_preview_contract()
    print("flat map material-transfer tests passed")


if __name__ == "__main__":
    main()
