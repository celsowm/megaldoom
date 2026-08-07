#!/usr/bin/env python3
"""Source contracts for compact billboard/enemy registry ownership."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    registry = (ROOT / "src/billboard/billboard_registry.c").read_text()
    billboard = (ROOT / "src/billboard/billboard.c").read_text()
    projection = (ROOT / "src/billboard/billboard_projection.c").read_text()
    enemy = (ROOT / "src/billboard/billboard_enemy.c").read_text()
    combat = (ROOT / "src/billboard/billboard_combat.c").read_text()

    assert "s_active_indices[BILLBOARD_OBJECT_COUNT]" in registry
    assert "s_enemy_indices[BILLBOARD_OBJECT_COUNT]" in registry
    assert "s_blocking_indices[BILLBOARD_OBJECT_COUNT]" in registry
    assert "s_blocking_count = 0;" in registry
    assert "if (type->blocking)" in registry
    assert "s_blocking_indices[s_blocking_count++]" in registry
    assert "remove_index_stable" in registry
    assert "indices[i] = indices[i + 1];" in registry
    assert "remove_index_stable(s_active_indices, &s_active_count, index);" in registry
    assert "remove_index_stable(s_target_indices, &s_target_count, index);" in registry
    assert "remove_index_stable(s_blocking_indices, &s_blocking_count, index);" in registry
    assert "s_living_enemy_count--" in registry
    assert "billboard_registry_reset();" in billboard
    assert "billboard_registry_add(object_index);" in billboard
    assert "billboard_registry_deactivate(i);" in billboard
    assert "billboard_registry_active_indices()" in projection
    assert "billboard_registry_enemy_indices()" in enemy
    collision = billboard[billboard.index("bool billboard_position_blocked"):]
    assert "billboard_registry_blocking_indices()" in collision
    assert "billboard_registry_blocking_count()" in collision
    assert "billboard_registry_active_indices()" not in collision
    assert "billboard_registry_enemy_died(best_index);" in combat
    assert "return billboard_registry_living_enemy_count();" in combat

    # Deactivating any object removes it from each applicable compact registry.
    # A pickup or enemy is absent from the blocker list, so the stable helper is
    # a no-op there; a barrel is removed without perturbing remaining order.
    deactivate = registry[registry.index("void billboard_registry_deactivate"):]
    assert deactivate.index("remove_index_stable(s_active_indices") < deactivate.index(
        "g_billboards[index].active = FALSE"
    )
    assert "remove_index_stable" not in registry[registry.index(
        "void billboard_registry_enemy_died"
    ):registry.index("const u8 *billboard_registry_active_indices")]

    # The expensive player-distance products must occur only after the active
    # enemy guard in the update loop.
    guard = enemy.index("if (!object->active || (object->type_id != BILLBOARD_TYPE_DUMMY))")
    distance = enemy.index("const s32 dist_sq", guard)
    assert guard < distance

    print("ok    billboard registry: compact active/enemy/blocking iteration and stable removal")


if __name__ == "__main__":
    main()
