#ifndef MEGALDOOM_GAME_H
#define MEGALDOOM_GAME_H

#include <genesis.h>

#define VERSION_STRING "MegalDoom 0.0.4"

#define FP_SHIFT 8
#define FP_ONE   (1 << FP_SHIFT)
#define TO_FP(x) ((x) << FP_SHIFT)

#define ANGLE_MASK 1023
#define ANGLE_90   256
#define ANGLE_180  512
#define ANGLE_270  768
#define ANGLE_FULL 1024

#define MAP_W 16
#define MAP_H 16

#define CANVAS_W 256
#define CANVAS_H 160

#define VIEW_X 0
#define VIEW_Y 0
#define VIEW_W 256
#define VIEW_H 112
#define VIEW_COLS 64
#define COL_W 4

#define HUD_Y VIEW_H
#define HUD_H (CANVAS_H - VIEW_H)

// 170 / 1024 of a full circle is about 59.7 degrees.
#define FOV_ANGLE 170
#define RAY_MAX   (FP_ONE * 14)
#define DDA_MAX_STEPS 32

#define PLAYER_RADIUS 36
#define PLAYER_SPEED  14
#define STRAFE_SPEED  11
#define ROT_SPEED     9

#define ENABLE_MINIMAP 1
#define ACTOR_MAX_SCREEN_H 96
#define ACTOR_X_STEP 2

#define TILE_EMPTY 0
#define TILE_WALL  1
#define TILE_DOOR  2
#define TILE_TECH  3

#define PALIDX(i) ((u8)(((i) << 4) | (i)))

#define C_BLACK       PALIDX(0)
#define C_CEIL_DARK   PALIDX(1)
#define C_CEIL_LIGHT  PALIDX(2)
#define C_FLOOR_DARK  PALIDX(3)
#define C_FLOOR_LIGHT PALIDX(4)
#define C_WALL_DARK   PALIDX(5)
#define C_WALL_MID    PALIDX(6)
#define C_WALL_LIGHT  PALIDX(7)
#define C_DOOR_DARK   PALIDX(8)
#define C_DOOR_LIGHT  PALIDX(9)
#define C_TECH_DARK   PALIDX(10)
#define C_TECH_LIGHT  PALIDX(11)
#define C_HUD_BG      PALIDX(12)
#define C_HUD_LINE    PALIDX(13)
#define C_PLAYER      PALIDX(14)
#define C_WHITE       PALIDX(15)

typedef struct Player {
    s32 x;
    s32 y;
    u16 angle;
} Player;

typedef struct Actor {
    s32 x;
    s32 y;
    u8 alive;
} Actor;

typedef struct RayHit {
    u8 tile;
    u8 side;   // 0 = vertical grid side, 1 = horizontal grid side
    u8 tex_x;  // 0..63 fake texture coordinate
    s16 map_x;
    s16 map_y;
    s32 dist;  // Q8 distance along ray
} RayHit;

#endif
