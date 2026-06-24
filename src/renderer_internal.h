#ifndef MEGALDOOM_RENDERER_INTERNAL_H
#define MEGALDOOM_RENDERER_INTERNAL_H

#include "renderer.h"
#include "fixed_math.h"

#define RENDERER_VERSION_TEXT "MEGALDOOM REWRITE GATE 60"
#define VIEW_TILE_W 16
#define VIEW_TILE_H 12
#define VIEW_TILE_COUNT (VIEW_TILE_W * VIEW_TILE_H)
#define VIEW_TILE_BASE TILE_USER_INDEX
#define PAIR_TILE_COUNT 256
#define VIEW_TILEMAP_X 12
#define VIEW_TILEMAP_Y 8
#define COMPASS_X 4
#define COMPASS_Y 8
#define COMPASS_W 5
#define COMPASS_H 5

extern u32 g_pair_tiles[PAIR_TILE_COUNT][8];
extern u16 g_view_tilemap[VIEW_TILE_COUNT];
extern u16 g_compass_tilemap[COMPASS_W * COMPASS_H];
extern u8 g_wall_tex_y_by_height[VIEW_TILE_H + 1][VIEW_TILE_H];

void set_view_pair_tile(u16 x, u16 y, u8 left_color, u8 right_color);
void set_view_column_color(u16 column, u16 y, u8 color);

#endif
