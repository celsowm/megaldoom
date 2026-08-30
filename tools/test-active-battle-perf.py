#!/usr/bin/env python3
"""Contracts for semantic-preserving active-battle performance work."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def timer_redraws(duration):
    """Spawn is dirty; held poses are stable; zero transition clears once."""
    value = duration
    redraws = [True]
    while value:
        value -= 1
        redraws.append(value == 0)
    return redraws


def timer_redraws_vblank(duration, per_iter):
    """Vblank-credited timer: saturating subtract per iteration, clear once."""
    value = duration
    redraws = [True]
    while value:
        value = value - per_iter if value > per_iter else 0
        redraws.append(value == 0)
    return redraws


def effect_redraws(frame_count, hold):
    frame = 0
    timer = hold
    active = True
    redraws = [True]  # spawn request
    while active:
        changed = False
        if timer > 1:
            timer -= 1
        elif frame + 1 < frame_count:
            frame += 1
            timer = hold
            changed = True
        else:
            active = False
            changed = True
        redraws.append(changed)
    return redraws


def main():
    main_c = (ROOT / "src/main.c").read_text()
    enemy_c = (ROOT / "src/billboard/billboard_enemy.c").read_text()
    barrel_c = (ROOT / "src/billboard/billboard_barrel.c").read_text()
    billboard_h = (ROOT / "src/billboard/billboard.h").read_text()
    effects_c = (ROOT / "src/billboard/billboard_effects.c").read_text()
    projection_c = (ROOT / "src/billboard/billboard_projection.c").read_text()
    perf_c = (ROOT / "src/renderer/renderer_perf_overlay.c").read_text()
    perf_state_c = (ROOT / "src/renderer/renderer_perf.c").read_text()
    scene_c = (ROOT / "src/renderer/renderer_scene.c").read_text()
    overlay_c = (ROOT / "src/renderer/renderer_overlay.c").read_text()
    redraw_c = (ROOT / "src/renderer/renderer_redraw.c").read_text()

    # Weapon flash counts real vblanks (framerate-independent); damage flash
    # stays iteration-based. Both redraw only on activation and final clear.
    # The flash duration is now per-weapon (WEAPON_DEFS in src/weapons.c), so
    # the cadence property has to hold for EVERY weapon, not just the pistol:
    # a slow motion frame (~11 vblanks) must still display the flash for one
    # full frame, which is what makes muzzle feedback survive at any framerate.
    weapons_c = (ROOT / "src/weapons.c").read_text()
    # Field order: ammo_type, ammo_per_shot, pellets, spread_cols, melee_range,
    # cooldown_vblanks, flash_vblanks, automatic, sfx, sfx_len.
    weapon_flashes = {
        name: int(row.split(",")[6])
        for name, row in re.findall(
            r"^    \[WEAPON_(\w+)\] = \{\s*([^}]*?),\s*$",
            weapons_c, re.S | re.M)
    }
    assert set(weapon_flashes) == {
        "FIST", "CHAINSAW", "PISTOL", "SHOTGUN", "CHAINGUN"}, weapon_flashes
    for flash in weapon_flashes.values():
        assert flash >= 1
        assert timer_redraws_vblank(flash, 11) == [True, True]
    # The pistol keeps the shipped 6-vblank flash, so its feel is unchanged.
    assert weapon_flashes["PISTOL"] == 6
    assert timer_redraws_vblank(6, 2) == [True, False, False, True]
    damage_frames = int(re.search(
        r"#define PLAYER_DAMAGE_FLASH_FRAMES (\d+)", main_c).group(1)
    )
    assert timer_redraws(damage_frames) == [True] + [False] * 5 + [True]
    assert "if (g_weapon_flash == 0)" in main_c
    assert "g_weapon_flash = weapon->flash_vblanks;" in main_c
    assert "if (g_player_damage_flash == 0)" in main_c

    # Four-frame puff and three-frame blood retain two iterations per pose,
    # with redraw only on spawn, pose transition, and final clear.
    assert effect_redraws(4, 2) == [True, False, True, False, True, False, True, False, True]
    assert effect_redraws(3, 2) == [True, False, True, False, True, False, True]
    assert "only pose transitions and final clear" in effects_c

    # The hint is internal and suppresses only redraw visibility probes.
    assert "bool redraw_pending" in billboard_h
    assert "billboard_update_enemies(\n                &g_player, renderer_redraw_is_pending(&redraw))" in main_c
    loop = enemy_c[enemy_c.index("BillboardEnemyUpdate billboard_update_enemies"):]
    assert loop.index("if (object->life_state == ENEMY_DEAD)") < loop.index("hits_before")
    assert "const EnemyVisualChange change = update_dummy" in loop
    assert "const bool was_visible = redraw_pending ? FALSE" in loop
    assert "const bool now_visible = (!redraw_pending && changed)" in loop
    assert "if (!redraw_pending && changed" in loop

    # Expensive distances are deferred to the decisions that consume them.
    alive = enemy_c[enemy_c.index("static bool update_dummy_alive"):enemy_c.index(
        "static bool enemy_affects_view")]
    assert alive.index("if (object->move_cooldown != 0)") < alive.index(
        "const s32 home_dx")
    hit_block = loop[loop.index("if (update.hits > hits_before)"):loop.index(
        "// Pair separation")]
    assert "const s32 dist_sq" in hit_block

    # Every profiling family requested by the plan is visible in DEBUG_PERF.
    for symbol in (
        "s_debug_cache_hits", "s_debug_cache_misses", "s_debug_pair_tests",
        "s_debug_close_pairs", "s_debug_separation_attempts",
        "s_debug_separation_moves", "g_debug_prop_collision_calls",
        "g_debug_prop_collision_scanned",
    ):
        assert symbol in projection_c + enemy_c + (ROOT / "src/billboard/billboard.c").read_text()
    assert "sprintf" not in perf_c
    assert "PERF_OVERLAY_REFRESH_FRAMES 30" in perf_c
    for label in ('TXT(" RR")', 'TXT(" Qc")', 'TXT(" Es")', 'TXT(" P95")',
                  'TXT(" FPS")', 'TXT(" CPU")'):
        assert label in perf_c, label

    # The overlay never touches BG_A (whole-plane scrolled for weapon bob, so
    # text there swings with the gun) and never touches the 3D view's rows. The
    # view is centred, so the letterbox is split: PERF_OVERLAY_TOP_H rows on BG_B
    # above it, the rest on the window plane in the gutter below it.
    assert "VDP_setTileMapDataRect(BG_B, s_perf_tilemap, 0, 0," in perf_c
    assert "VDP_setTileMapDataRect(WINDOW, &s_perf_tilemap[PERF_OVERLAY_TOP_H" in perf_c
    assert "VDP_setTileMapDataRect(BG_A" not in perf_c
    internal = (ROOT / "src/renderer/renderer_internal.h").read_text()
    overlay_h = int(re.search(r"#define PERF_OVERLAY_H (\d+)", perf_c).group(1))
    view_y = int(re.search(r"#define VIEW_TILEMAP_Y (\d+)", internal).group(1))
    hud_h = int(re.search(r"#define FREEDOOM_HUD_TILE_H (\d+)",
                          (ROOT / "src/renderer/generated_hud_assets.h").read_text()).group(1))
    ray_h = int(re.search(r"#define RAY_VIEW_TILE_H (\d+)",
                          (ROOT / "src/raycast.h").read_text()).group(1))
    # Top band + bottom gutter is exactly the letterbox the centred view leaves.
    gutter_h = (28 - hud_h) - (view_y + ray_h)
    assert overlay_h <= view_y + gutter_h, (
        f"perf overlay is {overlay_h} rows tall but the view leaves only "
        f"{view_y} rows above and {gutter_h} below: it would cover the viewport")
    assert "#define PERF_OVERLAY_TOP_H VIEW_TILEMAP_Y" in perf_c

    # The FPS/CPU fields moved into that same overlay. VDP_showFPS and
    # VDP_showCPULoad draw on the text plane, which is BG_A -- they bobbed too.
    assert "VDP_showFPS(" not in main_c
    assert "VDP_showCPULoad(" not in main_c
    assert "renderer_perf_overlay_sample_host(frame);" in main_c

    # SGDK's stock font paints colour index 15; gameplay never writes PAL0[15],
    # so without this the whole overlay renders black on black.
    renderer_c = (ROOT / "src/renderer/renderer.c").read_text()
    assert "PAL_setColor(15, RGB24_TO_VDPCOLOR(0xFFFFFF));" in renderer_c

    # SRP boundaries: main requests redraws through one policy owner, renderer
    # scene records metrics without owning aggregation, and barrels animate in
    # their own subsystem instead of the enemy AI implementation.
    assert "renderer_redraw_request_base" in redraw_c
    assert "renderer_redraw_request_overlay" in redraw_c
    assert "base_dirty = TRUE" not in main_c
    assert "overlay_dirty = TRUE" not in main_c
    assert "static RendererPerfSnapshot s_perf" in perf_state_c
    assert "RendererPerfSnapshot renderer_get_perf_snapshot" in perf_state_c
    assert "RendererPerfSnapshot renderer_get_perf_snapshot" not in scene_c
    assert "void renderer_mark_overlay_tile" in overlay_c
    assert "void renderer_mark_overlay_tile" not in scene_c
    assert "renderer_overlay_restore_previous" in overlay_c
    assert "BillboardEnemyUpdate billboard_update_barrels" in barrel_c
    assert "BillboardEnemyUpdate billboard_update_barrels" not in enemy_c

    # Pair separation caches pre-pass visibility per simulated enemy and checks
    # final visibility once per moved enemy, never once per close pair.
    separation = loop[loop.index("// Pair separation"):loop.index("return update;")]
    pair_loop = separation[:separation.index(
        "// Separation can move one enemy through several close pairs."
    )]
    final_visibility = separation[separation.index(
        "// Separation can move one enemy through several close pairs."
    ):]
    assert "s_simulated_enemy_visibility" in enemy_c
    assert "SEPARATION_WAS_VISIBLE" in enemy_c
    assert "SEPARATION_MOVED" in enemy_c
    assert "enemy_affects_view" not in pair_loop
    assert "if ((visibility & SEPARATION_MOVED) == 0) continue;" in final_visibility
    assert final_visibility.count("enemy_affects_view") == 1

    print("ok    active-battle perf: stable semantics, transition redraws, debug evidence")


if __name__ == "__main__":
    main()
