#ifndef MEGALDOOM_RAYCAST_H
#define MEGALDOOM_RAYCAST_H

#include <genesis.h>

#define RAY_VIEW_COLS 32

typedef struct {
    s32 x;
    s32 y;
    u16 angle;
} PlayerState;

typedef struct {
    u16 height;
    u16 depth;
    u8 tex_x;
    u8 color;
} RayColumn;

void raycast_init(void);
void player_init(PlayerState *player, u16 phase_index);
void player_try_move(PlayerState *player, s16 forward, s16 strafe);
void player_apply_world_push(PlayerState *player, s32 dx, s32 dy);
void raycast_cast_frame(const PlayerState *player, RayColumn *columns);

#endif
