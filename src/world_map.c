#include "world_map.h"

static const u8 INITIAL_WORLD_MAP[WORLD_MAP_H][WORLD_MAP_W] = {
    {1, 1, 1, 1, 1, 1, 1, 1},
    {1, 0, 0, 0, 0, 0, 2, 1},
    {1, 0, 0, 0, 1, 0, 0, 1},
    {1, 0, 0, 0, 1, 0, 0, 1},
    {1, 0, 1, 0, 0, 0, 0, 1},
    {1, 0, 1, 0, 0, 2, 0, 1},
    {1, 0, 0, 0, 0, 0, 0, 1},
    {1, 1, 1, 1, 1, 1, 1, 1},
};

static u8 g_world_map[WORLD_MAP_H][WORLD_MAP_W];

void world_map_init(void) {
    for (u16 y = 0; y < WORLD_MAP_H; y++) {
        for (u16 x = 0; x < WORLD_MAP_W; x++) {
            g_world_map[y][x] = INITIAL_WORLD_MAP[y][x];
        }
    }
}

u8 world_map_get_tile(s16 cell_x, s16 cell_y) {
    if ((cell_x < 0) || (cell_y < 0) || (cell_x >= WORLD_MAP_W) || (cell_y >= WORLD_MAP_H)) {
        return WORLD_TILE_WALL;
    }

    return g_world_map[cell_y][cell_x];
}

bool world_map_is_solid(s16 cell_x, s16 cell_y) {
    return world_map_get_tile(cell_x, cell_y) != WORLD_TILE_EMPTY;
}

bool world_map_toggle_door_in_front(s32 x, s32 y, u16 angle) {
    const s16 dir_x = fx_cos(angle);
    const s16 dir_y = fx_sin(angle);

    for (s32 dist = FX_ONE / 2; dist <= FX_ONE * 2; dist += FX_ONE / 2) {
        const s32 probe_x = x + (((s32)dir_x * dist) >> FX_SHIFT);
        const s32 probe_y = y + (((s32)dir_y * dist) >> FX_SHIFT);
        const s16 cell_x = (s16)(probe_x / WORLD_CELL_SIZE);
        const s16 cell_y = (s16)(probe_y / WORLD_CELL_SIZE);

        if ((cell_x < 0) || (cell_y < 0) || (cell_x >= WORLD_MAP_W) || (cell_y >= WORLD_MAP_H)) {
            continue;
        }

        if (INITIAL_WORLD_MAP[cell_y][cell_x] == WORLD_TILE_DOOR) {
            g_world_map[cell_y][cell_x] =
                (g_world_map[cell_y][cell_x] == WORLD_TILE_DOOR) ? WORLD_TILE_EMPTY : WORLD_TILE_DOOR;
            return TRUE;
        }
    }

    return FALSE;
}
