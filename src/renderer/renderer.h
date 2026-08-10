#ifndef MEGALDOOM_RENDERER_H
#define MEGALDOOM_RENDERER_H

#include <genesis.h>
#include "billboard.h"
#include "bsp_map.h"
#include "raycast.h"

typedef struct {
    u32 frame;
    u16 phase;
    u16 player_health;
    u16 health_percent;
    u16 ammo;
    u16 armor;
    u16 shot_cooldown;
    u16 enemy_count;
    u16 target_count;
    u16 target_health;
    BillboardPickupCounts pickups;
    u8 key_mask;
    BillboardPickupKind last_pickup;
    DoorActionResult action_status;
    BillboardShotResult shot_status;
    u8 portrait_state;
    bool level_cleared;
    // FALSE for the melee weapons, which have no ammo pool: Doom leaves the
    // status-bar ammo digits blank for those rather than showing a zero.
    bool ammo_visible;
} RendererHudState;

void renderer_init(void);
void renderer_invalidate_scene(void);
void renderer_draw_static_screen(void);
void renderer_draw_hud(const RendererHudState *state);
u16 renderer_get_menu_tile_base(void);
void renderer_restore_after_menu(void);
void renderer_render_scene(const RayColumn *columns,
                           const PlayerState *player,
                           const RaySceneColors *scene_colors,
                           bool base_dirty,
                           bool weapon_flash,
                           bool damage_flash,
                           bool low_health_warning);
// Draw the fire-pose weapon overlay right now, over the previous displayed
// frame. Called from the fire handler so muzzle feedback does not wait for the
// ~10-vblank cast+upload; the per-variant cache makes the scene-path redraw of
// the same variant a no-op.
void renderer_draw_weapon_flash(void);
// Select the weapon whose overlay is drawn, streaming its tileset into the
// shared VRAM window (see renderer_frame_overlay.c). Must be re-called after
// anything that reclaims that window -- level reset, renderer_restore_after_menu
// -- as well as on every weapon change. weapon_id is a WeaponId (src/weapons.h).
void renderer_set_weapon(u8 weapon_id);
void renderer_queue_scene_upload(const RayColumn *columns,
                                 const RaySceneColors *scene_colors);
void renderer_upload_scene_step(void);
bool renderer_scene_upload_pending(void);

// Background upload pump: install renderer_upload_background_pump as the
// SGDK V-INT callback, then arm it only around code that touches neither the
// VDP nor g_view_tiles (the BSP cast). While armed, vblank interrupts DMA the
// previous frame's queued upload, overlapping it with CPU render work. The
// caller must drain any still-pending upload (pump disarmed, serial
// waitVSync + renderer_upload_scene_step) before anything writes g_view_tiles
// or queues a new upload — the renderer never blocks on vsync itself.
void renderer_upload_background_pump(void);
void renderer_upload_background_arm(void);
void renderer_upload_background_disarm(void);

#endif
