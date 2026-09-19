#include <genesis.h>
#include "automap.h"
#include "billboard.h"
#include "bsp_map.h"
#include "bsp_render.h"
#include "debug_checkpoint.h"
#include "debug_light.h"
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

// The weapon flash counts real vblanks (display only). It is set after its
// decrement runs, so the firing iteration always renders it and it survives
// >= 1 displayed frame. When shots happen is WEAPON_DEFS' Doom timeline, run
// on 35 Hz player tics (see WeaponState below).
#define PLAYER_DAMAGE_FLASH_FRAMES 6
#define PLAYER_MAX_HEALTH 100
#define PLAYER_MAX_ARMOR 200
// Doom raises a new weapon before it can fire. Kept at the shipped ~10
// vblanks (6 tics), shorter than Doom's lower-and-raise.
#define WEAPON_RAISE_TICS 6
// Safety cap on fire actions resolved in one main-loop iteration. A 4-tic
// weapon over the 3 tics an iteration can credit never reaches it.
#define MAX_SHOTS_PER_ITERATION 4
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
// Test-only spawn pose for headless screenshots: set DEBUG_START_POSE=1 with
// DEBUG_START_POSE_X/_Y (map units) and DEBUG_START_POSE_ANGLE (256ths), plus
// DEBUG_START_LEVEL for a map other than E1M1. Compiled out of release ROMs.
#ifndef DEBUG_START_POSE
#define DEBUG_START_POSE 0
#endif
// Test-only key bits (BSP_KEY_*) granted at level entry, so a headless
// screenshot can show the status-bar key cards without walking a route.
#ifndef DEBUG_START_KEYS
#define DEBUG_START_KEYS 0
#endif

// One row per campaign level, indexed by phase_index. The length is
// MEGALDOOM_MAP_COUNT, which tools/wad-map-extract.py emits from the map list
// it was actually given, so the campaign has a single source of truth and
// adding a level cannot leave a stale `phase_index == 0` branch behind.
// Par times are Doom's own for E1M1..E1M7.
typedef struct {
    const u8 *music;
    u16 par_seconds;
} CampaignLevel;

static const CampaignLevel CAMPAIGN[MEGALDOOM_MAP_COUNT] = {
    { test_music, 30 },
    { e1m2_music, 75 },
    { e1m3_music, 120 },
    { e1m4_music, 90 },
    { e1m5_music, 165 },
    { e1m6_music, 180 },
    { e1m7_music, 180 },
};

static PlayerState g_player;
static RayColumn g_ray_columns[RAY_SAMPLE_COLS_MAX];
static RaySceneColors g_scene_colors;
static u16 g_weapon_flash = 0;
static u16 g_player_damage_flash = 0;
// Doom's player->armortype: 1 (green, absorbs a third) or 2 (blue, half);
// 0 once the armour is used up.
static u8 g_player_armor_type = 0;

// Where the held weapon is in its Doom state sequence (WeaponDef). `timer`
// counts 35 Hz tics left in `phase`; `refire` is Doom's player->refire.
typedef enum {
    WEAPON_PHASE_READY = 0,
    WEAPON_PHASE_RAISE,
    WEAPON_PHASE_WINDUP,
    WEAPON_PHASE_GAP,
    WEAPON_PHASE_TAIL,
    WEAPON_PHASE_RELEASE
} WeaponPhase;
typedef struct {
    u8 phase;
    u8 timer;
    u8 shots_fired;
    u8 refire;
} WeaponState;
static WeaponState g_weapon_state;

static void weapon_state_raise(void) {
    g_weapon_state.phase = WEAPON_PHASE_RAISE;
    g_weapon_state.timer = WEAPON_RAISE_TICS;
    g_weapon_state.shots_fired = 0;
    g_weapon_state.refire = 0;
}

// Spend `*tics` on the weapon's timeline until it reaches a fire action
// (returns TRUE, with `*accurate` set) or runs out of time (FALSE). `trigger`
// is whether an attack may start from ready (a press, or the button held);
// `held` is the button state A_ReFire sees. An attack only starts, or
// refires, with ammo for it (P_CheckAmmo).
static bool weapon_state_step(const WeaponDef *weapon, u16 *tics, bool *trigger,
                              bool held, bool has_ammo, bool *accurate) {
    WeaponState *st = &g_weapon_state;
    for (;;) {
        if (st->phase == WEAPON_PHASE_READY) {
            if (!*trigger || !has_ammo) {
                return FALSE;
            }
            *trigger = held;  // a tap starts one attack, not one per step
            // The button is sampled after this iteration's tics ran, so the
            // attack starts on the last of them; as in Doom, the tic that
            // sees the press does not count toward the windup.
            *tics = 0;
            st->phase = WEAPON_PHASE_WINDUP;
            st->timer = weapon->windup_tics;
            st->shots_fired = 0;
            continue;
        }
        if (st->timer > *tics) {
            st->timer = (u8)(st->timer - *tics);
            *tics = 0;
            return FALSE;
        }
        *tics = (u16)(*tics - st->timer);
        st->timer = 0;
        switch (st->phase) {
        case WEAPON_PHASE_WINDUP:
        case WEAPON_PHASE_GAP:
            *accurate = (bool)(weapon->accurate_first && (st->refire == 0));
            st->shots_fired++;
            if (st->shots_fired < weapon->shots) {
                st->phase = WEAPON_PHASE_GAP;
                st->timer = weapon->shot_gap_tics;
            } else {
                st->phase = WEAPON_PHASE_TAIL;
                st->timer = weapon->tail_tics;
            }
            return TRUE;
        case WEAPON_PHASE_TAIL:
            // A_ReFire.
            if (held && has_ammo) {
                if (st->refire < 0xFF) {
                    st->refire++;
                }
                st->phase = WEAPON_PHASE_WINDUP;
                st->timer = weapon->windup_tics;
                st->shots_fired = 0;
            } else {
                st->refire = 0;
                st->phase = WEAPON_PHASE_RELEASE;
                st->timer = weapon->release_tics;
            }
            break;
        default:  // RAISE, RELEASE
            st->phase = WEAPON_PHASE_READY;
            break;
        }
    }
}
static RendererHudState g_hud;
static AutomapState g_automap;
#if DEBUG_BLASTEM_CHECKPOINT
static s32 g_checkpoint_prev_x;
static s32 g_checkpoint_prev_y;
#endif

#if DEBUG_START_E1M1_EXIT
static void debug_place_e1m1_exit(void) {
    // 48 units in front of the exit SEG 376 (x=2912, facing east), aimed at its
    // centre. The pose used to be (3200, 4768), which is on the far side of
    // the solid wall at x=3104: it only ever reached the switch because use
    // ignored walls (fixed 2026-09-11, see bsp_map.c use_surface_visible).
    g_player.x = 2960;
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

// Fold one trace's outcome into a trigger pull's (or a frame's) result: the
// most significant status wins (kill over damage over none), explosion counts
// and splash damage sum, since one blast can set off several barrels.
static void merge_fire_result(BillboardFireResult *merged, const BillboardFireResult *hit) {
    if (hit->status > merged->status) {
        merged->status = hit->status;
    }
    merged->player_damage = (u16)(merged->player_damage + hit->player_damage);
    merged->explosion_count = (u8)(merged->explosion_count + hit->explosion_count);
    merged->thrust_x += hit->thrust_x;
    merged->thrust_y += hit->thrust_y;
    merged->pain = (bool)(merged->pain || hit->pain);
    if (hit->hit_target && !merged->hit_target) {
        merged->hit_target = TRUE;
        merged->target_x = hit->target_x;
        merged->target_y = hit->target_y;
    }
}

// One trigger pull, Doom's weapon action functions: each of `pellets` traces
// rolls its damage and then its aim offset from the P_Random table
// (P_GunShot's order), and is blocked by the wall depth at the view column its
// offset points down, so an outer shotgun pellet cannot punch through a corner
// the centre one clears. `accurate` is Doom's refire == 0 for the pistol and
// chaingun; the shotgun and the melee weapons always roll a spread.
static BillboardFireResult fire_weapon(const WeaponDef *weapon, const RayColumn *columns,
                                       bool accurate) {
    BillboardFireResult merged = {.status = BILLBOARD_SHOT_NONE};
    const u8 pellets = (weapon->pellets > 0) ? weapon->pellets : 1;

    for (u8 i = 0; i < pellets; i++) {
        const u16 damage = weapon_roll_damage(weapon);
        const s16 spread_q12 = weapon_roll_spread_q12(accurate);
        s16 aim_col = (s16)(RAY_VIEW_CENTER_X + (((s32)spread_q12 * RAY_PROJ_X) / 4096));
        if (aim_col < 0) aim_col = 0;
        if (aim_col >= RAY_VIEW_COLS) aim_col = (s16)(RAY_VIEW_COLS - 1);

        const u16 depth = columns[RAY_SAMPLE_OF(aim_col)].depth;
        const BillboardFireResult hit = billboard_fire_hitscan(
            &g_player, spread_q12, depth, weapon->melee_range, damage,
            (bool)(weapon != &WEAPON_DEFS[WEAPON_CHAINSAW]));
        merge_fire_result(&merged, &hit);
    }
    return merged;
}

// A_Punch turns the player to face what it hit. A_Saw pulls toward it instead:
// a target more than ANG90/20 away snaps to ANG90/21 short of it, a nearer one
// is overshot by ANG90/20 -- the chainsaw's familiar shake. Both are 3 of this
// engine's 256 angle steps (64/20 and 64/21 round to 3).
#define SAW_TURN_STEP 3
static bool turn_to_melee_target(u8 weapon_id, const BillboardFireResult *hit) {
    if (!hit->hit_target) {
        return FALSE;
    }
    const u16 target = billboard_angle_to(&g_player, hit->target_x, hit->target_y);
    u16 angle = target;
    if (weapon_id == WEAPON_CHAINSAW) {
        const s16 diff = (s16)(s8)(u8)(target - g_player.angle);
        if (diff < -SAW_TURN_STEP) {
            angle = (u16)(target + SAW_TURN_STEP);
        } else if (diff < 0) {
            angle = (u16)(g_player.angle - SAW_TURN_STEP);
        } else if (diff > SAW_TURN_STEP) {
            angle = (u16)(target - SAW_TURN_STEP);
        } else {
            angle = (u16)(g_player.angle + SAW_TURN_STEP);
        }
    }
    angle &= ANGLE_MASK;
    if (angle == g_player.angle) {
        return FALSE;
    }
    g_player.angle = angle;
    return TRUE;
}

/* The two damage producers (enemy AI and barrel splash) intentionally share
 * this path, which is Doom's P_DamageMobj for the player: skill "I'm too young
 * to die" halves the damage, the knockback thrust (computed by the producer
 * from the full damage) goes onto the player's momentum and so through the
 * same collision as walking, then armour absorbs a third (green) or half
 * (blue) until it runs out. Doom has no invulnerability window after a hit.
 * DEBUG_E2E_GOD only changes the player consequence; the attack, explosion,
 * enemy update and collision that produced it have already run. */
static void apply_player_damage(u16 total_damage, s32 thrust_x, s32 thrust_y,
                                DoomSkill skill,
                                u16 *player_health, u16 *player_armor,
                                bool *player_dead, u16 *death_lockout,
                                RendererRedrawState *redraw) {
#if DEBUG_E2E_GOD
    (void)total_damage;
    (void)thrust_x;
    (void)thrust_y;
    (void)skill;
    (void)player_health;
    (void)player_armor;
    (void)player_dead;
    (void)death_lockout;
    (void)redraw;
    debug_e2e_god_hit();
#else
    u16 damage = total_damage;
    if (skill == DOOM_SKILL_IM_TOO_YOUNG_TO_DIE) {
        damage >>= 1;
        thrust_x /= 2;
        thrust_y /= 2;
    }
    if (damage == 0) {
        return;
    }
    if (g_player_armor_type != 0) {
        u16 saved = (g_player_armor_type == 1) ? (u16)(damage / 3) : (u16)(damage / 2);
        if (*player_armor <= saved) {
            saved = *player_armor;
            g_player_armor_type = 0;
        }
        *player_armor = (u16)(*player_armor - saved);
        damage = (u16)(damage - saved);
    }
    if (*player_health <= damage) {
        game_audio_play_sfx(sfx_player_death, sizeof(sfx_player_death), SOUND_PCM_CH2);
        *player_health = 0;
        *player_dead = TRUE;
        *death_lockout = DEATH_INPUT_LOCKOUT_VBLANKS;
        debug_checkpoint_mark(DEBUG_CHECKPOINT_DEATH);
        debug_e2e_death();
        player_controller_reset();
        frontend_load_death_prompt(renderer_get_menu_tile_base());
        renderer_redraw_request_overlay(redraw, RENDERER_REDRAW_DAMAGE);
    } else {
        debug_light_note_knockback(thrust_x >> 16, thrust_y >> 16);
        player_controller_add_thrust(thrust_x, thrust_y);
        *player_health = (u16)(*player_health - damage);
        g_player_damage_flash = PLAYER_DAMAGE_FLASH_FRAMES;
        renderer_redraw_request_overlay(redraw, RENDERER_REDRAW_DAMAGE);
        game_audio_play_sfx(sfx_player_pain, sizeof(sfx_player_pain), SOUND_PCM_CH2);
    }
#endif
}

static void enter_level(u16 phase_index, DoomSkill skill, bool pistol_start,
                        bool *level_cleared, u16 *shot_cooldown,
                        u16 *player_health, u16 *player_armor, PlayerArsenal *arsenal,
                        u8 *player_keys, u32 *frame, LevelProgress *progress) {
    bsp_map_reset(phase_index);
    billboard_init(phase_index, skill);
    player_init(&g_player, phase_index);
    debug_e2e_level_start(phase_index);
#if DEBUG_START_E1M1_EXIT
    // Test-only pose: east of E1M1's certified exit SEG 376, facing its
    // SW1STRTN surface.  This is compiled out of release ROMs.
    if (phase_index == 0) {
        debug_place_e1m1_exit();
    }
#endif
#if DEBUG_START_POSE
    g_player.x = DEBUG_START_POSE_X;
    g_player.y = DEBUG_START_POSE_Y;
    g_player.angle = DEBUG_START_POSE_ANGLE;
#endif
    /* Publish the genuine spawn pose before the first host-controlled frame;
     * otherwise a pose-driven runner would steer from the mailbox's zero
     * placeholder while the first gameplay iteration is still initializing. */
    debug_e2e_pose(g_player.x, g_player.y, g_player.angle);
    player_controller_reset();
    /* Resetting controller state must not leave the host with a pre-reset
     * angle; publish once more at the exact hand-off to gameplay. */
    debug_e2e_pose(g_player.x, g_player.y, g_player.angle);
    automap_reset(&g_automap, &g_player);
    debug_light_level_start(&g_player);
    g_weapon_flash = 0;
    g_player_damage_flash = 0;
    *level_cleared = FALSE;
    *shot_cooldown = 0;
    g_weapon_state = (WeaponState){0};
    if (pistol_start) {
        *player_health = PLAYER_MAX_HEALTH;
        *player_armor = 0;
        g_player_armor_type = 0;
        // New game and rebirth use Doom's pistol start. A normal map exit
        // deliberately skips this block and carries the inventory forward.
        for (u16 i = 0; i < AMMO_TYPE_COUNT; i++) {
            arsenal->ammo[i] = 0;
        }
        arsenal->ammo[AMMO_BULLETS] = WEAPON_START_BULLETS;
        arsenal->owned = WEAPON_START_OWNED;
        arsenal->current = WEAPON_PISTOL;
    }
    doom_random_reset();
    *player_keys = BSP_KEY_NONE;
#if DEBUG_START_KEYS
    *player_keys = (u8)DEBUG_START_KEYS;
#endif
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

// Boot viewport size. Normally RAY_VIEW_SIZE_DEFAULT, i.e. the historical
// 20x15; overridable so a comparison or capture build can start at a size the
// deterministic routes cannot reach, because they replay a pad and never open
// the OPTIONS menu (EXTRA_FLAGS="-DMEGALDOOM_VIEW_SIZE_BOOT=2").
#ifndef MEGALDOOM_VIEW_SIZE_BOOT
#define MEGALDOOM_VIEW_SIZE_BOOT RAY_VIEW_SIZE_DEFAULT
#endif
#if (MEGALDOOM_VIEW_SIZE_BOOT < 0) || (MEGALDOOM_VIEW_SIZE_BOOT >= RAY_VIEW_SIZE_COUNT)
#error "MEGALDOOM_VIEW_SIZE_BOOT is not one of the RAY_VIEW_SIZE_* presets"
#endif

int main(bool hard) {
    (void)hard;

    // Once, before the frontend: renderer_init() runs again on every level
    // transition and must adopt whatever the player selected, not reset it.
    raycast_set_view_size(MEGALDOOM_VIEW_SIZE_BOOT);
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
        u16 phase_index = DEBUG_E2E_ACTIVE ? (u16)DEBUG_E2E_START_LEVEL : DEBUG_START_LEVEL;
        LevelProgress level_progress;
        u16 player_health = PLAYER_MAX_HEALTH;
        u16 player_armor = 0;
        PlayerArsenal arsenal;
        u8 player_keys = BSP_KEY_NONE;
        u16 shot_cooldown = 0;
        u16 previous_system_joy;
        u32 prev_vtimer;
        DoomSkill skill;

#if DEBUG_START_E1M1_EXIT || DEBUG_E2E_ACTIVE || DEBUG_START_POSE
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
        game_audio_play_music(CAMPAIGN[phase_index].music);

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
        BillboardFireResult fire_result = {.status = BILLBOARD_SHOT_NONE};
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
            // restore_after_menu cleared BG_B's letterbox, and the menu may
            // have switched the debug overlay on or off.
            debug_light_invalidate();
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
            if (debug_light_update_rescue(&g_player, system_joy, elapsed_vblanks)) {
                // Back inside: drop the momentum that carried the player out.
                player_controller_reset();
                renderer_redraw_request_base(&redraw, RENDERER_REDRAW_BASE);
            }
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
            debug_e2e_pose(g_player.x, g_player.y, g_player.angle);
#if DEBUG_BLASTEM_CHECKPOINT
            {
                const s32 dx = g_player.x - g_checkpoint_prev_x;
                const s32 dy = g_player.y - g_checkpoint_prev_y;
                if (dx != 0 || dy != 0) {
                    debug_checkpoint_mark(DEBUG_CHECKPOINT_MOVED);
                    debug_e2e_mark(DEBUG_E2E_EVENT_MOVED);
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
                weapon_state_raise();
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
                    // Doom: a bonus adds a point and gives green class if none;
                    // green (100) and blue (200) set both, unless the player
                    // already has at least that many points (P_GiveArmor).
                    if (pickup.amount == 1) {
                        player_armor++;
                        if (g_player_armor_type == 0) g_player_armor_type = 1;
                    } else if (player_armor < pickup.amount) {
                        player_armor = pickup.amount;
                        g_player_armor_type = (u8)(pickup.amount / 100);
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
                        weapon_state_raise();
                        g_weapon_flash = 0;
                        renderer_redraw_request_overlay(&redraw, RENDERER_REDRAW_WEAPON);
                    }
                } else if (pickup.effect == BILLBOARD_EFFECT_KEY) {
                    player_keys = (u8)(player_keys | pickup.key_mask);
                    debug_checkpoint_mark(DEBUG_CHECKPOINT_KEY);
                    debug_e2e_collect_key(pickup.key_mask);
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

            debug_e2e_use((u8)action, use.target, use.required_key);

            action_status = action;

            if (action != DOOR_ACTION_NONE) {
                debug_e2e_mark(DEBUG_E2E_EVENT_INTERACTION);
                if (action == DOOR_ACTION_LOCKED) {
                    debug_e2e_locked(use.required_key);
                } else if (action == DOOR_ACTION_UNLOCKED) {
                    debug_e2e_unlocked(use.required_key);
                }
                if (action == DOOR_ACTION_EXIT) {
                    demo_exit_pending = TRUE;
                    debug_checkpoint_mark(DEBUG_CHECKPOINT_EXIT);
                    debug_e2e_exit(phase_index);
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
            stats.par_seconds = CAMPAIGN[phase_index].par_seconds;
            const FrontendIntermissionAction intermission =
                frontend_run_intermission(&stats);
            if ((phase_index + 1 < MEGALDOOM_MAP_COUNT) &&
                (intermission == FRONTEND_INTERMISSION_CONTINUE)) {
                phase_index++;
                demo_exit_pending = FALSE;
                renderer_init();
                renderer_redraw_init(&redraw);
                game_audio_play_music(CAMPAIGN[phase_index].music);
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

        // The held weapon's Doom timeline advances on the player's own tics;
        // each fire action it reaches is one trigger pull (fire_weapon). A
        // press waits the weapon's windup (the pistol's 4 tics) before the
        // shot leaves, holding the button refires through A_ReFire, and a tap
        // the ISR latched between iterations starts one attack.
        const WeaponDef *weapon = &WEAPON_DEFS[arsenal.current];
        {
            u16 weapon_tics = player_dead ? 0 : player_controller_tics_last_update();
            bool trigger = (bool)((control & (PLAYER_CONTROL_FIRE |
                                              PLAYER_CONTROL_FIRE_HELD)) != 0);
            const bool held = (bool)((control & PLAYER_CONTROL_FIRE_HELD) != 0);
            bool accurate = FALSE;
            u8 shots = 0;

            while ((shots < MAX_SHOTS_PER_ITERATION) &&
                   weapon_state_step(weapon, &weapon_tics, &trigger, held,
                                     weapon_has_ammo(arsenal.current, arsenal.ammo),
                                     &accurate)) {
                // A_FireCGun's own ammo check: the second shot of a pair
                // needs a bullet too.
                if (!weapon_has_ammo(arsenal.current, arsenal.ammo)) {
                    continue;
                }
                debug_checkpoint_mark(DEBUG_CHECKPOINT_COMBAT);
                const BillboardFireResult hit = fire_weapon(weapon, g_ray_columns, accurate);
                merge_fire_result(&fire_result, &hit);
                if ((weapon->melee_range > 0) && turn_to_melee_target(arsenal.current, &hit)) {
                    renderer_redraw_request_base(&redraw, RENDERER_REDRAW_BASE);
                }
                shots++;
                if (weapon->ammo_type != AMMO_NONE) {
                    arsenal.ammo[weapon->ammo_type] =
                        (u16)(arsenal.ammo[weapon->ammo_type] - weapon->ammo_per_shot);
                }
            }
            shot_cooldown = g_weapon_state.timer;

            const BillboardShotResult shot = fire_result.status;
            if (shots > 0) {
                if ((shot == BILLBOARD_SHOT_DAMAGE) ||
                    (shot == BILLBOARD_SHOT_KILL) ||
                    (shot == BILLBOARD_SHOT_EXPLOSION)) {
                    // A directly-shot barrel (SHOT_EXPLOSION) is a connected
                    // hit on a destructible target: the E2E combat_hit contract
                    // is "the weapon reached something it could destroy", which
                    // a detonation satisfies as fully as a kill does. This also
                    // lets an E2E route aim its FIRE waypoint at a barrel -- an
                    // immovable target that monster infighting can never remove
                    // before the follower arrives, which is what stalled E1M2.
                    debug_e2e_mark(DEBUG_E2E_EVENT_COMBAT_HIT);
                }
                g_weapon_flash = weapon->flash_vblanks;
                renderer_draw_weapon_flash();
                renderer_redraw_request_overlay(&redraw, RENDERER_REDRAW_WEAPON);

                // The weapon's own sound on PCM channel 2 (channel 1 is reserved
                // for music PCM). Connected-hit SFX go on channel 3 so the shot
                // and the enemy reaction never cancel each other out.
                game_audio_play_sfx(weapon->sfx, weapon->sfx_len, SOUND_PCM_CH2);
                if (shot == BILLBOARD_SHOT_KILL) {
                    game_audio_play_sfx(sfx_enemy_death, sizeof(sfx_enemy_death), SOUND_PCM_CH3);
                } else if (fire_result.explosion_count > 0) {
                    game_audio_play_sfx(sfx_barexp, sizeof(sfx_barexp), SOUND_PCM_CH3);
                } else if (fire_result.pain) {
                    game_audio_play_sfx(sfx_enemy_pain, sizeof(sfx_enemy_pain), SOUND_PCM_CH3);
                }

                if ((shot == BILLBOARD_SHOT_DAMAGE) || (shot == BILLBOARD_SHOT_KILL) ||
                    (shot == BILLBOARD_SHOT_EXPLOSION)) {
                    renderer_redraw_request_overlay(
                        &redraw, (shot == BILLBOARD_SHOT_EXPLOSION) ?
                            RENDERER_REDRAW_BARREL : RENDERER_REDRAW_ENEMY_POSE);
                }
            }
            if ((control & (PLAYER_CONTROL_FIRE | PLAYER_CONTROL_FIRE_HELD)) != 0) {
                shot_status = shot;
            }
        }

        if (!level_cleared && !player_dead) {
            // Consume the result returned by this exact trigger pull. Keeping
            // damage on the fire result prevents stale HUD shot state or a later
            // explosion from overwriting the player-facing blast outcome.
            if ((fire_result.status == BILLBOARD_SHOT_EXPLOSION) &&
                (fire_result.player_damage > 0)) {
                apply_player_damage(fire_result.player_damage, fire_result.thrust_x,
                                    fire_result.thrust_y, skill, &player_health,
                                    &player_armor, &player_dead, &death_lockout, &redraw);
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

            if ((enemy_update.player_damage > 0) && !player_dead) {
                apply_player_damage(enemy_update.player_damage, enemy_update.thrust_x,
                                    enemy_update.thrust_y, skill, &player_health,
                                    &player_armor, &player_dead, &death_lockout, &redraw);
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
#if !DEBUG_PERF
        // Shares BG_B's top letterbox rows with the DEBUG_PERF overlay, so only
        // one of the two is ever compiled in.
        debug_light_draw(&g_player, phase_index);
#endif

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
                u32 bb_door_subticks;
                u32 bb_door_slots;
                u32 bb_post_subticks;
                u32 bb_post_slots;
                u32 bb_mag_subticks;
                u32 bb_mag_slots;
                u32 wall_rows[CADENCE_WALL_REASON_COUNT];
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
            s_cadence.bb_door_subticks = g_cadence_bb_door_subticks;
            s_cadence.bb_door_slots = g_cadence_bb_door_slots;
            s_cadence.bb_post_subticks = g_cadence_bb_post_subticks;
            s_cadence.bb_post_slots = g_cadence_bb_post_slots;
            s_cadence.bb_mag_subticks = g_cadence_bb_mag_subticks;
            s_cadence.bb_mag_slots = g_cadence_bb_mag_slots;
            for (u16 r = 0; r < CADENCE_WALL_REASON_COUNT; r++) {
                s_cadence.wall_rows[r] = g_cadence_wall_rows[r];
            }
            debug_checkpoint_publish_perf(&s_cadence, sizeof(s_cadence));
        }
#endif
        frame++;
        }
    }

    return 0;
}
