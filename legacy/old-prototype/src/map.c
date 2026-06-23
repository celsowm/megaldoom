#include <genesis.h>
#include "game.h"
#include "map.h"
#include "trig1024_q8.h"

const u8 g_initial_map[MAP_H][MAP_W] = {
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,1,1,1,0,0,0,0,2,0,1},
    {1,0,0,0,0,0,1,3,1,0,0,0,0,2,0,1},
    {1,0,0,1,1,0,1,3,1,0,0,0,0,2,0,1},
    {1,0,0,1,0,0,0,0,1,0,0,1,1,1,0,1},
    {1,0,0,1,0,0,0,0,1,0,0,1,3,0,0,1},
    {1,0,0,1,0,0,0,0,0,0,0,1,3,0,0,1},
    {1,0,0,1,1,1,0,0,0,1,1,1,0,1,1,1},
    {1,0,0,0,0,1,0,0,0,1,0,0,0,0,0,1},
    {1,0,0,0,0,1,0,0,0,1,0,0,0,0,0,1},
    {1,0,1,1,0,1,0,0,0,1,0,1,1,1,0,1},
    {1,0,0,1,0,0,0,1,0,0,0,0,0,1,0,1},
    {1,0,0,1,0,0,0,1,0,0,0,0,0,1,0,1},
    {1,1,2,1,0,0,0,1,0,0,0,0,0,0,0,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
};

u8 g_world_map[MAP_H][MAP_W];

static inline s16 sin_q8_local(u16 a) {
    return SIN1024_Q8[a & ANGLE_MASK];
}

static inline s16 cos_q8_local(u16 a) {
    return SIN1024_Q8[(a + ANGLE_90) & ANGLE_MASK];
}

void map_init(void) {
    for (s16 y = 0; y < MAP_H; y++) {
        for (s16 x = 0; x < MAP_W; x++) {
            g_world_map[y][x] = g_initial_map[y][x];
        }
    }
}

u8 map_toggle_door_in_front(const Player *player) {
    s16 ca = cos_q8_local(player->angle);
    s16 sa = sin_q8_local(player->angle);

    // Probe a few points in front of the player. This avoids requiring perfect alignment.
    for (s32 dist = FP_ONE / 2; dist <= FP_ONE * 2; dist += FP_ONE / 2) {
        s32 fx = player->x + (((s32)ca * dist) >> FP_SHIFT);
        s32 fy = player->y + (((s32)sa * dist) >> FP_SHIFT);
        s16 mx = (s16)(fx >> FP_SHIFT);
        s16 my = (s16)(fy >> FP_SHIFT);

        if (mx < 0 || my < 0 || mx >= MAP_W || my >= MAP_H) continue;

        if (g_initial_map[my][mx] == TILE_DOOR) {
            if (g_world_map[my][mx] == TILE_DOOR) {
                g_world_map[my][mx] = TILE_EMPTY;
            } else {
                g_world_map[my][mx] = TILE_DOOR;
            }
            return TRUE;
        }
    }

    return FALSE;
}
