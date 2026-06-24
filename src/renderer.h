#ifndef MEGALDOOM_RENDERER_H
#define MEGALDOOM_RENDERER_H

#include <genesis.h>
#include "billboard.h"
#include "raycast.h"
#include "world_map.h"

void renderer_init(void);
void renderer_draw_static_screen(void);
void renderer_render_scene(const RayColumn *columns,
                           const PlayerState *player,
                           bool weapon_flash,
                           bool damage_flash,
                           bool low_health_warning);
void renderer_draw_frame_counter(u32 frame);
void renderer_draw_pickup_counter(BillboardPickupCounts counts);
void renderer_draw_last_pickup(BillboardPickupKind kind);
void renderer_draw_action_status(DoorActionResult action);
void renderer_draw_goal_status(bool level_cleared);
void renderer_draw_shot_status(BillboardShotResult shot);
void renderer_draw_shot_cooldown(u16 frames_left);
void renderer_draw_enemy_count(u16 count);
void renderer_draw_player_health(u16 hp);
void renderer_draw_phase(u16 phase_index);
void renderer_draw_target_count(u16 count);
void renderer_draw_target_health(u16 hp);

#endif
