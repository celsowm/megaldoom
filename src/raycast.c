#include "raycast.h"
#include "fixed_math.h"
#include "bsp_map.h"

// Player physics. The world geometry and collision now live in the BSP map
// (bsp_map.c); the old grid DDA raycaster is gone — walls are drawn by
// bsp_cast_frame() in bsp_render.c.

#define PLAYER_RADIUS 32

static bool is_blocked_at(s32 x, s32 y) {
    return bsp_circle_blocked(x, y, PLAYER_RADIUS);
}

void player_init(PlayerState *player, u16 phase_index) {
    // Spawn comes from the active map's player-1 start (THINGS lump for E1M1).
    (void)phase_index;
    player->x = bsp_player_start_x;
    player->y = bsp_player_start_y;
    player->angle = bsp_player_start_angle;
}

#define PLAYER_MOVE_SUBSTEP 48

void player_try_move(PlayerState *player, s16 forward, s16 strafe) {
    const s16 dir_x = fx_cos(player->angle);
    const s16 dir_y = fx_sin(player->angle);
    const s16 side_x = fx_cos((u16)(player->angle + ANGLE_90));
    const s16 side_y = fx_sin((u16)(player->angle + ANGLE_90));
    const s32 dx = (((s32)dir_x * forward) + ((s32)side_x * strafe)) >> FX_SHIFT;
    const s32 dy = (((s32)dir_y * forward) + ((s32)side_y * strafe)) >> FX_SHIFT;
    const s32 abs_x = (dx < 0) ? -dx : dx;
    const s32 abs_y = (dy < 0) ? -dy : dy;
    const s32 span = (abs_x > abs_y) ? abs_x : abs_y;
    // Split a large displacement into sub-steps so collision is sampled along the path
    // instead of only at the endpoint (prevents tunnelling through walls at low fps).
    s16 steps = (s16)((span / PLAYER_MOVE_SUBSTEP) + 1);
    const s32 step_x = dx / steps;
    const s32 step_y = dy / steps;

    while (steps-- > 0) {
        const s32 next_x = player->x + step_x;
        const s32 next_y = player->y + step_y;

        if (!is_blocked_at(next_x, player->y)) {
            player->x = next_x;
        }

        if (!is_blocked_at(player->x, next_y)) {
            player->y = next_y;
        }
    }
}

void player_apply_world_push(PlayerState *player, s32 dx, s32 dy) {
    const s32 next_x = player->x + dx;
    const s32 next_y = player->y + dy;

    if (!is_blocked_at(next_x, player->y)) {
        player->x = next_x;
    }

    if (!is_blocked_at(player->x, next_y)) {
        player->y = next_y;
    }
}
