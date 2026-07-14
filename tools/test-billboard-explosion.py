#!/usr/bin/env python3
"""Source contracts for the explosive-barrel feature.

These contracts are intentionally implementer-agnostic: they assert that the
plumbing exists in the right files at the right level, not that any specific
runtime behavior occurs (the runtime is exercised by the ROM build + check-rom
guardrails). Mirrors tools/test-billboard-registry.py in style.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def main():
    billboard_h = (ROOT / "src/billboard.h").read_text()
    billboard_internal_h = (ROOT / "src/billboard_internal.h").read_text()
    billboard_c = (ROOT / "src/billboard.c").read_text()
    billboard_combat_c = (ROOT / "src/billboard_combat.c").read_text()
    billboard_enemy_c = (ROOT / "src/billboard_enemy.c").read_text()
    billboard_registry_c = (ROOT / "src/billboard_registry.c").read_text()
    billboard_explosion_c = (ROOT / "src/billboard_explosion.c").read_text()
    billboard_explosion_h = (ROOT / "src/billboard_explosion.h").read_text()
    main_c = (ROOT / "src/main.c").read_text()
    generated_assets_h = (ROOT / "src/generated_billboard_assets.h").read_text()
    generated_geometry_h = (ROOT / "src/generated_billboard_geometry.h").read_text()
    generated_e1m1_map_c = (ROOT / "src/generated_e1m1_map.c").read_text()

    # 1. BARREL type is targetable. The BILLBOARD_TYPES[BILLBOARD_TYPE_BARREL]
    #    row's flag column (collectible, targetable, blocking) was flipped so
    #    the pistol hit-scan can acquire a barrel.
    barrel_row = re.search(
        r"\{BILLBOARD_VISUAL_BARREL,.*?BILLBOARD_MAX_DEPTH, FALSE, TRUE,  TRUE\}",
        billboard_c)
    assert barrel_row is not None, (
        "BARREL row must have targetable=TRUE (FALSE, TRUE, TRUE)."
    )

    # 2. BillboardShotResult gains an EXPLOSION variant.
    assert "BILLBOARD_SHOT_EXPLOSION = 3" in billboard_h, (
        "BillboardShotResult must include BILLBOARD_SHOT_EXPLOSION = 3."
    )

    # 3. fire_center must walk the union target list (DUMMY + BARREL), not the
    #    AI-only enemy list. The literal accessor call is the contract.
    assert "billboard_registry_target_indices()" in billboard_combat_c, (
        "billboard_fire_center must iterate billboard_registry_target_indices()."
    )

    # 4. The new visual ID lives in the enum AND in both generated headers.
    assert "BILLBOARD_VISUAL_BARREL_EXPLODING = 19" in billboard_h, (
        "billboard.h must declare BILLBOARD_VISUAL_BARREL_EXPLODING = 19."
    )
    assert "FREEDOOM_BILLBOARD_BARREL_EXPLOSION_FRAMES" in generated_assets_h, (
        "generated_billboard_assets.h must bake the BEXP frame array."
    )
    assert "FREEDOOM_BILLBOARD_BARREL_EXPLOSION_FRAME_COUNT 5" in generated_assets_h, (
        "generated_billboard_assets.h must expose the 5-frame count."
    )
    # The static world-texture count stays at 17 (items/props only). The BEXP
    # frames live in a separate dedicated array, not the world-texture array.
    assert "FREEDOOM_BILLBOARD_WORLD_TEXTURE_COUNT 17" in generated_assets_h, (
        "World-texture count must stay at 17 (BARREL_EXPLODING uses a sibling array)."
    )
    assert "FREEDOOM_BILLBOARD_WORLD_GEOMETRY_COUNT 17" in generated_geometry_h, (
        "World-geometry count must stay at 17."
    )

    # 5. The explosion routine is defined in its own translation unit and wired
    #    in from the hit-scan kill branch.
    assert "BarrelExplosionResult billboard_apply_explosion(" in billboard_explosion_h, (
        "billboard_explosion.h must declare billboard_apply_explosion."
    )
    assert "billboard_apply_explosion(player, best_object->x, best_object->y);" in billboard_combat_c, (
        "billboard_combat.c must call billboard_apply_explosion on a barrel kill."
    )

    # 6. The per-frame death clock is defined and called from main.c.
    assert "BillboardEnemyUpdate billboard_update_barrels(const PlayerState *player);" in billboard_h, (
        "billboard.h must declare billboard_update_barrels."
    )
    assert "advance_barrel_death" in billboard_enemy_c, (
        "billboard_enemy.c must implement advance_barrel_death."
    )
    assert "billboard_update_barrels(&g_player)" in main_c, (
        "main.c must call billboard_update_barrels each frame."
    )

    # 7. AoE radius is the literal 128 Doom map units.
    assert "#define BARREL_EXPLOSION_RADIUS 128" in billboard_internal_h, (
        "BARREL_EXPLOSION_RADIUS must be 128 (literal Doom units)."
    )
    assert "#define BARREL_EXPLOSION_RADIUS_SQ (BARREL_EXPLOSION_RADIUS * BARREL_EXPLOSION_RADIUS)" in billboard_internal_h, (
        "BARREL_EXPLOSION_RADIUS_SQ must be the squared radius."
    )

    # 8. The original Doom barrel THING type (2035) is still mapped. The full
    #    population test (test-billboard-population.py) covers count; here we
    #    just verify the case still exists.
    assert "case 2035:" in billboard_c, (
        "map_thing_type must still route case 2035 to BARREL."
    )

    # 9. Chain-reaction guard: a barrel already flagged ENEMY_DYING must be
    #    short-circuited so each barrel explodes exactly once.
    assert "life_state != ENEMY_ALIVE" in billboard_explosion_c, (
        "billboard_explosion.c must short-circuit when life_state != ENEMY_ALIVE."
    )

    # 10. Render pipeline: the per-object visual override flips dying barrels
    #     to BARREL_EXPLODING so the renderer pulls pixels from the BEXP array.
    assert "BILLBOARD_VISUAL_BARREL_EXPLODING" in billboard_c, (
        "billboard_get_object_visual_id must reference BILLBOARD_VISUAL_BARREL_EXPLODING."
    )

    # 11. The six E1M1 barrels still appear in the generated map (the population
    #     test asserts a count of 6 barrels separately; this is the literal
    #     waveform check that the THING generation did not regress to a different
    #     type id).
    barrel_count = len(re.findall(r"\b2035u\b", generated_e1m1_map_c))
    assert barrel_count >= 6, f"E1M1 must still place >=6 barrels (2035u); found {barrel_count}."

    # Damage field of barrel_explosion.c must apply AoE to the player using the
    # wall occlusion check matching pistol LoS parity.
    assert "bsp_segment_hits_wall(bx, by, player->x, player->y)" in billboard_explosion_c, (
        "Player AoE must apply wall-occlusion via bsp_segment_hits_wall."
    )

    # The registry must publish the target list (DUMMY + BARREL union) and prune
    # it on deactivate so dying barrels exit both the active AND target arrays.
    assert "s_target_indices[BILLBOARD_OBJECT_COUNT]" in billboard_registry_c, (
        "billboard_registry.c must hold a target_indices array."
    )
    assert "s_target_count--" in billboard_registry_c, (
        "billboard_registry_deactivate must prune s_target_indices."
    )
    assert "billboard_registry_target_indices(void)" in billboard_registry_c
    assert "billboard_registry_target_count(void)" in billboard_registry_c

    print("ok    billboard explosion: target registry, hit-scan, AoE chains, BEXP death clock")


if __name__ == "__main__":
    main()
