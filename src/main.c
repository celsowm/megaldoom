#include <genesis.h>
#include "billboard.h"
#include "bsp_map.h"
#include "bsp_render.h"
#include "debug_checkpoint.h"
#include "fixed_math.h"
#include "frontend.h"
#include "game_audio.h"
#include "player_controller.h"
#include "raycast.h"
#include "renderer.h"
#include "renderer_perf.h"
#include "renderer_redraw.h"
#include "resources.h"

// Shot timers count real vblanks (not loop iterations) so the gun feel does not
// stretch when a motion frame takes ~11 vblanks: 12 vblanks ~= 0.2 s between
// shots at any framerate. The flash is set after its decrement runs, so the
// firing iteration always renders it and it survives >= 1 displayed frame.
#define SHOT_COOLDOWN_VBLANKS 12
#define WEAPON_FLASH_VBLANKS 6
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

// Checkpoint mailbox for BlastEm's deterministic-route runner. Set to 0 (or
// build with -DDEBUG_BLASTEM_CHECKPOINT=0) for clean release builds.
#ifndef DEBUG_BLASTEM_CHECKPOINT
#define DEBUG_BLASTEM_CHECKPOINT 0
#endif

static PlayerState g_player;
static RayColumn g_ray_columns[RAY_VIEW_COLS];
static RaySceneColors g_scene_colors;
static u16 g_weapon_flash = 0;
static u16 g_player_damage_flash = 0;
static u16 g_player_invuln = 0;
static RendererHudState g_hud;
#if DEBUG_BLASTEM_CHECKPOINT
static s32 g_checkpoint_prev_x;
static s32 g_checkpoint_prev_y;
#endif

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

// Drains any still-in-flight view upload (background pump disarmed) before
// code that writes g_view_tiles or queues a new upload. Usually free: the
// cast outlasts the 2-vblank upload, so the V-INT pump has already landed it.
// V-Int callback: the DMA pump runs first (vblank-time-critical, self-gated
// on g_bg_pump_armed), then the gameplay pad poll (self-gated on the active
// flag, inert during frontend/menus). See player_controller_vint_poll.
static void main_vint_callback(void) {
    renderer_upload_background_pump();
    player_controller_vint_poll();
}

static void wait_scene_upload_complete(void) {
    while (renderer_scene_upload_pending()) {
        VDP_waitVSync();
        renderer_upload_scene_step();
    }
}

static void render_current_view(u16 player_health, bool base_dirty) {
#if DEBUG_PERF || CADENCE_STAGE_PROBE
    const u32 cast_start = getSubTick();
#endif
    if (base_dirty) {
        // The cast is pure CPU (writes g_ray_columns only), so vblank
        // interrupts during it may safely DMA the PREVIOUS frame's queued
        // upload — that is the whole overlap win. Note this steals CPU time
        // that lands inside the cast timing below on motion frames.
        renderer_upload_background_arm();
        bsp_cast_frame(&g_player, g_ray_columns, &g_scene_colors);
        renderer_upload_background_disarm();
    }
#if DEBUG_PERF
    renderer_debug_set_cast_subticks(base_dirty ? (getSubTick() - cast_start) : 0);
#elif CADENCE_STAGE_PROBE
    if (base_dirty) {
        g_cadence_cast_subticks += getSubTick() - cast_start;
        g_cadence_rebuild_frames++;
    }
#endif
    // Anything past this point may write g_view_tiles, so the previous
    // frame's upload must have fully landed.
    wait_scene_upload_complete();
    renderer_render_scene(
        g_ray_columns, &g_player, &g_scene_colors, base_dirty,
        g_weapon_flash > 0, g_player_damage_flash > 0,
        (bool)(player_health <= 20));
}

static void reset_level(u16 phase_index, DoomSkill skill, bool *level_cleared, u16 *shot_cooldown,
                        u16 *player_health, u16 *player_armor, u16 *player_ammo,
                        u8 *player_keys, u32 *frame) {
    bsp_map_reset(phase_index);
    billboard_init(phase_index, skill);
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
    (void)hard;

    JOY_init();
    fx_init_tables();
    bsp_init();
    game_audio_init();
    // Inert until armed around the BSP cast (see render_current_view); the
    // frontend/menus run with it installed but never armed.
    SYS_setVIntCallback(main_vint_callback);

    while (TRUE) {
        u32 frame = 0;
        RendererRedrawState redraw;
        bool level_cleared = FALSE;
        u16 phase_index = 0;
        u16 player_health = PLAYER_MAX_HEALTH;
        u16 player_armor = 0;
        u16 player_ammo = PLAYER_START_AMMO;
        u8 player_keys = BSP_KEY_NONE;
        u16 shot_cooldown = 0;
        u16 previous_system_joy;
        u32 prev_vtimer;
        DoomSkill skill;

        skill = frontend_run();
        game_audio_stop_music();
        renderer_init();
        renderer_redraw_init(&redraw);
        game_audio_play_music(test_music);

        reset_level(phase_index, skill, &level_cleared, &shot_cooldown, &player_health,
                    &player_armor, &player_ammo, &player_keys, &frame);
        JOY_update();
        previous_system_joy = JOY_readJoypad(JOY_1);
        prev_vtimer = vtimer;
        // From here on, the V-Int callback is the sole JOY_update caller: it
        // samples every vblank instead of once per (possibly ~11-vblank) main
        // loop iteration, and latches taps that would otherwise land and
        // release between two iterations.
        player_controller_set_poll_active(TRUE);
        debug_checkpoint_mark(DEBUG_CHECKPOINT_GAMEPLAY);

#if DEBUG_PERF
    // Scanline cursor (sprite 0): top = 0% load, bottom = 100% load, averaged.
    SYS_showFrameLoad(TRUE);
#endif

        while (TRUE) {
        u16 control = 0;
        u16 system_joy;
        u16 system_pressed;
        DoorActionResult action_status = g_hud.action_status;
        BillboardShotResult shot_status = g_hud.shot_status;
        BillboardFireResult fire_result = {BILLBOARD_SHOT_NONE, 0, 0, 0, 0};
        // Real vblanks elapsed since last iteration. Keep it clamped for future diagnostics,
        // but now it IS fed to the turn controller so rotation stays time-correct.
        u32 cur_vtimer;
        u16 elapsed_vblanks;
        u16 elapsed_frames;
        u16 latched_pressed;
#if DEBUG_PERF
        const u32 gameplay_start = getSubTick();
#endif

#if DEBUG_PERF
        bsp_debug_reset_query_stats();
        billboard_debug_reset_stats();
#endif

        // The ISR poll (armed above) is the sole JOY_update caller now; just
        // read its cached state and drain whatever it latched since the last
        // iteration.
        latched_pressed = player_controller_consume_latched();
        system_joy = JOY_readJoypad(JOY_1);
        system_pressed = (u16)((system_joy & ~previous_system_joy) | (latched_pressed & BUTTON_START));
        previous_system_joy = system_joy;

        if ((system_pressed & BUTTON_START) != 0) {
            // The pause menu drives its own JOY_update loops; stop the ISR
            // poll so the two never race the same pad read.
            player_controller_set_poll_active(FALSE);
            const FrontendPauseAction pause_action =
                frontend_run_pause(renderer_get_menu_tile_base());
            if (pause_action == FRONTEND_PAUSE_QUIT_TO_TITLE) {
#if DEBUG_PERF
                SYS_showFrameLoad(FALSE);
#endif
                break;
            }

            renderer_restore_after_menu();
            sync_hud(frame, phase_index, player_health, player_armor, player_ammo,
                     player_keys, shot_cooldown, action_status, shot_status, level_cleared);
            renderer_draw_hud(&g_hud);
            renderer_redraw_request_base(&redraw, RENDERER_REDRAW_BASE);
            JOY_update();
            previous_system_joy = JOY_readJoypad(JOY_1);
            prev_vtimer = vtimer;
            // Reseeds the ISR edge baseline from the pad state just read above,
            // so a button still held from before/during the menu does not
            // phantom-fire, and clears any latch accrued while polling was off.
            player_controller_set_poll_active(TRUE);
            continue;
        }

        cur_vtimer = vtimer;
        // Doors (and other wall-clock timers) get the raw vblank delta; the
        // clamped copy below only feeds the movement/turn controller, whose
        // ramp was tuned for a [1,4] step.
        elapsed_vblanks = (u16)(cur_vtimer - prev_vtimer);
        prev_vtimer = cur_vtimer;
        if (elapsed_vblanks < 1) {
            elapsed_vblanks = 1;
        }
        elapsed_frames = (elapsed_vblanks > 4) ? 4 : elapsed_vblanks;
        // elapsed_frames is fed to player_controller_update below so turning is time-correct.

        if (bsp_update_doors(elapsed_vblanks)) {
            renderer_redraw_request_base(&redraw, RENDERER_REDRAW_BASE);
        }

        if (shot_cooldown > 0) {
            shot_cooldown = (shot_cooldown > elapsed_vblanks)
                ? (u16)(shot_cooldown - elapsed_vblanks) : 0;
        }
        if (g_weapon_flash > 0) {
            g_weapon_flash = (g_weapon_flash > elapsed_vblanks)
                ? (u16)(g_weapon_flash - elapsed_vblanks) : 0;
            if (g_weapon_flash == 0) {
                renderer_redraw_request_overlay(&redraw, RENDERER_REDRAW_WEAPON);
            }
        }
        if (g_player_damage_flash > 0) {
            g_player_damage_flash--;
            if (g_player_damage_flash == 0) {
                renderer_redraw_request_overlay(&redraw, RENDERER_REDRAW_DAMAGE);
            }
        }
        if (g_player_invuln > 0) {
            g_player_invuln--;
        }
        if (billboard_update_effects()) {
            renderer_redraw_request_overlay(&redraw, RENDERER_REDRAW_EFFECT);
        }

        if (!level_cleared) {
            control = player_controller_update(&g_player, elapsed_frames, latched_pressed);
#if DEBUG_BLASTEM_CHECKPOINT
            {
                const s32 dx = g_player.x - g_checkpoint_prev_x;
                const s32 dy = g_player.y - g_checkpoint_prev_y;
                if (dx > FX_ONE || dx < -FX_ONE || dy > FX_ONE || dy < -FX_ONE) {
                    debug_checkpoint_mark(DEBUG_CHECKPOINT_MOVED);
                }
                g_checkpoint_prev_x = g_player.x;
                g_checkpoint_prev_y = g_player.y;
            }
#endif
        } else if ((system_pressed & BUTTON_A) != 0) {
            phase_index = (u16)((phase_index + 1) & 1);
            reset_level(phase_index, skill, &level_cleared, &shot_cooldown, &player_health,
                        &player_armor, &player_ammo, &player_keys, &frame);
            renderer_redraw_request_base(&redraw, RENDERER_REDRAW_BASE);
        }

        // PLAYER_CONTROL_PREVIOUS_WEAPON, PLAYER_CONTROL_NEXT_WEAPON and
        // PLAYER_CONTROL_TOGGLE_AUTOMAP are intentionally reserved until the
        // corresponding weapon and automap systems are implemented.

        if ((control & PLAYER_CONTROL_CHANGED) != 0) {
            renderer_redraw_request_base(&redraw, RENDERER_REDRAW_BASE);
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
                renderer_redraw_request_overlay(&redraw, RENDERER_REDRAW_OTHER);
                game_audio_play_sfx(sfx_pickup, sizeof(sfx_pickup), SOUND_PCM_CH2);
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
                renderer_redraw_request_base(&redraw, RENDERER_REDRAW_BASE);
                // Door / platform move sound on PCM channel 3. A toggle or a
                // key-unlock moves the door; a locked bump stays silent.
                if ((action == DOOR_ACTION_TOGGLED) || (action == DOOR_ACTION_UNLOCKED)) {
                    game_audio_play_sfx(sfx_door, sizeof(sfx_door), SOUND_PCM_CH3);
                }
            }
        }

        if ((control & PLAYER_CONTROL_FIRE) != 0) {
            BillboardShotResult shot = BILLBOARD_SHOT_NONE;

            if ((shot_cooldown == 0) && (player_ammo > 0)) {
                debug_checkpoint_mark(DEBUG_CHECKPOINT_COMBAT);
                fire_result = billboard_fire_center(
                    &g_player, g_ray_columns[RAY_VIEW_COLS / 2].depth);
                shot = fire_result.status;
                shot_cooldown = SHOT_COOLDOWN_VBLANKS;
                player_ammo--;
                g_weapon_flash = WEAPON_FLASH_VBLANKS;
                renderer_draw_weapon_flash();
                renderer_redraw_request_overlay(&redraw, RENDERER_REDRAW_WEAPON);

                // Pistol gunshot on PCM channel 2 (channel 1 is reserved for
                // music PCM). Connected-hit SFX go on channel 3 so the gunshot
                // and the enemy reaction never cancel each other out.
                game_audio_play_sfx(sfx_pistol, sizeof(sfx_pistol), SOUND_PCM_CH2);
                if (shot == BILLBOARD_SHOT_DAMAGE) {
                    game_audio_play_sfx(sfx_enemy_pain, sizeof(sfx_enemy_pain), SOUND_PCM_CH3);
                } else if (shot == BILLBOARD_SHOT_KILL) {
                    game_audio_play_sfx(sfx_enemy_death, sizeof(sfx_enemy_death), SOUND_PCM_CH3);
                } else if (fire_result.explosion_count > 0) {
                    game_audio_play_sfx(sfx_barexp, sizeof(sfx_barexp), SOUND_PCM_CH3);
                }

                if ((shot == BILLBOARD_SHOT_DAMAGE) || (shot == BILLBOARD_SHOT_KILL) ||
                    (shot == BILLBOARD_SHOT_EXPLOSION)) {
                    renderer_redraw_request_overlay(
                        &redraw, (shot == BILLBOARD_SHOT_EXPLOSION) ?
                            RENDERER_REDRAW_BARREL : RENDERER_REDRAW_ENEMY_POSE);
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
                        game_audio_play_sfx(sfx_player_death, sizeof(sfx_player_death), SOUND_PCM_CH2);
                        reset_level(phase_index, skill, &level_cleared, &shot_cooldown,
                                    &player_health, &player_armor, &player_ammo,
                                    &player_keys, &frame);
                        renderer_redraw_request_base(&redraw, RENDERER_REDRAW_BASE);
                    } else {
                        player_apply_world_push(&g_player,
                                                (s32)fire_result.push_x * PLAYER_HIT_PUSH_STEP,
                                                (s32)fire_result.push_y * PLAYER_HIT_PUSH_STEP);
                        player_health = (u16)(player_health - damage);
                        g_player_damage_flash = PLAYER_DAMAGE_FLASH_FRAMES;
                        g_player_invuln = PLAYER_INVULN_FRAMES;
                        renderer_redraw_request_overlay(&redraw, RENDERER_REDRAW_DAMAGE);
                        game_audio_play_sfx(sfx_player_pain, sizeof(sfx_player_pain), SOUND_PCM_CH2);
                    }
            }
        }

        if (!level_cleared) {
            const BillboardEnemyUpdate enemy_update = billboard_update_enemies(
                &g_player, renderer_redraw_is_pending(&redraw));

            if (enemy_update.moved) {
                if (enemy_update.position_changed) {
                    renderer_redraw_request_overlay(&redraw, RENDERER_REDRAW_ENEMY_MOVE);
                }
                if (enemy_update.pose_changed) {
                    renderer_redraw_request_overlay(&redraw, RENDERER_REDRAW_ENEMY_POSE);
                }
            }

            if ((enemy_update.hits > 0) && (g_player_invuln == 0)) {
                const u16 armor_absorb = (u16)(((PLAYER_HIT_DAMAGE / 3) < player_armor) ? (PLAYER_HIT_DAMAGE / 3) : player_armor);
                const u16 damage = (u16)(PLAYER_HIT_DAMAGE - armor_absorb);
                player_armor = (u16)(player_armor - armor_absorb);
                if (player_health <= damage) {
                    game_audio_play_sfx(sfx_player_death, sizeof(sfx_player_death), SOUND_PCM_CH2);
                    reset_level(phase_index, skill, &level_cleared, &shot_cooldown,
                                &player_health, &player_armor, &player_ammo,
                                &player_keys, &frame);
                    renderer_redraw_request_base(&redraw, RENDERER_REDRAW_BASE);
                } else {
                    player_apply_world_push(&g_player,
                                            (s32)enemy_update.push_x * PLAYER_HIT_PUSH_STEP,
                                            (s32)enemy_update.push_y * PLAYER_HIT_PUSH_STEP);
                    player_health = (u16)(player_health - damage);
                    g_player_damage_flash = PLAYER_DAMAGE_FLASH_FRAMES;
                    g_player_invuln = PLAYER_INVULN_FRAMES;
                    renderer_redraw_request_overlay(&redraw, RENDERER_REDRAW_DAMAGE);
                    game_audio_play_sfx(sfx_player_pain, sizeof(sfx_player_pain), SOUND_PCM_CH2);
                }
            }
        }

        if (!level_cleared) {
            // Drive the BEXP animation cycle and request an overlay redraw only
            // when its dedicated animator reports a visual transition.
            const BillboardEnemyUpdate barrel_update = billboard_update_barrels(&g_player);

            if (barrel_update.moved) {
                renderer_redraw_request_overlay(&redraw, RENDERER_REDRAW_BARREL);
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

        if (renderer_redraw_is_pending(&redraw)) {
#if DEBUG_PERF
            renderer_debug_set_redraw_reasons(renderer_redraw_reasons(&redraw));
#endif
            render_current_view(player_health, renderer_redraw_base_is_dirty(&redraw));
            renderer_redraw_consume(&redraw);
            renderer_queue_scene_upload(g_ray_columns, &g_scene_colors);
        }

        // Enter vblank first, then push the freshly built frame to VRAM so the
        // ~9.6KB view-tile DMA runs at the fast vblank rate instead of stalling
        // the CPU mid active-display. Then pad to a fixed cadence so each visual
        // frame is shown for the same duration, keeping motion uniform instead of
        // stuttering between 60 and 30fps.
#if DEBUG_PERF
        // Keep the legacy serial shape under DEBUG_PERF (background pump is
        // disabled there) so the perf overlay's per-step upload accounting and
        // total-vblank attribution stay comparable with historic captures.
        VDP_waitVSync();
        renderer_upload_scene_step();
        while (renderer_scene_upload_pending() ||
               ((u16)(vtimer - cur_vtimer) < TARGET_FRAME_VSYNCS)) {
            VDP_waitVSync();
            renderer_upload_scene_step();
        }
#else
        // Do NOT block on upload completion here: a motion frame that already
        // blew past the cadence target skips this loop entirely and its queued
        // upload instead rides the vblank interrupts that fire during the NEXT
        // frame's cast (renderer_upload_background_pump), overlapping the DMA
        // with CPU work. renderer_upload_wait_complete() in render_current_view
        // guarantees it has landed before anything writes g_view_tiles again.
        while ((u16)(vtimer - cur_vtimer) < TARGET_FRAME_VSYNCS) {
            VDP_waitVSync();
            renderer_upload_scene_step();
        }
#endif
#if DEBUG_PERF
        // Total VBlanks consumed by this iteration (target = TARGET_FRAME_VSYNCS,
        // but a frame that spilled past its deadline shows the real cost here).
        // Recorded one iteration ahead of the perf overlay's read.
        renderer_debug_set_total_vblanks((u16)(vtimer - cur_vtimer));
#elif DEBUG_BLASTEM_CHECKPOINT
        // Release-cadence probe: DEBUG_PERF's subtick instrumentation slows the
        // frame so much (per-sample getSubTick calls, the asm-verify probe, the
        // text overlay) that its vblank counts say nothing about what a release
        // build runs at. This branch exists only in checkpoint builds WITHOUT
        // DEBUG_PERF: a handful of adds per iteration plus a 32-byte mailbox
        // copy, so its cadence is representative of release. Published through
        // the same g_debug_perf_mailbox (unused by anything else when
        // DEBUG_PERF is off); decoded by tools/decode-cadence.py.
        {
            typedef struct {
                u16 magic;        // 0xCADE
                u16 last_vblanks;
                u16 max_vblanks;
                u16 missed;       // iterations over TARGET_FRAME_VSYNCS
                u32 iterations;
                u32 vblank_sum;
                u16 hist[8];      // bucket = min(vblanks, 7)
                // Coarse stage accumulators (CADENCE_STAGE_PROBE, see
                // debug_checkpoint.h) so stage shares are measurable at
                // release speed, not just under DEBUG_PERF distortion.
                u32 cast_subticks;
                u32 pack_subticks;
                u32 projection_subticks;
                u32 billboard_subticks;
                u32 rebuild_frames;
                u32 nodes_visited;
                u32 boxes_projected;
                u32 segs_tested;
                u32 segs_drawn;
                u32 drawseg_subticks;
                u32 sample_subticks;
                u32 samples;
                // Traversal attribution for the cast time outside draw_seg.
                u32 box_calls;
                u32 box_near_path;
                u32 box_cheap_reject;
                u32 box_early_out;
                u32 box_subticks;
                u32 range_closed_calls;
                u32 range_closed_subticks;
                u32 all_closed_subticks;
                u32 scene_frames;
                // Billboard raster attribution (see debug_checkpoint.h).
                u32 bb_objects;
                u32 bb_rows;
                u32 bb_bytes;
                u32 bb_opaque;
                u32 bb_commits;
                u32 bb_marks;
                u32 bb_mismatch;
                u32 bb_setup_subticks;
                u32 bb_rows_subticks;
                u32 bb_max_bytes;
                u32 bb_max_subticks;
            } CadenceSnapshot;
            static CadenceSnapshot s_cadence;
            const u16 vb = (u16)(vtimer - cur_vtimer);
            s_cadence.magic = 0xCADE;
            s_cadence.last_vblanks = vb;
            if (vb > s_cadence.max_vblanks) s_cadence.max_vblanks = vb;
            if (vb > TARGET_FRAME_VSYNCS) s_cadence.missed++;
            s_cadence.iterations++;
            s_cadence.vblank_sum += vb;
            s_cadence.hist[(vb < 7) ? vb : 7]++;
            s_cadence.cast_subticks = g_cadence_cast_subticks;
            s_cadence.pack_subticks = g_cadence_pack_subticks;
            s_cadence.projection_subticks = g_cadence_projection_subticks;
            s_cadence.billboard_subticks = g_cadence_billboard_subticks;
            s_cadence.rebuild_frames = g_cadence_rebuild_frames;
            s_cadence.nodes_visited = g_cadence_nodes_visited;
            s_cadence.boxes_projected = g_cadence_boxes_projected;
            s_cadence.segs_tested = g_cadence_segs_tested;
            s_cadence.segs_drawn = g_cadence_segs_drawn;
            s_cadence.drawseg_subticks = g_cadence_drawseg_subticks;
            s_cadence.sample_subticks = g_cadence_sample_subticks;
            s_cadence.samples = g_cadence_samples;
            s_cadence.box_calls = g_cadence_box_calls;
            s_cadence.box_near_path = g_cadence_box_near_path;
            s_cadence.box_cheap_reject = g_cadence_box_cheap_reject;
            s_cadence.box_early_out = g_cadence_box_early_out;
            s_cadence.box_subticks = g_cadence_box_subticks;
            s_cadence.range_closed_calls = g_cadence_range_closed_calls;
            s_cadence.range_closed_subticks = g_cadence_range_closed_subticks;
            s_cadence.all_closed_subticks = g_cadence_all_closed_subticks;
            s_cadence.scene_frames = g_cadence_scene_frames;
            s_cadence.bb_objects = g_cadence_bb_objects;
            s_cadence.bb_rows = g_cadence_bb_rows;
            s_cadence.bb_bytes = g_cadence_bb_bytes;
            s_cadence.bb_opaque = g_cadence_bb_opaque;
            s_cadence.bb_commits = g_cadence_bb_commits;
            s_cadence.bb_marks = g_cadence_bb_marks;
            s_cadence.bb_mismatch = g_cadence_bb_mismatch;
            s_cadence.bb_setup_subticks = g_cadence_bb_setup_subticks;
            s_cadence.bb_rows_subticks = g_cadence_bb_rows_subticks;
            s_cadence.bb_max_bytes = g_cadence_bb_max_bytes;
            s_cadence.bb_max_subticks = g_cadence_bb_max_subticks;
            debug_checkpoint_publish_perf(&s_cadence, sizeof(s_cadence));
        }
#endif
        frame++;
        }
    }

    return 0;
}
