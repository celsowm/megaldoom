#ifndef MEGALDOOM_WORLD_MAP_H
#define MEGALDOOM_WORLD_MAP_H

#include <genesis.h>
#include "fixed_math.h"

#define WORLD_MAP_W 8
#define WORLD_MAP_H 8
#define WORLD_CELL_SIZE FX_ONE

#define WORLD_TILE_EMPTY 0
#define WORLD_TILE_WALL 1
#define WORLD_TILE_DOOR 2
#define WORLD_TILE_LOCKED_DOOR 3
#define WORLD_TILE_EXIT_SWITCH 4

typedef enum {
    DOOR_ACTION_NONE = 0,
    DOOR_ACTION_TOGGLED = 1,
    DOOR_ACTION_LOCKED = 2,
    DOOR_ACTION_UNLOCKED = 3,
    DOOR_ACTION_EXIT = 4,
    DOOR_ACTION_EXIT_LOCKED = 5
} DoorActionResult;

void world_map_init(u16 phase_index);
u8 world_map_get_tile(s16 cell_x, s16 cell_y);
bool world_map_is_solid(s16 cell_x, s16 cell_y);
DoorActionResult world_map_toggle_door_in_front(s32 x, s32 y, u16 angle, bool has_key, bool *consumed_key);

#endif
