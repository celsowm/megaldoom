#ifndef MEGALDOOM_RAYCAST_H
#define MEGALDOOM_RAYCAST_H

#include <genesis.h>

#define RAY_VIEW_COLS 40

typedef struct {
    s32 x;
    s32 y;
    u16 angle;
} PlayerState;

typedef enum {
    RAY_TEXTURE_WALL = 0,
    RAY_TEXTURE_DOOR = 1,
    RAY_TEXTURE_LOCKED_DOOR = 2,
    RAY_TEXTURE_EXIT = 3,
    RAY_TEXTURE_WALL_BROWN = 4,
    RAY_TEXTURE_WALL_GRAY = 5,
    RAY_TEXTURE_WALL_METAL = 6,
    RAY_TEXTURE_WALL_BRICK = 7,
    RAY_TEXTURE_WALL_TECH = 8
} RayTextureId;

typedef struct {
    u16 height;
    u16 depth;
    u8 tex_x;
    u8 texture_id;
    u8 shade;
} RayColumn;

void raycast_init(void);
void player_init(PlayerState *player, u16 phase_index);
void player_try_move(PlayerState *player, s16 forward, s16 strafe);
void player_apply_world_push(PlayerState *player, s32 dx, s32 dy);
void raycast_cast_frame(const PlayerState *player, RayColumn *columns);

#endif
