#!/usr/bin/env python3
"""Contracts for the data-driven full-gameplay E2E manifest."""
import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EVENTS = {"started", "moved", "combat_hit", "interaction", "key", "locked",
          "unlocked", "exit"}


def main():
    manifest_path = ROOT / "tools" / "e2e-levels.json"
    manifest = json.loads(manifest_path.read_text())
    assert manifest["schemaVersion"] == 1
    levels = manifest["levels"]
    limits = (ROOT / "src" / "bsp" / "generated_map_limits.h").read_text()
    map_count = int(re.search(r"MEGALDOOM_MAP_COUNT (\d+)", limits).group(1))

    # This equality is deliberate: adding a campaign map without an E2E
    # scenario must fail the static suite before an untested release exists.
    assert len(levels) == map_count
    assert [case["index"] for case in levels] == list(range(map_count))
    assert len({case["name"] for case in levels}) == map_count

    for case in levels:
        assert set(case) == {"name", "index", "map", "waypoints", "frames", "requiredEvents",
                             "requiredKeys", "requiresLockedDoor"}
        assert case["name"] == f"E1M{case['index'] + 1}"
        assert case["map"] == case["name"]
        assert case["waypoints"].startswith("out/e2e/")
        assert case["frames"] > 0
        assert set(case["requiredEvents"]) <= EVENTS
        assert {"started", "moved", "combat_hit", "interaction", "exit"} <= \
            set(case["requiredEvents"])
        assert 0 <= case["requiredKeys"] <= 0x07
        if case["requiresLockedDoor"]:
            assert case["requiredKeys"] != 0

    assert levels[0]["requiredKeys"] == 0
    assert levels[1]["requiredKeys"] == 0x04
    assert levels[1]["requiresLockedDoor"] is True

    main_source = (ROOT / "src" / "main.c").read_text()
    checkpoint_header = (ROOT / "src" / "debug_checkpoint.h").read_text()
    checkpoint_source = (ROOT / "src" / "debug_checkpoint.c").read_text()
    level_runner = (ROOT / "tools" / "test-level-e2e.ps1").read_text()
    campaign_runner = (ROOT / "tools" / "test-campaign-e2e.ps1").read_text()
    focused_exit = (ROOT / "tools" / "test-e1m1-exit.ps1").read_text()

    for token in ("DEBUG_E2E_START_LEVEL", "DEBUG_E2E_GOD", "DEBUG_E2E_ACTIVE",
                  "DebugE2EState", "DEBUG_E2E_EVENT_UNLOCKED"):
        assert token in checkpoint_header
    assert "volatile DebugE2EState g_debug_e2e_state" in checkpoint_source
    assert "apply_player_damage" in main_source
    assert main_source.count("apply_player_damage(") == 3  # definition + two producers
    assert "debug_e2e_collect_key(pickup.key_mask)" in main_source
    assert "debug_e2e_locked(use.required_key)" in main_source
    assert "debug_e2e_unlocked(use.required_key)" in main_source
    assert "debug_e2e_exit(phase_index)" in main_source
    assert "debug_e2e_use((u8)action, use.target, use.required_key)" in main_source
    assert "g_debug_e2e_state" in level_runner and "-Bytes 20" in level_runner
    assert "useSerial -eq 0" in level_runner
    assert "DEBUG_E2E_GOD=1" in level_runner
    assert "test-level-e2e.ps1" in campaign_runner
    assert "e2e-common.ps1" in focused_exit
    assert "generate-e2e-routes.py" in level_runner
    assert "-Waypoints" in level_runner
    assert "waypoints.complete" in level_runner
    assert "godHits -eq 0" in level_runner
    print("ok    E2E levels: manifest covers %d campaign maps" % map_count)


if __name__ == "__main__":
    main()
