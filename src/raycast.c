#include "raycast.h"
#include "fixed_math.h"
#include "bsp_map.h"
#include "billboard.h"

// Player physics. The world geometry and collision now live in the BSP map
// (bsp_map.c); the old grid DDA raycaster is gone — walls are drawn by
// bsp_cast_frame() in bsp_render.c.

// ---- Viewport geometry ----------------------------------------------------
// The live size behind the RAY_VIEW_* macros in raycast.h. Everything that
// allocates uses the _MAX forms instead, so these only ever bound loops and
// centre coordinates -- shrinking them frees frame time, never memory.
const RayViewSize g_view_sizes[RAY_VIEW_SIZE_COUNT] = {
    { RAY_VIEW_SIZE_0_W, RAY_VIEW_SIZE_0_H },  // 160x120 -- historical, 90 deg
    { RAY_VIEW_SIZE_1_W, RAY_VIEW_SIZE_1_H },  // 176x120 -- wider field, ~95 deg
    { RAY_VIEW_SIZE_2_W, RAY_VIEW_SIZE_2_H },  // 176x128 -- the _MAX pair
};

// Every preset must fit the buffers and centre exactly, and the default must be
// the historical viewport so an untouched option changes nothing.
_Static_assert(RAY_VIEW_SIZE_0_W <= RAY_VIEW_TILE_W_MAX,
               "view size 0 is wider than RAY_VIEW_TILE_W_MAX");
_Static_assert(RAY_VIEW_SIZE_0_H <= RAY_VIEW_TILE_H_MAX,
               "view size 0 is taller than RAY_VIEW_TILE_H_MAX");
_Static_assert((RAY_VIEW_SIZE_0_W & 1) == 0,
               "view size 0 needs an even tile width to centre exactly");
_Static_assert(RAY_VIEW_SIZE_1_W <= RAY_VIEW_TILE_W_MAX,
               "view size 1 is wider than RAY_VIEW_TILE_W_MAX");
_Static_assert(RAY_VIEW_SIZE_1_H <= RAY_VIEW_TILE_H_MAX,
               "view size 1 is taller than RAY_VIEW_TILE_H_MAX");
_Static_assert((RAY_VIEW_SIZE_1_W & 1) == 0,
               "view size 1 needs an even tile width to centre exactly");
_Static_assert(RAY_VIEW_SIZE_2_W <= RAY_VIEW_TILE_W_MAX,
               "view size 2 is wider than RAY_VIEW_TILE_W_MAX");
_Static_assert(RAY_VIEW_SIZE_2_H <= RAY_VIEW_TILE_H_MAX,
               "view size 2 is taller than RAY_VIEW_TILE_H_MAX");
_Static_assert((RAY_VIEW_SIZE_2_W & 1) == 0,
               "view size 2 needs an even tile width to centre exactly");
_Static_assert(RAY_VIEW_SIZE_DEFAULT == 0,
               "the default preset must be row 0, the historical viewport");

u16 g_view_tile_w = RAY_VIEW_SIZE_0_W;
u16 g_view_tile_h = RAY_VIEW_SIZE_0_H;
u16 g_view_cols = RAY_VIEW_SIZE_0_W * 8;
u16 g_view_rows = RAY_VIEW_SIZE_0_H * 8;
u16 g_view_center_x = (RAY_VIEW_SIZE_0_W * 8) / 2;
u16 g_view_center_y = (RAY_VIEW_SIZE_0_H * 8) / 2;

static u16 s_view_size_index = RAY_VIEW_SIZE_DEFAULT;

u16 raycast_view_size(void) {
    return s_view_size_index;
}

bool raycast_set_view_size(u16 size_index) {
    if (size_index >= RAY_VIEW_SIZE_COUNT) return FALSE;
    const RayViewSize *size = &g_view_sizes[size_index];
    if (size->tile_w == g_view_tile_w && size->tile_h == g_view_tile_h) {
        s_view_size_index = size_index;
        return FALSE;
    }
    s_view_size_index = size_index;
    g_view_tile_w = size->tile_w;
    g_view_tile_h = size->tile_h;
    g_view_cols = (u16)(g_view_tile_w * 8);
    g_view_rows = (u16)(g_view_tile_h * 8);
    g_view_center_x = (u16)(g_view_cols / 2);
    g_view_center_y = (u16)(g_view_rows / 2);
    return TRUE;
}

static bool is_blocked_at(s32 x, s32 y) {
#if DEBUG_PERF
    bsp_debug_set_query_owner(BSP_QUERY_PLAYER);
#endif
    return bsp_circle_blocked(x, y, PLAYER_COLLISION_RADIUS) ||
           billboard_position_blocked(x, y, PLAYER_COLLISION_RADIUS);
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

    // The common free-space case needs one BSP/prop query. Preserve Doom-like
    // wall sliding with the old per-axis retries only after a combined move is
    // actually blocked.
    if (!is_blocked_at(next_x, next_y)) {
        player->x = next_x;
        player->y = next_y;
        return;
    }
    if (!is_blocked_at(next_x, player->y)) player->x = next_x;
    if (!is_blocked_at(player->x, next_y)) player->y = next_y;
}
