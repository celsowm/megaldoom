#!/usr/bin/env python3
"""Source contracts for compact billboard/enemy registry ownership."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    registry = (ROOT / "src/billboard_registry.c").read_text()
    billboard = (ROOT / "src/billboard.c").read_text()
    projection = (ROOT / "src/billboard_projection.c").read_text()
    enemy = (ROOT / "src/billboard_enemy.c").read_text()
    combat = (ROOT / "src/billboard_combat.c").read_text()

    assert "s_active_indices[BILLBOARD_OBJECT_COUNT]" in registry
    assert "s_enemy_indices[BILLBOARD_OBJECT_COUNT]" in registry
    assert "s_active_count--" in registry
    assert "s_living_enemy_count--" in registry
    assert "billboard_registry_reset();" in billboard
    assert "billboard_registry_add(object_index);" in billboard
    assert "billboard_registry_deactivate(i);" in billboard
    assert "billboard_registry_active_indices()" in projection
    assert "billboard_registry_enemy_indices()" in enemy
    assert "billboard_registry_enemy_died(best_index);" in combat
    assert "return billboard_registry_living_enemy_count();" in combat

    # The expensive player-distance products must occur only after the active
    # enemy guard in the update loop.
    guard = enemy.index("if (!object->active || (object->type_id != BILLBOARD_TYPE_DUMMY))")
    distance = enemy.index("const s32 dist_sq", guard)
    assert guard < distance

    print("ok    billboard registry: compact active/enemy iteration and cached counts")


if __name__ == "__main__":
    main()
