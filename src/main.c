#include <genesis.h>
#include "automap.h"
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
#include "weapons.h"

// What the player is carrying. Bundled into one struct rather than four more
// out-parameters: reset_level and sync_hud already thread the whole player
// state by pointer, and this keeps both signatures readable.
typedef struct {
    u16 ammo[AMMO_TYPE_COUNT];
    u8 current;  // WeaponId
    u8 owned;    // bitmask of WEAPON_OWNED_BIT(WeaponId)
} PlayerArsenal;

#define LEVEL_SECRET_BYTES ((MEGALDOOM_MAP_MAX_SECTORS + 7) / 8)
typedef struct {
    u32 time_vblanks;
    u16 secrets_found;
    u8 visited_secret_bits[LEVEL_SECRET_BYTES];
} LevelProgress;

// Shot timers count real vblanks (not loop iterations) so the gun feel does not
// stretch when a motion frame takes ~11 vblanks: the pistol's 12 vblanks is
// ~0.2 s between shots at any framerate. The per-weapon cooldown and flash
// durations live in WEAPON_DEFS (src/weapons.c); the flash is set after its
// decrement runs, so the firing iteration always renders it and it survives
// >= 1 displayed frame.
#define PLAYER_DAMAGE_FLASH_FRAMES 6
#define PLAYER_INVULN_FRAMES 24
#define PLAYER_HIT_PUSH_STEP (FX_ONE / 4)
#define PLAYER_MAX_HEALTH 100
#define PLAYER_MAX_ARMOR 200
#define PLAYER_HIT_DAMAGE 20
// Doom raises a new weapon before it can fire. One cooldown's worth of vblanks
// is enough to stop a switch from being a free instant shot.
#define WEAPON_RAISE_VBLANKS 10
// Doom locks respawn input for roughly a second after death (PST_REBORN) so a
// still-held fire button from the killing blow cannot instantly restart the
// level. Counted in real vblanks, same unit as elapsed_vblanks.
#define DEATH_INPUT_LOCKOUT_VBLANKS 35
// Blink period for the death-screen "PRESS FIRE" prompt, counted in main-loop
// iterations (not vblanks): ~24/32 of the cycle lit. At the target 2-vblank
// cadence this is close to a 1-second blink; exact timing is not gameplay-
// critical.
#define DEATH_PROMPT_BLINK_MASK 0x1F
#define DEATH_PROMPT_BLINK_ON_FRAMES 24
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
#ifndef DEBUG_START_LEVEL
#define DEBUG_START_LEVEL 0
#endif
#ifndef DEBUG_START_E1M1_EXIT
#define DEBUG_START_E1M1_EXIT 0
#endif

static PlayerState g_player;
static RayColumn g_ray_columns[RAY_VIEW_COLS];
static RaySceneColors g_scene_colors;
static u16 g_weapon_flash = 0;
static u16 g_player_damage_flash = 0;
static u16 g_player_invuln = 0;
static RendererHudState g_hud;
static AutomapState g_automap;
#if DEBUG_BLASTEM_CHECKPOINT
static s32 g_checkpoint_prev_x;
static s32 g_checkpoint_prev_y;
#endif

#if DEBUG_START_E1M1_EXIT
static void debug_place_e1m1_exit(void) {
    g_player.x = 3200;
    g_player.y = 4768;
    g_player.angle = ANGLE_STEPS / 2;
}
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
                     const PlayerArsenal *arsenal,
                     u8 player_keys,
                     u16 shot_cooldown,
                     DoorActionResult action_status,
                     BillboardShotResult shot_status,
                     bool level_cleared) {
    // The status bar shows the CURRENT weapon's pool. The melee weapons have no
    // pool, and Doom leaves the ammo digits blank for them rather than showing
    // a zero -- renderer_hud.c clears the field when ammo_visible is FALSE.
    const u8 ammo_type = WEAPON_DEFS[arsenal->current].ammo_type;
    g_hud.frame = frame;
    g_hud.phase = (u16)((phase_index % 99) + 1);
    g_hud.player_health = player_health;
    g_hud.health_percent = (u16)((player_health * 100u) / PLAYER_MAX_HEALTH);
    g_hud.armor = player_armor;
    g_hud.ammo = (ammo_type == AMMO_NONE) ? 0 : arsenal->ammo[ammo_type];
    g_hud.ammo_visible = (bool)(ammo_type != AMMO_NONE);
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

static void render_current_view(u16 player_health, bool base_dirty, bool player_dead) {
#if PERF_FIXED_POSE
    // Pose-locked perf harness (see debug_checkpoint.h). Pin the camera before
    // the cast reads it, drop the two pose-keyed caches so a static scene still
    // costs what a motion frame costs, and force the rebuild path. Every
    // iteration then rasterizes an identical scene, which is what makes an A/B
    // between two builds mean anything: the routes themselves cannot hold pose,
    // because the loop is vblank-paced, so the faster build gets more
    // iterations per route frame and walks somewhere else.
    g_player.x = PERF_POSE_X;
    g_player.y = PERF_POSE_Y;
    g_player.angle = PERF_POSE_ANGLE;
    bsp_invalidate_node_cache();
    pack_stage_invalidate_coherence();
    billboard_projection_invalidate_cache();
    // base_dirty is NOT forced here: redraw policy has exactly one owner
    // (renderer_redraw_request_base), and tools/test-active-battle-perf.py
    // enforces that main.c never sets the flag directly. The harness asks for
    // the rebuild through that owner, in the main loop below.
#endif
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
        g_weapon_flash > 0, (bool)((g_player_damage_flash > 0) || player_dead),
        (bool)(player_health <= 20));
}

static void add_ammo(PlayerArsenal *arsenal, u8 ammo_type, u16 amount) {
    if ((ammo_type == AMMO_NONE) || (ammo_type >= AMMO_TYPE_COUNT)) return;
    const u16 total = (u16)(arsenal->ammo[ammo_type] + amount);
    arsenal->ammo[ammo_type] =
        (total > AMMO_MAX[ammo_type]) ? AMMO_MAX[ammo_type] : total;
}

static void level_progress_reset(LevelProgress *progress) {
    progress->time_vblanks = 0;
    progress->secrets_found = 0;
    for (u16 i = 0; i < LEVEL_SECRET_BYTES; i++) {
        progress->visited_secret_bits[i] = 0;
    }
}

static void level_progress_visit(LevelProgress *progress, s32 x, s32 y) {
    const u16 subsector = bsp_find_subsector(x, y);
    if (subsector >= bsp_subsector_count) return;
    const u16 sector = bsp_subsector_sector[subsector];
    if (!bsp_sector_is_secret(sector)) return;
    const u8 mask = (u8)(1u << (sector & 7));
    u8 *entry = &progress->visited_secret_bits[sector >> 3];
    if ((*entry & mask) != 0) return;
    *entry = (u8)(*entry | mask);
    progress->secrets_found++;
}

// One trigger pull: `pellets` independent hitscans fanned across `spread_cols`
// view columns, each blocked by the wall depth at ITS OWN column so an outer
// shotgun pellet cannot punch through a corner the centre pellet clears. Their
// results merge into one outcome for the HUD and the reaction sound: the most
// significant status wins (kill over damage over none), explosion counts and
// splash damage sum, since one blast can set off several barrels.
static BillboardFireResult fire_weapon(const WeaponDef *weapon, const RayColumn *columns) {
    BillboardFireResult merged = {BILLBOARD_SHOT_NONE, 0, 0, 0, 0};
    const u8 pellets = (weapon->pellets > 0) ? weapon->pellets : 1;

    for (u8 i = 0; i < pellets; i++) {
        s16 aim_col = RAY_VIEW_CENTER_X;
        if ((pellets > 1) && (weapon->spread_cols > 0)) {
            // Fan the pellets evenly across [-spread, +spread].
            aim_col = (s16)(RAY_VIEW_CENTER_X +
                (((s16)(2 * i) - (s16)(pellets - 1)) * (s16)weapon->spread_cols) /
                (s16)(pellets - 1));
        }
        if (aim_col < 0) aim_col = 0;
        if (aim_col >= RAY_VIEW_COLS) aim_col = (s16)(RAY_VIEW_COLS - 1);

        u16 depth = columns[aim_col].depth;
        // Melee weapons reach only a fixed distance, never all the way to the
        // wall; billboard_fire_center treats this as the pellet's stop depth.
        if ((weapon->melee_range > 0) && (depth > weapon->melee_range)) {
            depth = weapon->melee_range;
        }

        const BillboardFireResult hit = billboard_fire_center(
            &g_player, depth, aim_col, weapon_roll_damage());
        if (hit.status > merged.status) {
            merged.status = hit.status;
        }
        merged.player_damage = (u16)(merged.player_damage + hit.player_damage);
        merged.explosion_count = (u8)(merged.explosion_count + hit.explosion_count);
        merged.push_x = (s16)(merged.push_x + hit.push_x);
        merged.push_y = (s16)(merged.push_y + hit.push_y);
    }
    return merged;
}

static void enter_level(u16 phase_index, DoomSkill skill, bool pistol_start,
                        bool *level_cleared, u16 *shot_cooldown,
                        u16 *player_health, u16 *player_armor, PlayerArsenal *arsenal,
                        u8 *player_keys, u32 *frame, LevelProgress *progress) {
    bsp_map_reset(phase_index);
    billboard_init(phase_index, skill);
    player_init(&g_player, phase_index);
#if DEBUG_START_E1M1_EXIT
    // Test-only pose: east of E1M1's certified exit SEG 376, facing its
    // SW1STRTN surface.  This is compiled out of release ROMs.
    if (phase_index == 0) {
        debug_place_e1m1_exit();
    }
#endif
    player_controller_reset();
    automap_reset(&g_automap, &g_player);
    g_weapon_flash = 0;
    g_player_damage_flash = 0;
    g_player_invuln = 0;
    *level_cleared = FALSE;
    *shot_cooldown = 0;
    if (pistol_start) {
        *player_health = PLAYER_MAX_HEALTH;
        *player_armor = 0;
        // New game and rebirth use Doom's pistol start. A normal map exit
        // deliberately skips this block and carries the inventory forward.
        for (u16 i = 0; i < AMMO_TYPE_COUNT; i++) {
            arsenal->ammo[i] = 0;
        }
        arsenal->ammo[AMMO_BULLETS] = WEAPON_START_BULLETS;
        arsenal->owned = WEAPON_START_OWNED;
        arsenal->current = WEAPON_PISTOL;
    }
    weapon_reset_damage_roll();
    *player_keys = BSP_KEY_NONE;
    *frame = 0;
    level_progress_reset(progress);
    level_progress_visit(progress, g_player.x, g_player.y);

    renderer_invalidate_scene();
    // Must follow renderer_draw_static_screen: that path repaints BG_A, and the
    // weapon selection has to be re-applied on top of a fresh screen anyway.
    renderer_draw_static_screen();
    renderer_set_weapon(arsenal->current);
    sync_hud(*frame, phase_index, *player_health, *player_armor, arsenal,
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
        bool demo_exit_pending = FALSE;
        bool player_dead = FALSE;
        u16 death_lockout = 0;
        u16 phase_index = DEBUG_START_LEVEL;
        LevelProgress level_progress;
        u16 player_health = PLAYER_MAX_HEALTH;
        u16 player_armor = 0;
        PlayerArsenal arsenal;
        u8 player_keys = BSP_KEY_NONE;
        u16 shot_cooldown = 0;
        u16 previous_system_joy;
        u32 prev_vtimer;
        DoomSkill skill;

#if DEBUG_START_E1M1_EXIT
        // The exit route exercises gameplay only; bypass the time-varying
        // frontend so its single C pulse always lands after the V-Int input
        // latch is armed.  Release builds retain the normal frontend path.
        skill = DOOM_SKILL_HURT_ME_PLENTY;
#else
        skill = frontend_run();
#endif
        game_audio_stop_music();
        renderer_init();
        renderer_redraw_init(&redraw);
        game_audio_play_music((phase_index == 0) ? test_music : e1m2_music);

        enter_level(phase_index, skill, TRUE, &level_cleared, &shot_cooldown,
                    &player_health, &player_armor, &arsenal, &player_keys, &frame,
                    &level_progress);
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
        u16 gameplay_pressed;
        AutomapInput automap_input;
        bool six_button_pad;
        bool automap_toggled;
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
        system_pressed = (u16)((system_joy & ~previous_system_joy) | latched_pressed);
        previous_system_joy = system_joy;

        cur_vtimer = vtimer;
        elapsed_vblanks = (u16)(cur_vtimer - prev_vtimer);
        prev_vtimer = cur_vtimer;
        if (elapsed_vblanks < 1) elapsed_vblanks = 1;
        elapsed_frames = (elapsed_vblanks > 4) ? 4 : elapsed_vblanks;

        six_button_pad = (bool)(JOY_getJoypadType(JOY_1) == JOY_TYPE_PAD6);
        automap_input = (!player_dead && !level_cleared) ?
            automap_update_input(&g_automap, &g_player, system_joy,
                system_pressed, six_button_pad, elapsed_frames) :
            (AutomapInput){0, 0};
        gameplay_pressed = (u16)(latched_pressed & ~automap_input.consumed_buttons);
        automap_toggled = (bool)((automap_input.flags & AUTOMAP_INPUT_TOGGLED) != 0);
        if (automap_toggled) {
            wait_scene_upload_complete();
            renderer_invalidate_scene();
            renderer_set_automap_active(g_automap.active);
            renderer_redraw_request_base(&redraw, RENDERER_REDRAW_BASE);
        } else if (automap_input.flags & AUTOMAP_INPUT_REDRAW) {
            renderer_redraw_request_base(&redraw, RENDERER_REDRAW_BASE);
        }

        if ((system_pressed & BUTTON_START) != 0 &&
            (automap_input.consumed_buttons & BUTTON_START) == 0) {
            // The pause menu drives its own JOY_update loops; stop the ISR
            // poll so the two never race the same pad read.
            player_controller_set_poll_active(FALSE);
            // The pause panel is a full-screen BG_A image; drop the window plane
            // so the status numbers do not stay painted over its bottom rows.
            renderer_hud_window_suspend();
            const FrontendPauseAction pause_action =
                frontend_run_pause(renderer_get_menu_tile_base());
            if (pause_action == FRONTEND_PAUSE_QUIT_TO_TITLE) {
#if DEBUG_PERF
                SYS_showFrameLoad(FALSE);
#endif
                break;
            }

            renderer_restore_after_menu();
            if (g_automap.active) renderer_set_automap_active(TRUE);
            if (player_dead) {
                // The pause panel borrowed the same PAIR_TILE_BASE region the
                // death prompt lives in and restore_after_menu cleared BG_A;
                // reload and re-show it so the death screen picks up where it
                // left off instead of losing its prompt.
                frontend_load_death_prompt(renderer_get_menu_tile_base());
                frontend_set_death_prompt(renderer_get_menu_tile_base(), TRUE);
            }
            sync_hud(frame, phase_index, player_health, player_armor, &arsenal,
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

        if (!player_dead && !level_cleared) {
            level_progress.time_vblanks += elapsed_vblanks;
        }
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

        // Doom's PST_REBORN: the player is frozen and takes no input while
        // dead. The world keeps rendering (and enemies keep moving, below) so
        // the death screen is not a separate blocking menu -- it is the
        // gameplay view itself, red-locked, with the status bar face and
        // health at zero, exactly like the original.
        if (player_dead) {
            if (death_lockout > 0) {
                death_lockout = (death_lockout > elapsed_vblanks)
                    ? (u16)(death_lockout - elapsed_vblanks) : 0;
            }
            frontend_set_death_prompt(renderer_get_menu_tile_base(),
                (bool)((frame & DEATH_PROMPT_BLINK_MASK) < DEATH_PROMPT_BLINK_ON_FRAMES));
            if ((death_lockout == 0) &&
                ((latched_pressed & (BUTTON_A | BUTTON_B | BUTTON_C)) != 0)) {
                frontend_set_death_prompt(renderer_get_menu_tile_base(), FALSE);
                enter_level(phase_index, skill, TRUE, &level_cleared, &shot_cooldown,
                            &player_health, &player_armor, &arsenal, &player_keys, &frame,
                            &level_progress);
                player_dead = FALSE;
                renderer_redraw_request_base(&redraw, RENDERER_REDRAW_BASE);
            }
        }

        if (!level_cleared && !player_dead) {
#if DEBUG_START_E1M1_EXIT
            // Keep every test pulse on the exact exit target.
            if (phase_index == 0) debug_place_e1m1_exit();
#endif
            const PlayerControlMode control_mode = automap_toggled ?
                PLAYER_CONTROL_MODE_SUPPRESSED :
                (g_automap.active ?
                    (g_automap.follow ? PLAYER_CONTROL_MODE_AUTOMAP_FOLLOW :
                                        PLAYER_CONTROL_MODE_AUTOMAP_PAN) :
                    PLAYER_CONTROL_MODE_GAMEPLAY);
            control = player_controller_update(
                &g_player, elapsed_frames, gameplay_pressed, control_mode);
            if (g_automap.active && g_automap.follow &&
                (control & PLAYER_CONTROL_CHANGED)) {
                g_automap.center_x = g_player.x;
                g_automap.center_y = g_player.y;
            }
            level_progress_visit(&level_progress, g_player.x, g_player.y);
#if DEBUG_BLASTEM_CHECKPOINT
            {
                const s32 dx = g_player.x - g_checkpoint_prev_x;
                const s32 dy = g_player.y - g_checkpoint_prev_y;
                if (dx != 0 || dy != 0) {
                    debug_checkpoint_mark(DEBUG_CHECKPOINT_MOVED);
                }
                g_checkpoint_prev_x = g_player.x;
                g_checkpoint_prev_y = g_player.y;
            }
#endif
        }

        if (!player_dead && ((control & (PLAYER_CONTROL_NEXT_WEAPON |
                                         PLAYER_CONTROL_PREVIOUS_WEAPON)) != 0)) {
            const u8 next = weapon_cycle(
                arsenal.current, arsenal.owned, arsenal.ammo,
                (bool)((control & PLAYER_CONTROL_NEXT_WEAPON) != 0));
            if (next != arsenal.current) {
                arsenal.current = next;
                renderer_set_weapon(next);
                // The new weapon has to be raised before it fires, and its idle
                // pose must replace whatever the old one left on BG_A.
                if (shot_cooldown < WEAPON_RAISE_VBLANKS) {
                    shot_cooldown = WEAPON_RAISE_VBLANKS;
                }
                g_weapon_flash = 0;
                renderer_redraw_request_overlay(&redraw, RENDERER_REDRAW_WEAPON);
            }
        }

        if (!g_automap.active && (control & PLAYER_CONTROL_WEAPON_BOB) != 0) {
            // Bob advanced (or decayed to neutral) without a whole-pixel world
            // step: a weapon-overlay frame re-applies the BG_A scroll, no cast.
            renderer_redraw_request_overlay(&redraw, RENDERER_REDRAW_WEAPON);
        }

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
                    add_ammo(&arsenal, pickup.ammo_type, pickup.amount);
                } else if (pickup.effect == BILLBOARD_EFFECT_WEAPON) {
                    add_ammo(&arsenal, pickup.ammo_type, pickup.amount);
                    arsenal.owned = (u8)(arsenal.owned | WEAPON_OWNED_BIT(pickup.weapon_id));
                    // Doom switches you to a weapon you just picked up when it
                    // outranks what you are holding. Never mid-death.
                    if (!player_dead && (pickup.weapon_id > arsenal.current) &&
                        weapon_has_ammo(pickup.weapon_id, arsenal.ammo)) {
                        arsenal.current = pickup.weapon_id;
                        renderer_set_weapon(arsenal.current);
                        if (shot_cooldown < WEAPON_RAISE_VBLANKS) {
                            shot_cooldown = WEAPON_RAISE_VBLANKS;
                        }
                        g_weapon_flash = 0;
                        renderer_redraw_request_overlay(&redraw, RENDERER_REDRAW_WEAPON);
                    }
                } else if (pickup.effect == BILLBOARD_EFFECT_KEY) {
                    player_keys = (u8)(player_keys | pickup.key_mask);
                    debug_checkpoint_mark(DEBUG_CHECKPOINT_KEY);
                }
                renderer_redraw_request_overlay(&redraw, RENDERER_REDRAW_OTHER);
                game_audio_play_sfx(
                    (pickup.effect == BILLBOARD_EFFECT_WEAPON) ? sfx_weapon_up : sfx_pickup,
                    (pickup.effect == BILLBOARD_EFFECT_WEAPON) ? sizeof(sfx_weapon_up)
                                                               : sizeof(sfx_pickup),
                    SOUND_PCM_CH2);
            }
        }

        if ((control & PLAYER_CONTROL_USE) != 0) {
            const BspUseResult use =
                bsp_use_in_front(g_player.x, g_player.y, g_player.angle, player_keys);
            const DoorActionResult action = use.action;

            action_status = action;

            if (action != DOOR_ACTION_NONE) {
                if (action == DOOR_ACTION_EXIT) {
                    demo_exit_pending = TRUE;
                    debug_checkpoint_mark(DEBUG_CHECKPOINT_EXIT);
                }
                renderer_redraw_request_base(&redraw, RENDERER_REDRAW_BASE);
                // Door / platform move sound on PCM channel 3. A toggle or a
                // key-unlock moves the door; a locked bump stays silent.
                if ((action == DOOR_ACTION_TOGGLED) || (action == DOOR_ACTION_UNLOCKED)) {
                    game_audio_play_sfx(sfx_door, sizeof(sfx_door), SOUND_PCM_CH3);
                }
            }
        }

        if (demo_exit_pending) {
            // The V-int pad poll belongs to gameplay.  The ending uses its
            // own JOY_update edge detector, exactly like title/menu screens.
            player_controller_set_poll_active(FALSE);
            renderer_upload_background_disarm();
            wait_scene_upload_complete();
            FrontendIntermissionStats stats;
            stats.completed_level = phase_index;
            stats.next_level = (u16)(phase_index + 1);
            stats.kills = billboard_get_kill_count();
            stats.kill_total = billboard_get_kill_total();
            stats.items = billboard_get_item_count();
            stats.item_total = billboard_get_item_total();
            stats.secrets = level_progress.secrets_found;
            stats.secret_total = bsp_current_map()->secret_count;
            stats.time_vblanks = level_progress.time_vblanks;
            stats.par_seconds = (phase_index == 0) ? 30 : 75;
            const FrontendIntermissionAction intermission =
                frontend_run_intermission(&stats);
            if ((phase_index == 0) &&
                (intermission == FRONTEND_INTERMISSION_CONTINUE)) {
                phase_index = 1;
                demo_exit_pending = FALSE;
                renderer_init();
                renderer_redraw_init(&redraw);
                game_audio_play_music(e1m2_music);
                enter_level(phase_index, skill, FALSE, &level_cleared,
                            &shot_cooldown, &player_health, &player_armor,
                            &arsenal, &player_keys, &frame, &level_progress);
                JOY_update();
                previous_system_joy = JOY_readJoypad(JOY_1);
                prev_vtimer = vtimer;
                player_controller_set_poll_active(TRUE);
                debug_checkpoint_mark(DEBUG_CHECKPOINT_GAMEPLAY);
                continue;
            }
            break;
        }

        // Semi-automatic weapons fire on the button's rising edge; the chaingun
        // and chainsaw keep firing while it is held. The cooldown gate below is
        // what paces the automatic ones.
        const WeaponDef *weapon = &WEAPON_DEFS[arsenal.current];
        if ((control & (weapon->automatic ? (PLAYER_CONTROL_FIRE | PLAYER_CONTROL_FIRE_HELD)
                                          : PLAYER_CONTROL_FIRE)) != 0) {
            BillboardShotResult shot = BILLBOARD_SHOT_NONE;

            if ((shot_cooldown == 0) && weapon_has_ammo(arsenal.current, arsenal.ammo)) {
                debug_checkpoint_mark(DEBUG_CHECKPOINT_COMBAT);
                fire_result = fire_weapon(weapon, g_ray_columns);
                shot = fire_result.status;
                shot_cooldown = weapon->cooldown_vblanks;
                if (weapon->ammo_type != AMMO_NONE) {
                    arsenal.ammo[weapon->ammo_type] =
                        (u16)(arsenal.ammo[weapon->ammo_type] - weapon->ammo_per_shot);
                }
                g_weapon_flash = weapon->flash_vblanks;
                renderer_draw_weapon_flash();
                renderer_redraw_request_overlay(&redraw, RENDERER_REDRAW_WEAPON);

                // The weapon's own sound on PCM channel 2 (channel 1 is reserved
                // for music PCM). Connected-hit SFX go on channel 3 so the shot
                // and the enemy reaction never cancel each other out.
                game_audio_play_sfx(weapon->sfx, weapon->sfx_len, SOUND_PCM_CH2);
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

        if (!level_cleared && !player_dead) {
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
                        // Doom's death: freeze the player, lock health/face at
                        // zero and hold the red screen; reset_level is deferred
                        // to the player's own FIRE/USE press below.
                        game_audio_play_sfx(sfx_player_death, sizeof(sfx_player_death), SOUND_PCM_CH2);
                        player_health = 0;
                        player_dead = TRUE;
                        death_lockout = DEATH_INPUT_LOCKOUT_VBLANKS;
                        debug_checkpoint_mark(DEBUG_CHECKPOINT_DEATH);
                        player_controller_reset();
                        frontend_load_death_prompt(renderer_get_menu_tile_base());
                        renderer_redraw_request_overlay(&redraw, RENDERER_REDRAW_DAMAGE);
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
            // player_controller_update (and so the tic count it tracks) only
            // runs while the player is alive -- while dead there is no fresh
            // player-clock progress to report, so charging 0 here (rather than
            // replaying the stale count from the instant of death on every
            // iteration of the death lockout) is what keeps AI cadence frozen
            // instead of running at a rate detached from real time.
            const u16 enemy_tics = player_dead ? 0 : player_controller_tics_last_update();
            const BillboardEnemyUpdate enemy_update = billboard_update_enemies(
                &g_player, renderer_redraw_is_pending(&redraw), enemy_tics);

            if (enemy_update.moved) {
                if (enemy_update.position_changed) {
                    renderer_redraw_request_overlay(&redraw, RENDERER_REDRAW_ENEMY_MOVE);
                }
                if (enemy_update.pose_changed) {
                    renderer_redraw_request_overlay(&redraw, RENDERER_REDRAW_ENEMY_POSE);
                }
            }

            if ((enemy_update.hits > 0) && (g_player_invuln == 0) && !player_dead) {
                const u16 armor_absorb = (u16)(((PLAYER_HIT_DAMAGE / 3) < player_armor) ? (PLAYER_HIT_DAMAGE / 3) : player_armor);
                const u16 damage = (u16)(PLAYER_HIT_DAMAGE - armor_absorb);
                player_armor = (u16)(player_armor - armor_absorb);
                if (player_health <= damage) {
                    // See the barrel-explosion death branch above: freeze and
                    // hold red, defer reset_level to the player's own press.
                    game_audio_play_sfx(sfx_player_death, sizeof(sfx_player_death), SOUND_PCM_CH2);
                    player_health = 0;
                    player_dead = TRUE;
                    death_lockout = DEATH_INPUT_LOCKOUT_VBLANKS;
                    debug_checkpoint_mark(DEBUG_CHECKPOINT_DEATH);
                    player_controller_reset();
                    frontend_load_death_prompt(renderer_get_menu_tile_base());
                    renderer_redraw_request_overlay(&redraw, RENDERER_REDRAW_DAMAGE);
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

        if (player_dead && g_automap.active) {
            wait_scene_upload_complete();
            automap_close(&g_automap);
            renderer_invalidate_scene();
            renderer_set_automap_active(FALSE);
            renderer_redraw_request_base(&redraw, RENDERER_REDRAW_BASE);
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
        sync_hud(frame, phase_index, player_health, player_armor, &arsenal,
                 player_keys, shot_cooldown, action_status, shot_status, level_cleared);
        renderer_draw_hud(&g_hud);

#if DEBUG_PERF
        // Once per iteration (SYS_getFPS counts calls/sec). The old
        // VDP_showFPS/VDP_showCPULoad drew straight onto BG_A, which is
        // whole-plane scrolled for weapon bob, so both fields swung with the
        // gun. They are now sampled here and rendered by the perf overlay on
        // the unscrolled BG_B band above the viewport.
        renderer_perf_overlay_sample_host(frame);
#endif

#if PERF_FIXED_POSE
        // The harness pins the camera, so nothing ever marks the base dirty and
        // the render would be skipped entirely (494 of 495 iterations idled at 2
        // vblanks on the first attempt). Request it unconditionally: the point is
        // to time a rebuild frame, repeatedly, on one unchanging scene.
        renderer_redraw_request_base(&redraw, RENDERER_REDRAW_OTHER);
#endif
        if (renderer_redraw_is_pending(&redraw)) {
#if DEBUG_PERF
            renderer_debug_set_redraw_reasons(renderer_redraw_reasons(&redraw));
#endif
            if (g_automap.active) {
                wait_scene_upload_complete();
                renderer_render_automap(&g_player, &g_automap);
            } else {
                render_current_view(player_health,
                    renderer_redraw_base_is_dirty(&redraw), player_dead);
            }
            renderer_redraw_consume(&redraw);
            if (g_automap.active) renderer_queue_full_view_upload();
            else renderer_queue_scene_upload(g_ray_columns, &g_scene_colors);
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
                u32 pack_columns;
                u32 pack_flat_tiles;
                u32 pack_mixed_tiles;
                u32 bb_max_bytes;
                u32 bb_max_subticks;
                u32 pack_desc_subticks;
                u32 pack_tiles_subticks;
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
            s_cadence.pack_columns = g_cadence_pack_columns;
            s_cadence.pack_flat_tiles = g_cadence_pack_flat_tiles;
            s_cadence.pack_mixed_tiles = g_cadence_pack_mixed_tiles;
            s_cadence.bb_max_bytes = g_cadence_bb_max_bytes;
            s_cadence.bb_max_subticks = g_cadence_bb_max_subticks;
            s_cadence.pack_desc_subticks = g_cadence_pack_desc_subticks;
            s_cadence.pack_tiles_subticks = g_cadence_pack_tiles_subticks;
            debug_checkpoint_publish_perf(&s_cadence, sizeof(s_cadence));
        }
#endif
        frame++;
        }
    }

    return 0;
}
