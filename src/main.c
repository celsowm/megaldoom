#include <genesis.h>
#include "billboard.h"
#include "bsp_map.h"
#include "bsp_render.h"
#include "fixed_math.h"
#include "player_controller.h"
#include "raycast.h"
#include "renderer.h"
#include "resources.h"

#define SHOT_COOLDOWN_FRAMES 6
#define WEAPON_FLASH_FRAMES 2
#define PLAYER_DAMAGE_FLASH_FRAMES 6
#define PLAYER_INVULN_FRAMES 24
#define PLAYER_HIT_PUSH_STEP (FX_ONE / 4)
#define PLAYER_MAX_HEALTH 100
#define PLAYER_MAX_ARMOR 200
#define PLAYER_START_AMMO 50
#define PLAYER_HIT_DAMAGE 20
// Locked frame cadence: every iteration lasts this many vblanks (1 = 60fps, 2 = 30fps,
// 3 = 20fps). A steady cadence is what makes movement feel uniform; the lock only pays
// off when a redraw reliably finishes within this many vblanks. Tune from the DEBUG_PERF
// CPU-load% while moving (turn right + go north): load under ~95% -> 1 holds 60; under
// ~185% -> 2 holds 30; otherwise 3 holds a rock-steady 20. Default 2 targets 30fps
// after consolidating rendering into the single BSP cast.
// TARGET_FRAME_VSYNCS is defined in player_controller.h (shared with the movement ramp).

// Perf diagnostics overlay (FPS + CPU load + frame-load cursor). Set to 0 (or
// build with -DDEBUG_PERF=0) for clean release builds.
#ifndef DEBUG_PERF
#define DEBUG_PERF 0
#endif

static PlayerState g_player;
static RayColumn g_ray_columns[RAY_VIEW_COLS];
static RaySceneColors g_scene_colors;
static u16 g_weapon_flash = 0;
static u16 g_player_damage_flash = 0;
static u16 g_player_invuln = 0;
static RendererHudState g_hud;

static u8 get_portrait_state(u16 player_health) {
    if (g_player_damage_flash > 0) {
        return 1;
    }
    if (player_health <= 20) {
        return 2;
    }

    return 0;
}

static void sync_hud(u32 frame,
                     u16 phase_index,
                     u16 player_health,
                     u16 player_armor,
                     u16 player_ammo,
                     u8 player_keys,
                     u16 shot_cooldown,
                     DoorActionResult action_status,
                     BillboardShotResult shot_status,
                     bool level_cleared) {
    g_hud.frame = frame;
    g_hud.phase = (u16)((phase_index % 99) + 1);
    g_hud.player_health = player_health;
    g_hud.health_percent = (u16)((player_health * 100u) / PLAYER_MAX_HEALTH);
    g_hud.armor = player_armor;
    g_hud.ammo = player_ammo;
    g_hud.key_mask = player_keys;
    g_hud.shot_cooldown = shot_cooldown;
    g_hud.enemy_count = billboard_get_enemy_count();
    g_hud.target_count = billboard_get_target_count();
    g_hud.target_health = billboard_get_target_health();
    g_hud.pickups = billboard_get_pickup_counts();
    g_hud.last_pickup = billboard_get_last_pickup_kind();
    g_hud.action_status = action_status;
    g_hud.shot_status = shot_status;
    g_hud.portrait_state = get_portrait_state(player_health);
    g_hud.level_cleared = level_cleared;
}

static void render_current_view(u16 player_health, bool base_dirty) {
#if DEBUG_PERF
    const u32 cast_start = getSubTick();
#endif
    if (base_dirty) {
        bsp_cast_frame(&g_player, g_ray_columns, &g_scene_colors);
    }
#if DEBUG_PERF
    renderer_debug_set_cast_subticks(base_dirty ? (getSubTick() - cast_start) : 0);
#endif
    renderer_render_scene(
        g_ray_columns, &g_player, &g_scene_colors, base_dirty,
        g_weapon_flash > 0, g_player_damage_flash > 0,
        (bool)(player_health <= 20));
}

static void reset_level(u16 phase_index, bool *level_cleared, u16 *shot_cooldown,
                        u16 *player_health, u16 *player_armor, u16 *player_ammo,
                        u8 *player_keys, u32 *frame) {
    bsp_map_reset(phase_index);
    billboard_init(phase_index);
    player_init(&g_player, phase_index);
    player_controller_reset();
    g_weapon_flash = 0;
    g_player_damage_flash = 0;
    g_player_invuln = 0;
    *level_cleared = FALSE;
    *shot_cooldown = 0;
    *player_health = PLAYER_MAX_HEALTH;
    *player_armor = 0;
    *player_ammo = PLAYER_START_AMMO;
    *player_keys = BSP_KEY_NONE;
    *frame = 0;

    renderer_invalidate_scene();
    renderer_draw_static_screen();
    sync_hud(*frame, phase_index, *player_health, *player_armor, *player_ammo,
             *player_keys, *shot_cooldown, DOOR_ACTION_NONE, BILLBOARD_SHOT_NONE, FALSE);
    renderer_draw_hud(&g_hud);
}

int main(bool hard) {
    u32 frame = 0;
    bool base_dirty = TRUE;
    bool overlay_dirty = TRUE;
    bool level_cleared = FALSE;
    u16 phase_index = 0;
    u16 player_health = PLAYER_MAX_HEALTH;
    u16 player_armor = 0;
    u16 player_ammo = PLAYER_START_AMMO;
    u8 player_keys = BSP_KEY_NONE;
    u16 shot_cooldown = 0;
    u32 prev_vtimer = vtimer;

    (void)hard;

    JOY_init();
    fx_init_tables();
    bsp_init();
    renderer_init();

    // Sound: load the XGM2 Z80 driver and start background music (the E1M1
    // theme, converted from Doom's MUS by tools/midi2vgm). XGM2 has reinforced
    // DMA contention protection, important given MegalDoom's heavy vblank
    // tile-upload DMA. The VGM is converted to XGM2 by rescomp at build time
    // (see res/resources.res). Sound effects (gunshots, pain, doors, pickups)
    // are Doom DS* lumps extracted by tools/extract-sfx.py into res/sound/*.wav
    // and converted by rescomp to XGM2 PCM samples, triggered below with
    // XGM2_playPCM(..) on PCM channels 2/3 (channel 1 stays free for music).
    Z80_loadDriver(Z80_DRIVER_XGM2, TRUE);
    XGM2_play(test_music);

    reset_level(phase_index, &level_cleared, &shot_cooldown, &player_health,
                &player_armor, &player_ammo, &player_keys, &frame);

#if DEBUG_PERF
    // Scanline cursor (sprite 0): top = 0% load, bottom = 100% load, averaged.
    SYS_showFrameLoad(TRUE);
#endif

    while (TRUE) {
        u16 control = 0;
        DoorActionResult action_status = g_hud.action_status;
        BillboardShotResult shot_status = g_hud.shot_status;
        BillboardFireResult fire_result = {BILLBOARD_SHOT_NONE, 0, 0, 0, 0};
        // Real vblanks elapsed since last iteration. Keep it clamped for future diagnostics,
        // but now it IS fed to the turn controller so rotation stays time-correct.
        const u32 cur_vtimer = vtimer;
        u16 elapsed_frames = (u16)(cur_vtimer - prev_vtimer);
#if DEBUG_PERF
        const u32 gameplay_start = getSubTick();
#endif

#if DEBUG_PERF
        bsp_debug_reset_query_stats();
        billboard_debug_reset_stats();
#endif

        prev_vtimer = cur_vtimer;
        if (elapsed_frames < 1) {
            elapsed_frames = 1;
        } else if (elapsed_frames > 4) {
            elapsed_frames = 4;
        }
        // elapsed_frames is fed to player_controller_update below so turning is time-correct.

        if (bsp_update_doors(elapsed_frames)) {
            base_dirty = TRUE;
            overlay_dirty = TRUE;
        }

        if (shot_cooldown > 0) {
            shot_cooldown--;
        }
        if (g_weapon_flash > 0) {
            g_weapon_flash--;
            overlay_dirty = TRUE;
        }
        if (g_player_damage_flash > 0) {
            g_player_damage_flash--;
            overlay_dirty = TRUE;
        }
        if (g_player_invuln > 0) {
            g_player_invuln--;
        }
        if (billboard_update_effects()) {
            overlay_dirty = TRUE;
        }

        JOY_update();
        if (!level_cleared) {
            control = player_controller_update(&g_player, elapsed_frames);
        } else if ((JOY_readJoypad(JOY_1) & BUTTON_START) != 0) {
            phase_index = (u16)((phase_index + 1) & 1);
            reset_level(phase_index, &level_cleared, &shot_cooldown, &player_health,
                        &player_armor, &player_ammo, &player_keys, &frame);
            base_dirty = TRUE;
            overlay_dirty = TRUE;
        }

        // PLAYER_CONTROL_PREVIOUS_WEAPON, PLAYER_CONTROL_NEXT_WEAPON and
        // PLAYER_CONTROL_TOGGLE_AUTOMAP are intentionally reserved until the
        // corresponding weapon and automap systems are implemented.

        if ((control & PLAYER_CONTROL_CHANGED) != 0) {
            base_dirty = TRUE;
            overlay_dirty = TRUE;
            const BillboardPickupResult pickup = billboard_collect_near(g_player.x, g_player.y);
            if (pickup.collected) {
                if (pickup.effect == BILLBOARD_EFFECT_HEALTH) {
                    player_health = (u16)((player_health + pickup.amount > PLAYER_MAX_HEALTH) ? PLAYER_MAX_HEALTH : player_health + pickup.amount);
                } else if (pickup.effect == BILLBOARD_EFFECT_ARMOR) {
                    if (pickup.amount == 1) {
                        player_armor++;
                    } else {
                        player_armor = (u16)((pickup.amount > player_armor) ? pickup.amount : player_armor);
                    }
                    if (player_armor > PLAYER_MAX_ARMOR) player_armor = PLAYER_MAX_ARMOR;
                } else if (pickup.effect == BILLBOARD_EFFECT_AMMO) {
                    player_ammo = (u16)((player_ammo + pickup.amount > 99) ? 99 : player_ammo + pickup.amount);
                } else if (pickup.effect == BILLBOARD_EFFECT_KEY) {
                    player_keys = (u8)(player_keys | pickup.key_mask);
                }
                overlay_dirty = TRUE;
                XGM2_playPCM(sfx_pickup, sizeof(sfx_pickup), SOUND_PCM_CH2);
            }
        }

        if ((control & PLAYER_CONTROL_USE) != 0) {
            const BspUseResult use =
                bsp_use_in_front(g_player.x, g_player.y, g_player.angle, player_keys);
            const DoorActionResult action = use.action;

            action_status = action;

            if (action != DOOR_ACTION_NONE) {
                if (action == DOOR_ACTION_EXIT) {
                    level_cleared = TRUE;
                }
                base_dirty = TRUE;
                overlay_dirty = TRUE;
                // Door / platform move sound on PCM channel 3. A toggle or a
                // key-unlock moves the door; a locked bump stays silent.
                if ((action == DOOR_ACTION_TOGGLED) || (action == DOOR_ACTION_UNLOCKED)) {
                    XGM2_playPCM(sfx_door, sizeof(sfx_door), SOUND_PCM_CH3);
                }
            }
        }

        if ((control & PLAYER_CONTROL_FIRE) != 0) {
            BillboardShotResult shot = BILLBOARD_SHOT_NONE;

            if ((shot_cooldown == 0) && (player_ammo > 0)) {
                fire_result = billboard_fire_center(
                    &g_player, g_ray_columns[RAY_VIEW_COLS / 2].depth);
                shot = fire_result.status;
                shot_cooldown = SHOT_COOLDOWN_FRAMES;
                player_ammo--;
                g_weapon_flash = WEAPON_FLASH_FRAMES;
                overlay_dirty = TRUE;

                // Pistol gunshot on PCM channel 2 (channel 1 is reserved for
                // music PCM). Connected-hit SFX go on channel 3 so the gunshot
                // and the enemy reaction never cancel each other out.
                XGM2_playPCM(sfx_pistol, sizeof(sfx_pistol), SOUND_PCM_CH2);
                if (shot == BILLBOARD_SHOT_DAMAGE) {
                    XGM2_playPCM(sfx_enemy_pain, sizeof(sfx_enemy_pain), SOUND_PCM_CH3);
                } else if (shot == BILLBOARD_SHOT_KILL) {
                    XGM2_playPCM(sfx_enemy_death, sizeof(sfx_enemy_death), SOUND_PCM_CH3);
                } else if (fire_result.explosion_count > 0) {
                    XGM2_playPCM(sfx_barexp, sizeof(sfx_barexp), SOUND_PCM_CH3);
                }

                if ((shot == BILLBOARD_SHOT_DAMAGE) || (shot == BILLBOARD_SHOT_KILL) ||
                    (shot == BILLBOARD_SHOT_EXPLOSION)) {
                    overlay_dirty = TRUE;
                }
            }

            shot_status = shot;
        }

        if (!level_cleared) {
            // Consume the result returned by this exact trigger pull. Keeping
            // damage on the fire result prevents stale HUD shot state or a later
            // explosion from overwriting the player-facing blast outcome.
            if ((fire_result.status == BILLBOARD_SHOT_EXPLOSION) &&
                (fire_result.player_damage > 0) && (g_player_invuln == 0)) {
                    const u16 total_damage = fire_result.player_damage;
                    const u16 armor_absorb = (u16)(((total_damage / 3) < player_armor) ? (total_damage / 3) : player_armor);
                    const u16 damage = (u16)(total_damage - armor_absorb);
                    player_armor = (u16)(player_armor - armor_absorb);
                    if (player_health <= damage) {
                        XGM2_playPCM(sfx_player_death, sizeof(sfx_player_death), SOUND_PCM_CH2);
                        reset_level(phase_index, &level_cleared, &shot_cooldown,
                                    &player_health, &player_armor, &player_ammo,
                                    &player_keys, &frame);
                        base_dirty = TRUE;
                        overlay_dirty = TRUE;
                    } else {
                        player_apply_world_push(&g_player,
                                                (s32)fire_result.push_x * PLAYER_HIT_PUSH_STEP,
                                                (s32)fire_result.push_y * PLAYER_HIT_PUSH_STEP);
                        player_health = (u16)(player_health - damage);
                        g_player_damage_flash = PLAYER_DAMAGE_FLASH_FRAMES;
                        g_player_invuln = PLAYER_INVULN_FRAMES;
                        overlay_dirty = TRUE;
                        XGM2_playPCM(sfx_player_pain, sizeof(sfx_player_pain), SOUND_PCM_CH2);
                    }
            }
        }

        if (!level_cleared) {
            const BillboardEnemyUpdate enemy_update = billboard_update_enemies(&g_player);

            if (enemy_update.moved) {
                overlay_dirty = TRUE;
            }

            if ((enemy_update.hits > 0) && (g_player_invuln == 0)) {
                const u16 armor_absorb = (u16)(((PLAYER_HIT_DAMAGE / 3) < player_armor) ? (PLAYER_HIT_DAMAGE / 3) : player_armor);
                const u16 damage = (u16)(PLAYER_HIT_DAMAGE - armor_absorb);
                player_armor = (u16)(player_armor - armor_absorb);
                if (player_health <= damage) {
                    XGM2_playPCM(sfx_player_death, sizeof(sfx_player_death), SOUND_PCM_CH2);
                    reset_level(phase_index, &level_cleared, &shot_cooldown,
                                &player_health, &player_armor, &player_ammo,
                                &player_keys, &frame);
                    base_dirty = TRUE;
                    overlay_dirty = TRUE;
                } else {
                    player_apply_world_push(&g_player,
                                            (s32)enemy_update.push_x * PLAYER_HIT_PUSH_STEP,
                                            (s32)enemy_update.push_y * PLAYER_HIT_PUSH_STEP);
                    player_health = (u16)(player_health - damage);
                    g_player_damage_flash = PLAYER_DAMAGE_FLASH_FRAMES;
                    g_player_invuln = PLAYER_INVULN_FRAMES;
                    overlay_dirty = TRUE;
                    XGM2_playPCM(sfx_player_pain, sizeof(sfx_player_pain), SOUND_PCM_CH2);
                }
            }
        }

        if (!level_cleared) {
            // Drive the BEXP animation cycle for any detonating barrels and drop
            // them from the registry when the last frame finishes. .moved reuses
            // the same overlay_dirty path so the engine redraws while a barrel
            // is mid-explosion without forcing a full base rebuild.
            const BillboardEnemyUpdate barrel_update = billboard_update_barrels(&g_player);

            if (barrel_update.moved) {
                overlay_dirty = TRUE;
            }
        }

#if DEBUG_PERF
        renderer_debug_set_gameplay_subticks(getSubTick() - gameplay_start);
#endif
        sync_hud(frame, phase_index, player_health, player_armor, player_ammo,
                 player_keys, shot_cooldown, action_status, shot_status, level_cleared);
        renderer_draw_hud(&g_hud);

#if DEBUG_PERF
        // Once per iteration (SYS_getFPS counts calls/sec). Row 0 of BG_A is free.
        // Clear first so a shrinking number (e.g. "426%" -> "25%") leaves no junk.
        // SYS_getFPS counts calls, so keep the FPS call once per game frame.
        // The larger CPU-load field can update less often.
        VDP_showFPS(FALSE, 1, 0);
        if ((frame & 7) == 0) {
            VDP_showCPULoad(10, 0);
        }
#endif

        if (base_dirty || overlay_dirty) {
            render_current_view(player_health, base_dirty);
            base_dirty = FALSE;
            overlay_dirty = FALSE;
            renderer_queue_scene_upload();
        }

        // Enter vblank first, then push the freshly built frame to VRAM so the
        // ~9.6KB view-tile DMA runs at the fast vblank rate instead of stalling
        // the CPU mid active-display. Then pad to a fixed cadence so each visual
        // frame is shown for the same duration, keeping motion uniform instead of
        // stuttering between 60 and 30fps.
        VDP_waitVSync();
        renderer_upload_scene_step();
        while (renderer_scene_upload_pending() ||
               ((u16)(vtimer - cur_vtimer) < TARGET_FRAME_VSYNCS)) {
            VDP_waitVSync();
            renderer_upload_scene_step();
        }
#if DEBUG_PERF
        // Total VBlanks consumed by this iteration (target = TARGET_FRAME_VSYNCS,
        // but a frame that spilled past its deadline shows the real cost here).
        // Recorded one iteration ahead of the perf overlay's read.
        renderer_debug_set_total_vblanks((u16)(vtimer - cur_vtimer));
#endif
        frame++;
    }

    return 0;
}
