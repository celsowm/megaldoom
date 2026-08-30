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


def tic_accumulator_tics(vblanks_per_iter, iterations):
    """Mirrors player_controller.c's 35Hz accumulator exactly:
    elapsed_frames = min(vblanks_per_iter, 4); acc += elapsed_frames*35;
    drain while acc>=60. Returns the per-iteration tic count that
    player_controller_tics_last_update() would report each of these
    iterations -- this is the actual source AI cadence is charged from
    (Phase 1), not a raw vblank count.
    """
    elapsed_frames = min(vblanks_per_iter, 4)
    acc = 0
    out = []
    for _ in range(iterations):
        acc += elapsed_frames * 35
        tics = 0
        while acc >= 60:
            acc -= 60
            tics += 1
        out.append(tics)
    return out


def death_sequence_wall_vblanks(hold, poses, vblanks_per_iter, n=200000):
    """Wall-clock vblanks to play every death pose at a given iteration rate.

    Mirrors advance_death fed the REAL per-iteration tic count from the 35Hz
    accumulator (not a raw vblank count -- that is exactly what Phase 1
    changed): the timer is charged tics with a saturating subtract, and at
    most one pose is advanced per call. The excess tics beyond what the
    current pose needed carry into the next pose's hold instead of being
    discarded (the "still a bit slow" follow-up fix) -- discarding them
    measured ~40% slower under heavy motion than this carry-forward version.
    """
    tics_iter = iter(tic_accumulator_tics(vblanks_per_iter, n))
    timer = hold
    index = 0
    elapsed = 0
    while index + 1 < poses:
        tics = next(tics_iter)
        elapsed += vblanks_per_iter
        if timer > tics:
            timer -= tics
            continue
        index += 1
        excess = tics - timer
        timer = (hold - excess) if excess < hold else 1
    return elapsed


def walk_pose_advances(hold, vblanks_per_iter, n_iters):
    """Pose advances and elapsed vblanks for the free-running walk cycle.

    Mirrors the tics > 0 guard in update_dummy_alive: a call whose iteration
    delivered zero tics must leave anim_timer/anim_frame untouched.
    """
    tics_seq = tic_accumulator_tics(vblanks_per_iter, n_iters)
    timer = hold
    advances = 0
    total_vblanks = 0
    for tics in tics_seq:
        total_vblanks += vblanks_per_iter
        if tics == 0:
            continue
        if timer > tics:
            timer -= tics
        else:
            timer = hold
            advances += 1
    return advances, total_vblanks


def main():
    main_c = (ROOT / "src/main.c").read_text()
    enemy_c = (ROOT / "src/billboard/billboard_enemy.c").read_text()
    combat_c = (ROOT / "src/billboard/billboard_combat.c").read_text()
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

    # The blood/puff impact is a static-position pooled effect that never
    # tracks its target after spawning, so it must be spawned AFTER
    # push_dummy_on_hit's knockback (up to 64u/axis), not before -- spawning
    # first left the decal visibly stranded behind the shoved body. Assert the
    # source order directly rather than just the presence of both calls.
    hit_block = combat_c[combat_c.index("if (best_object == NULL)"):
                         combat_c.index("if (best_object->hp > damage)")]
    assert hit_block.index("push_dummy_on_hit(") < hit_block.index(
        "billboard_effects_spawn_blood("), (
        "blood/puff must spawn after push_dummy_on_hit's knockback, or the "
        "decal is left behind the enemy's post-hit position")

    # Phase 1 (docs/ENEMY_AI_IMPROVEMENT_PLAN.md): every enemy AI timer counts
    # the player's own 35 Hz movement tics, not loop iterations and not raw
    # vblanks -- an iteration-counted cadence only reads correctly under a
    # "locked 30fps" that does not hold once a render frame runs long (a
    # motion frame is ~11 vblanks), which is what stretched the death
    # collapse to seconds and halved every enemy's relative speed in combat.
    player_controller_h = (ROOT / "src/player_controller.h").read_text()
    player_controller_c = (ROOT / "src/player_controller.c").read_text()
    internal_h = (ROOT / "src/billboard/billboard_internal.h").read_text()

    # The tic count is sourced from the player's own accumulator (not
    # re-derived), so the two clocks cannot drift apart under a later clamp
    # change -- lockstep by construction, not by agreement between two
    # independent computations.
    assert "u16 player_controller_tics_last_update(void);" in player_controller_h
    assert "static u16 s_tics_last_update = 0;" in player_controller_c
    assert "s_tics_last_update = 0;\n    while (s_doom_tic_accumulator >= VIDEO_VBLANKS_PER_SECOND)" in player_controller_c
    assert "s_tics_last_update++;" in player_controller_c
    assert "u16 player_controller_tics_last_update(void) { return s_tics_last_update; }" in player_controller_c

    # No AI counter is decremented by a bare `--` (the iteration-counted bug);
    # every one is a saturating subtract against the tic count instead.
    for bad in ("object->move_cooldown--", "object->attack_cooldown--",
                "object->spot_cooldown--", "object->attack_anim--"):
        assert bad not in enemy_c, bad
    for good in (
        "object->move_cooldown = (object->move_cooldown > tics)",
        "object->attack_cooldown = (object->attack_cooldown > tics)",
        "object->spot_cooldown = (object->spot_cooldown > tics)",
        "object->attack_anim = (object->attack_anim > tics)",
    ):
        assert good in enemy_c, good
    # A player tic does not fire every iteration (unlike elapsed_vblanks, which
    # main.c floor-clamps to >=1) -- the walk cadence must not advance a pose
    # on a call where zero tics elapsed, or the iteration-counted bug comes
    # back in miniature for exactly that case.
    assert "if (object->attack_anim == 0 && tics > 0)" in enemy_c

    # Movement advances at most one DUMMY_MOVE_STEP per iteration (see
    # try_move_dummy's call sites), so this must stay >= 3: the max tics a
    # single iteration can credit (elapsed_frames clamps to 4 in
    # player_controller.c, so ceil(4 * 35/60) = 3). Dropping it lower silently
    # re-caps pursuit speed at the iteration rate and reintroduces the exact
    # bug Phase 1 fixed, even though every cooldown above is correctly
    # tic-charged.
    move_interval = int(re.search(
        r"#define DUMMY_MOVE_INTERVAL (\d+)", internal_h).group(1))
    assert move_interval >= 3, (
        f"DUMMY_MOVE_INTERVAL is {move_interval}, but a single iteration can "
        "credit up to 3 tics (elapsed_frames clamps to 4, 4*35/60 rounds up "
        "to 3) -- below that, movement is capped by the iteration rate again "
        "regardless of how many tics the cooldown was charged")

    death_hold = int(re.search(
        r"#define ENEMY_DEATH_HOLD_TICS (\d+)", internal_h).group(1))
    poses = int(re.search(
        r"#define ENEMY_DEATH_FRAME_COUNT (\d+)", internal_h).group(1))
    assert "advance_death(BillboardObject *object, u16 tics)" in enemy_c
    assert "object->death_timer > tics" in enemy_c
    # Doom's actual value now that a tic is a tic (was a vblank-scaled
    # approximation, ENEMY_DEATH_HOLD_VBLANKS 9, before this phase).
    assert death_hold == 5, death_hold
    # The carry-forward fix (see below) assumes a single iteration can never
    # deliver more tics than one pose's hold -- max tics/iteration is 3 (the
    # DUMMY_MOVE_INTERVAL invariant above), so the hold must stay >= 3 or the
    # defensive `excess >= hold -> 1` clamp in advance_death starts firing in
    # normal play instead of only as a future-proofing guard.
    assert death_hold >= 3, death_hold

    # "Still a bit slow" follow-up: advance_death used to reset death_timer to
    # a flat ENEMY_DEATH_HOLD_TICS on every pose transition, discarding
    # whatever tics that call delivered beyond what the pose needed. Under
    # heavy motion that discard was the majority of the wasted time. It now
    # carries the excess into the next pose's hold instead.
    assert "object->death_timer = ENEMY_DEATH_HOLD_TICS;" not in enemy_c
    assert "const u16 excess = tics - object->death_timer;" in enemy_c
    assert "(excess < ENEMY_DEATH_HOLD_TICS)" in enemy_c

    # Fed the REAL per-iteration tic count from the 35Hz accumulator (not a
    # raw vblank count), verified against the model in tic_accumulator_tics.
    # Values below are exact outputs of that closed-form simulation, not
    # estimates -- see docs/ENEMY_AI_IMPROVEMENT_PLAN.md Phase 1 for the
    # derivation.
    fast = death_sequence_wall_vblanks(death_hold, poses, 2)    # idle, no clamp
    slow = death_sequence_wall_vblanks(death_hold, poses, 11)   # motion, clamped
    assert fast == 36, fast   # 0.6s
    assert slow == 99, slow  # 1.65s -- was 132 (2.2s) before the carry-forward fix,
                              # ~270 (4.5s) under the original pre-Phase-1 bug
    assert fast <= 60, fast    # idle case stays under a second
    assert slow <= 120, slow   # motion case now stays comfortably under 2 seconds
    assert slow / fast <= 3, (fast, slow)

    # Walk cadence: the tics > 0 guard above is exercised by real conditions,
    # not a defensive no-op -- at the fastest realistic cadence (1 vblank/iter,
    # a solid 60fps), a large fraction of iterations legitimately deliver zero
    # tics, and those calls must leave the pose untouched.
    zero_tic_iterations = tic_accumulator_tics(1, 200).count(0)
    assert zero_tic_iterations > 50, zero_tic_iterations  # not a corner case

    walk_hold = int(re.search(r"#define ENEMY_WALK_HOLD (\d+)", internal_h).group(1))
    idle_advances, idle_vblanks = walk_pose_advances(walk_hold, 2, 6000)
    motion_advances, motion_vblanks = walk_pose_advances(walk_hold, 11, 6000)
    idle_rate = idle_advances / (idle_vblanks / 60.0)
    motion_rate = motion_advances / (motion_vblanks / 60.0)
    # Idle tracks Doom's own cadence closely (35/4 = 8.75 poses/sec); motion
    # stays a real, bounded cadence rather than crawling towards zero -- the
    # pre-Phase-1 bug measured ~1.4 poses/sec here.
    assert 7.5 <= idle_rate <= 9.0, idle_rate
    assert 2.0 <= motion_rate <= 4.0, motion_rate

    # The hint is internal and suppresses only redraw visibility probes.
    assert "bool redraw_pending" in billboard_h
    assert "const u16 enemy_tics = player_dead ? 0 : player_controller_tics_last_update();" in main_c
    assert "billboard_update_enemies(\n                &g_player, renderer_redraw_is_pending(&redraw), enemy_tics)" in main_c
    loop = enemy_c[enemy_c.index("BillboardEnemyUpdate billboard_update_enemies"):]
    assert loop.index("if (object->life_state == ENEMY_DEAD)") < loop.index("hits_before")
    assert "const EnemyVisualChange change =\n            update_dummy" in loop
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
