#include "raycast.h"
#include "fixed_math.h"

#define MAP_W 8
#define MAP_H 8
#define MAP_CELL FX_ONE
#define PLAYER_START_X (3 * FX_ONE + (FX_ONE / 2))
#define PLAYER_START_Y (3 * FX_ONE + (FX_ONE / 2))
#define PLAYER_START_ANGLE 0
#define PLAYER_RADIUS 32
#define FOV_ANGLE 48
#define RAY_VIEW_TILE_H 12
#define DDA_MAX_STEPS 16

static const u8 MAP[MAP_H][MAP_W] = {
    {1, 1, 1, 1, 1, 1, 1, 1},
    {1, 0, 0, 0, 0, 0, 2, 1},
    {1, 0, 0, 0, 1, 0, 0, 1},
    {1, 0, 0, 0, 1, 0, 0, 1},
    {1, 0, 1, 0, 0, 0, 0, 1},
    {1, 0, 1, 0, 0, 2, 0, 1},
    {1, 0, 0, 0, 0, 0, 0, 1},
    {1, 1, 1, 1, 1, 1, 1, 1},
};

static u8 map_at(s16 cell_x, s16 cell_y) {
    if ((cell_x < 0) || (cell_y < 0) || (cell_x >= MAP_W) || (cell_y >= MAP_H)) {
        return 1;
    }

    return MAP[cell_y][cell_x];
}

static bool is_blocked_at(s32 x, s32 y) {
    const s16 min_x = (s16)((x - PLAYER_RADIUS) / MAP_CELL);
    const s16 max_x = (s16)((x + PLAYER_RADIUS) / MAP_CELL);
    const s16 min_y = (s16)((y - PLAYER_RADIUS) / MAP_CELL);
    const s16 max_y = (s16)((y + PLAYER_RADIUS) / MAP_CELL);

    for (s16 cy = min_y; cy <= max_y; cy++) {
        for (s16 cx = min_x; cx <= max_x; cx++) {
            if (map_at(cx, cy) != 0) {
                return TRUE;
            }
        }
    }

    return FALSE;
}

static u16 abs_s16_to_u16(s16 value) {
    return (u16)((value < 0) ? -value : value);
}

static u16 reciprocal_delta_q8(s16 direction) {
    const u16 abs_dir = abs_s16_to_u16(direction);

    if (abs_dir == 0) {
        return 0x7FFF;
    }

    return (u16)(((u32)FX_ONE << FX_SHIFT) / abs_dir);
}

static u16 cast_single_ray(const PlayerState *player, u16 angle, u8 *hit_color, u8 *tex_x) {
    const s16 dir_x = fx_cos(angle);
    const s16 dir_y = fx_sin(angle);
    s16 map_x = (s16)(player->x / MAP_CELL);
    s16 map_y = (s16)(player->y / MAP_CELL);
    s16 step_x;
    s16 step_y;
    u16 side_dist_x;
    u16 side_dist_y;
    const u16 delta_dist_x = reciprocal_delta_q8(dir_x);
    const u16 delta_dist_y = reciprocal_delta_q8(dir_y);
    u8 side = 0;

    if (dir_x < 0) {
        step_x = -1;
        side_dist_x = (u16)(((player->x - ((s32)map_x * MAP_CELL)) * delta_dist_x) >> FX_SHIFT);
    } else {
        step_x = 1;
        side_dist_x = (u16)(((((s32)(map_x + 1) * MAP_CELL) - player->x) * delta_dist_x) >> FX_SHIFT);
    }

    if (dir_y < 0) {
        step_y = -1;
        side_dist_y = (u16)(((player->y - ((s32)map_y * MAP_CELL)) * delta_dist_y) >> FX_SHIFT);
    } else {
        step_y = 1;
        side_dist_y = (u16)(((((s32)(map_y + 1) * MAP_CELL) - player->y) * delta_dist_y) >> FX_SHIFT);
    }

    for (u16 step = 0; step < DDA_MAX_STEPS; step++) {
        if (side_dist_x < side_dist_y) {
            side_dist_x = (u16)(side_dist_x + delta_dist_x);
            map_x = (s16)(map_x + step_x);
            side = 0;
        } else {
            side_dist_y = (u16)(side_dist_y + delta_dist_y);
            map_y = (s16)(map_y + step_y);
            side = 1;
        }

        const u8 tile = map_at(map_x, map_y);
        if (tile != 0) {
            const u16 dist = (side == 0) ? (u16)(side_dist_x - delta_dist_x) : (u16)(side_dist_y - delta_dist_y);
            const s32 hit_x = player->x + (((s32)dir_x * dist) >> FX_SHIFT);
            const s32 hit_y = player->y + (((s32)dir_y * dist) >> FX_SHIFT);
            const u16 wall_coord = (side == 0) ? (u16)(hit_y & (MAP_CELL - 1)) : (u16)(hit_x & (MAP_CELL - 1));

            *tex_x = (u8)(wall_coord >> 5);
            *hit_color = (tile == 2) ? 12 : ((side == 0) ? 4 : 5);
            return (dist == 0) ? 1 : dist;
        }
    }

    *tex_x = 0;
    *hit_color = 3;
    return (u16)(MAP_W * FX_ONE);
}

void raycast_init(void) {
}

void player_init(PlayerState *player) {
    player->x = PLAYER_START_X;
    player->y = PLAYER_START_Y;
    player->angle = PLAYER_START_ANGLE;
}

void player_try_move(PlayerState *player, s16 forward, s16 strafe) {
    const s16 dir_x = fx_cos(player->angle);
    const s16 dir_y = fx_sin(player->angle);
    const s16 side_x = fx_cos((u16)(player->angle + ANGLE_90));
    const s16 side_y = fx_sin((u16)(player->angle + ANGLE_90));
    const s32 dx = (((s32)dir_x * forward) + ((s32)side_x * strafe)) >> FX_SHIFT;
    const s32 dy = (((s32)dir_y * forward) + ((s32)side_y * strafe)) >> FX_SHIFT;
    const s32 next_x = player->x + dx;
    const s32 next_y = player->y + dy;

    if (!is_blocked_at(next_x, player->y)) {
        player->x = next_x;
    }

    if (!is_blocked_at(player->x, next_y)) {
        player->y = next_y;
    }
}

void raycast_cast_frame(const PlayerState *player, RayColumn *columns) {
    for (u16 col = 0; col < RAY_VIEW_COLS; col++) {
        const s16 ray_offset = (s16)((col * FOV_ANGLE) / RAY_VIEW_COLS) - (FOV_ANGLE / 2);
        const u16 ray_angle = (u16)(player->angle + ray_offset);
        const s16 correction = fx_cos((u16)ray_offset);
        u8 color;
        u8 tex_x;
        const u16 dist = cast_single_ray(player, ray_angle, &color, &tex_x);
        u16 corrected_dist = (u16)(((u32)dist * (u16)correction) >> FX_SHIFT);
        u16 height;

        if (corrected_dist == 0) {
            corrected_dist = 1;
        }

        height = (u16)((RAY_VIEW_TILE_H * FX_ONE) / corrected_dist);

        if (height < 1) {
            height = 1;
        } else if (height > RAY_VIEW_TILE_H) {
            height = RAY_VIEW_TILE_H;
        }

        columns[col].height = height;
        columns[col].tex_x = tex_x;
        columns[col].color = color;
    }
}
