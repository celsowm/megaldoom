#ifndef MEGALDOOM_RENDERER_H
#define MEGALDOOM_RENDERER_H

#include <genesis.h>
#include "billboard.h"
#include "raycast.h"
#include "world_map.h"

typedef struct {
    u32 frame;
    u16 phase;
    u16 player_health;
    u16 shot_cooldown;
    u16 enemy_count;
    u16 target_count;
    u16 target_health;
    BillboardPickupCounts pickups;
    BillboardPickupKind last_pickup;
    DoorActionResult action_status;
    BillboardShotResult shot_status;
    u8 portrait_state;
    bool level_cleared;
} RendererHudState;

void renderer_init(void);
void renderer_draw_static_screen(void);
void renderer_draw_hud(const RendererHudState *state);
void renderer_render_scene(const RayColumn *columns,
                           const PlayerState *player,
                           bool weapon_flash,
                           bool damage_flash,
                           bool low_health_warning);

#endif
