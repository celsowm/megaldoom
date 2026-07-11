#include "raycast.h"
#include "fixed_math.h"
#include "bsp_map.h"

// Player physics. The world geometry and collision now live in the BSP map
// (bsp_map.c); the old grid DDA raycaster is gone — walls are drawn by
// bsp_cast_frame() in bsp_render.c.

#define PLAYER_RADIUS 32

static bool is_blocked_at(s32 x, s32 y) {
#if DEBUG_PERF
    bsp_debug_set_query_owner(BSP_QUERY_PLAYER);
#endif
    return bsp_circle_blocked(x, y, PLAYER_RADIUS);
}

void player_init(PlayerState *player, u16 phase_index) {
    // Spawn comes from the active map's player-1 start (THINGS lump for E1M1).
    (void)phase_index;
    player->x = bsp_player_start_x;
    player->y = bsp_player_start_y;
    player->angle = bsp_player_start_angle;
}

#define PLAYER_MOVE_SUBSTEP 32

void player_try_move(PlayerState *player, s16 forward, s16 strafe) {
    const s16 dir_x = fx_cos(player->angle);
    const s16 dir_y = fx_sin(player->angle);
    const s16 side_x = (s16)-dir_y;
    const s16 side_y = dir_x;
    const s32 dx = (((s32)dir_x * forward) + ((s32)side_x * strafe)) >> FX_SHIFT;
    const s32 dy = (((s32)dir_y * forward) + ((s32)side_y * strafe)) >> FX_SHIFT;
    const s32 abs_x = (dx < 0) ? -dx : dx;
    const s32 abs_y = (dy < 0) ? -dy : dy;
    const s32 span = (abs_x > abs_y) ? abs_x : abs_y;
    // Split a large displacement into sub-steps so collision is sampled along the path
    // instead of only at the endpoint (prevents tunnelling through walls at low fps).
    s16 steps = (s16)((span + PLAYER_MOVE_SUBSTEP - 1) / PLAYER_MOVE_SUBSTEP);
    s32 step_base_x;
    s32 step_base_y;
    s32 step_rem_x;
    s32 step_rem_y;
    s32 error_x = 0;
    s32 error_y = 0;
    s16 i;

    if (steps < 1) {
        steps = 1;
    }
    step_base_x = dx / steps;
    step_base_y = dy / steps;
    step_rem_x = dx % steps;
    step_rem_y = dy % steps;

    for (i = 1; i <= steps; i++) {
        s32 step_x = step_base_x;
        s32 step_y = step_base_y;
        error_x += step_rem_x;
        error_y += step_rem_y;
        if (error_x >= steps) { step_x++; error_x -= steps; }
        else if (error_x <= -steps) { step_x--; error_x += steps; }
        if (error_y >= steps) { step_y++; error_y -= steps; }
        else if (error_y <= -steps) { step_y--; error_y += steps; }
        const s32 next_x = player->x + step_x;
        const s32 next_y = player->y + step_y;

        // One collision query handles the common unobstructed case. Only a
        // blocked diagonal pays for axis-separated retries to preserve sliding.
        if (!is_blocked_at(next_x, next_y)) {
            player->x = next_x;
            player->y = next_y;
        } else {
            if (!is_blocked_at(next_x, player->y)) player->x = next_x;
            if (!is_blocked_at(player->x, next_y)) player->y = next_y;
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
