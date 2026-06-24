#include "world_map.h"

static const u8 INITIAL_WORLD_MAPS[2][WORLD_MAP_H][WORLD_MAP_W] = {
    {
        {5, 6, 7, 8, 9, 6, 5, 8},
        {5, 0, 0, 0, 0, 4, 3, 8},
        {6, 0, 0, 0, 7, 0, 0, 9},
        {7, 0, 0, 0, 6, 0, 0, 5},
        {8, 0, 5, 0, 0, 0, 0, 6},
        {9, 0, 8, 0, 0, 2, 0, 7},
        {6, 0, 0, 0, 0, 0, 0, 5},
        {8, 5, 6, 7, 8, 9, 6, 5},
    },
    {
        {6, 5, 8, 9, 7, 6, 5, 8},
        {6, 0, 0, 0, 0, 0, 4, 8},
        {5, 0, 7, 8, 0, 9, 0, 6},
        {8, 0, 0, 0, 0, 7, 0, 5},
        {9, 3, 6, 0, 0, 0, 0, 7},
        {7, 0, 5, 0, 8, 0, 0, 9},
        {5, 0, 0, 0, 6, 2, 0, 8},
        {8, 6, 5, 7, 9, 8, 6, 5},
    }
};

static u8 g_world_map[WORLD_MAP_H][WORLD_MAP_W];
static u8 g_world_map_origin[WORLD_MAP_H][WORLD_MAP_W];

void world_map_init(u16 phase_index) {
    const u16 phase = (u16)(phase_index & 1);

    for (u16 y = 0; y < WORLD_MAP_H; y++) {
        for (u16 x = 0; x < WORLD_MAP_W; x++) {
            g_world_map[y][x] = INITIAL_WORLD_MAPS[phase][y][x];
            g_world_map_origin[y][x] = INITIAL_WORLD_MAPS[phase][y][x];
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

DoorActionResult world_map_toggle_door_in_front(s32 x, s32 y, u16 angle, bool has_key, bool *consumed_key) {
    const s16 dir_x = fx_cos(angle);
    const s16 dir_y = fx_sin(angle);

    if (consumed_key != NULL) {
        *consumed_key = FALSE;
    }

    for (s32 dist = FX_ONE / 2; dist <= FX_ONE * 2; dist += FX_ONE / 2) {
        const s32 probe_x = x + (((s32)dir_x * dist) >> FX_SHIFT);
        const s32 probe_y = y + (((s32)dir_y * dist) >> FX_SHIFT);
        const s16 cell_x = (s16)(probe_x / WORLD_CELL_SIZE);
        const s16 cell_y = (s16)(probe_y / WORLD_CELL_SIZE);

        if ((cell_x < 0) || (cell_y < 0) || (cell_x >= WORLD_MAP_W) || (cell_y >= WORLD_MAP_H)) {
            continue;
        }

        if (g_world_map_origin[cell_y][cell_x] == WORLD_TILE_DOOR) {
            g_world_map[cell_y][cell_x] =
                (g_world_map[cell_y][cell_x] == WORLD_TILE_DOOR) ? WORLD_TILE_EMPTY : WORLD_TILE_DOOR;
            return DOOR_ACTION_TOGGLED;
        }

        if (g_world_map_origin[cell_y][cell_x] == WORLD_TILE_EXIT_SWITCH) {
            return DOOR_ACTION_EXIT;
        }

        if (g_world_map_origin[cell_y][cell_x] == WORLD_TILE_LOCKED_DOOR) {
            if (!has_key) {
                return DOOR_ACTION_LOCKED;
            }

            if (g_world_map[cell_y][cell_x] == WORLD_TILE_LOCKED_DOOR) {
                g_world_map_origin[cell_y][cell_x] = WORLD_TILE_DOOR;
                g_world_map[cell_y][cell_x] = WORLD_TILE_EMPTY;
                if (consumed_key != NULL) {
                    *consumed_key = TRUE;
                }
                return DOOR_ACTION_UNLOCKED;
            } else {
                g_world_map[cell_y][cell_x] =
                    (g_world_map[cell_y][cell_x] == WORLD_TILE_DOOR) ? WORLD_TILE_EMPTY : WORLD_TILE_DOOR;
                return DOOR_ACTION_TOGGLED;
            }
        }
    }

    return DOOR_ACTION_NONE;
}
