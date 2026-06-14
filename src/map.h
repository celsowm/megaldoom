#ifndef MEGALDOOM_MAP_H
#define MEGALDOOM_MAP_H

#include <genesis.h>
#include "game.h"

extern const u8 g_initial_map[MAP_H][MAP_W];
extern u8 g_world_map[MAP_H][MAP_W];

void map_init(void);
u8 map_toggle_door_in_front(const Player *player);

static inline u8 map_cell(s16 mx, s16 my) {
    if (mx < 0 || my < 0 || mx >= MAP_W || my >= MAP_H) return TILE_WALL;
    return g_world_map[my][mx];
}

static inline u8 map_initial_cell(s16 mx, s16 my) {
    if (mx < 0 || my < 0 || mx >= MAP_W || my >= MAP_H) return TILE_WALL;
    return g_initial_map[my][mx];
}

static inline u8 map_cell_fixed(s32 fx, s32 fy) {
    return map_cell((s16)(fx >> FP_SHIFT), (s16)(fy >> FP_SHIFT));
}

static inline u8 is_blocked_fixed(s32 fx, s32 fy) {
    if (map_cell_fixed(fx, fy)) return TRUE;

    // Small collision radius, checked on four corners.
    if (map_cell_fixed(fx + PLAYER_RADIUS, fy + PLAYER_RADIUS)) return TRUE;
    if (map_cell_fixed(fx - PLAYER_RADIUS, fy + PLAYER_RADIUS)) return TRUE;
    if (map_cell_fixed(fx + PLAYER_RADIUS, fy - PLAYER_RADIUS)) return TRUE;
    if (map_cell_fixed(fx - PLAYER_RADIUS, fy - PLAYER_RADIUS)) return TRUE;

    return FALSE;
}

#endif
