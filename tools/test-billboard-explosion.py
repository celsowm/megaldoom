#!/usr/bin/env python3
"""Source and generated-asset contracts for explosions and pistol impacts."""

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def doom_radius_damage(dx: int, dy: int, target_radius: int) -> int:
    distance = max(abs(dx), abs(dy)) - target_radius
    distance = max(distance, 0)
    return 0 if distance >= 192 else 128 - (distance * 2) // 3


def main():
    billboard_h = (ROOT / "src/billboard.h").read_text()
    internal_h = (ROOT / "src/billboard_internal.h").read_text()
    billboard_c = (ROOT / "src/billboard.c").read_text()
    combat_c = (ROOT / "src/billboard_combat.c").read_text()
    effects_c = (ROOT / "src/billboard_effects.c").read_text()
    barrel_c = (ROOT / "src/billboard_barrel.c").read_text()
    explosion_c = (ROOT / "src/billboard_explosion.c").read_text()
    explosion_h = (ROOT / "src/billboard_explosion.h").read_text()
    projection_c = (ROOT / "src/billboard_projection.c").read_text()
    # renderer_scene.c was split by SRP into several files; billboard-draw
    # code these checks look for now lives across that set.
    renderer_c = "\n".join((ROOT / "src" / n).read_text() for n in (
        "renderer_scene.c", "renderer_pack.c", "renderer_doors.c",
        "renderer_billboard_draw.c", "renderer_frame_overlay.c",
        "renderer_upload.c", "renderer_sparse.c",
        "renderer_flats.c",
    ))
    main_c = (ROOT / "src/main.c").read_text()
    assets = (ROOT / "src/generated_billboard_assets.h").read_text()
    resources = (ROOT / "res/resources.res").read_text()
    generated_map = (ROOT / "src/generated_e1m1_map.c").read_text()

    # Barrel targeting and direct fire-result handoff.
    assert re.search(
        r"\{BILLBOARD_VISUAL_BARREL,.*?BILLBOARD_MAX_DEPTH, FALSE, TRUE,\s+TRUE\}",
        billboard_c,
    )
    assert "typedef struct {\n    BillboardShotResult status;" in billboard_h
    assert "BillboardFireResult billboard_fire_center" in billboard_h
    assert "billboard_get_last_explosion_result" not in explosion_h
    assert "fire_result = billboard_fire_center(" in main_c
    assert "fire_result.player_damage" in main_c
    assert "fire_result.status == BILLBOARD_SHOT_EXPLOSION" in main_c

    # Doom radius formula: Chebyshev distance, target-radius allowance, falloff.
    assert "u16 billboard_explosion_damage(" in explosion_h
    assert "((dx > dy) ? dx : dy) - target_radius" in explosion_c
    assert "((u32)distance * 2u) / 3u" in explosion_c
    assert "#define BARREL_EXPLOSION_RADIUS 192" in internal_h
    assert doom_radius_damage(0, 0, 16) == 128
    assert doom_radius_damage(36, 0, 16) == 115
    assert doom_radius_damage(64, 0, 16) == 96
    assert doom_radius_damage(64, 64, 16) == 96
    assert doom_radius_damage(207, 0, 16) == 1
    assert doom_radius_damage(208, 0, 16) == 0
    assert "result.player_damage + player_damage" in explosion_c
    assert explosion_c.count("bsp_segment_crosses_wall(") >= 2
    assert "player_damage > strongest_damage" in explosion_c

    # Chain reactions retain live-state guards and the source barrel population.
    assert "object->life_state != ENEMY_ALIVE" in explosion_c
    assert "BARREL_EXPLOSION_MAX_CHAIN" in explosion_c
    assert len(re.findall(r"\b2035u\b", generated_map)) >= 6
    process_blast = explosion_c[explosion_c.index("static void process_blast"):]
    assert "billboard_registry_target_indices()" in process_blast
    assert "billboard_registry_target_count()" in process_blast
    assert "billboard_registry_active_indices()" not in process_blast

    # Exact fastest BEXP cadence: every pose advances on the next update.
    assert "BARREL_DEATH_FRAME_HOLDS[BARREL_DEATH_FRAME_COUNT] = {1, 1, 1, 1, 1}" in internal_h
    assert "object->death_timer > 1" in barrel_c
    assert "BARREL_DEATH_FRAME_HOLDS[object->death_index]" in barrel_c

    # Native explosion geometry grows rather than being clipped to 24x48.
    assert "FREEDOOM_BILLBOARD_BARREL_EXPLOSION_W 60" in assets
    assert "FREEDOOM_BILLBOARD_BARREL_EXPLOSION_H 53" in assets
    geometry = re.search(
        r"FREEDOOM_BILLBOARD_BARREL_EXPLOSION_GEOMETRY.*?= \{(.*?)\n\};",
        assets,
        re.S,
    ).group(1)
    for row in ("{23, 32, 10, 28}", "{60, 53, 29, 49}"):
        assert row in geometry
    assert "FREEDOOM_BILLBOARD_BARREL_EXPLOSION_GEOMETRY[frame]" in billboard_c

    # Four-slot transient effects, native frame sets, shared painter order.
    assert "BILLBOARD_MAX_PROJECTED_EFFECTS 4" in billboard_h
    assert "IMPACT_POOL_COUNT BILLBOARD_MAX_PROJECTED_EFFECTS" in effects_c
    assert "IMPACT_FRAME_HOLD 2" in effects_c
    assert "#define IMPACT_WORLD_SCALE 1" in effects_c
    assert "#define BILLBOARD_WORLD_GEOMETRY_SCALE 1" in internal_h
    assert "IMPACT_MIN_SCREEN_RADIUS 2" in effects_c
    assert "PUFF_WALL_DEPTH_TOLERANCE 64" in renderer_c
    assert "billboard_depth_visible(object, columns[wall_col].depth)" in renderer_c
    assert "#define WALL_PUFF_DEPTH_BIAS 24" in combat_c
    assert "wall_depth - WALL_PUFF_DEPTH_BIAS" in combat_c
    # Hold frames do not redraw; frame transitions and final removal do.
    hold_branch = re.search(
        r"if \(impact->timer > 1\) \{(.*?)continue;\s*\}", effects_c, re.S
    ).group(1)
    assert "changed = TRUE" not in hold_branch
    assert effects_c.index("changed = TRUE;") > effects_c.index("impact->active = FALSE;")
    assert "FREEDOOM_BILLBOARD_PUFF_FRAME_COUNT 4" in assets
    assert "FREEDOOM_BILLBOARD_PUFF_W 15" in assets
    assert "FREEDOOM_BILLBOARD_PUFF_H 15" in assets
    assert "FREEDOOM_BILLBOARD_BLOOD_FRAME_COUNT 3" in assets
    assert "FREEDOOM_BILLBOARD_BLOOD_W 12" in assets
    assert "FREEDOOM_BILLBOARD_BLOOD_H 11" in assets
    assert "billboard_effects_spawn_blood(best_object->x, best_object->y)" in combat_c
    assert "spawn_wall_puff(player, wall_depth)" in combat_c
    assert "billboard_effects_spawn_puff(best_object->x, best_object->y)" in combat_c
    assert "billboard_project_effects(" in projection_c
    assert "BILLBOARD_MAX_PROJECTED_TOTAL" in renderer_c
    assert "BILLBOARD_VISUAL_PUFF" in renderer_c
    assert "BILLBOARD_VISUAL_BLOOD" in renderer_c
    assert "billboard_update_effects()" in main_c
    assert "#define SHOT_COOLDOWN_VBLANKS 12" in main_c

    # Explosion PCM is built into ROM and triggered once per returned event.
    assert 'WAV sfx_barexp       "sound/dsbarexp.wav"  XGM2' in resources
    assert "fire_result.explosion_count > 0" in main_c
    assert "game_audio_play_sfx(sfx_barexp" in main_c

    print("ok    Doom blast falloff, direct damage handoff, fast BEXP, PUFF/BLUD impacts")


if __name__ == "__main__":
    main()
