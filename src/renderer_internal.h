#ifndef MEGALDOOM_RENDERER_INTERNAL_H
#define MEGALDOOM_RENDERER_INTERNAL_H

#include "renderer.h"
#include "fixed_math.h"

#define RENDERER_VERSION_TEXT "MEGALDOOM REWRITE GATE 73"
#define VIEW_TILE_W 20
#define VIEW_TILE_H 15
#define VIEW_TILE_COUNT (VIEW_TILE_W * VIEW_TILE_H)
#define VIEW_PIXEL_H (VIEW_TILE_H * 8)
#define VIEW_TILE_BASE TILE_USER_INDEX
#define VIEW_DYNAMIC_TILE_COUNT VIEW_TILE_COUNT
#define VIEW_DIRTY_WORD_COUNT ((VIEW_TILE_COUNT + 31) / 32)
#define VIEW_DIRTY_FULL_THRESHOLD 260
#define VIEW_DIRTY_MAX_RUNS 64
#define PAIR_TILE_BASE (VIEW_TILE_BASE + VIEW_DYNAMIC_TILE_COUNT)
#define PAIR_TILE_COUNT 256
#define HUD_TILE_BASE (PAIR_TILE_BASE + PAIR_TILE_COUNT)
#define FACE_TILE_BASE (HUD_TILE_BASE + FREEDOOM_HUD_TILE_COUNT)
#define VIEW_TILEMAP_X 10
#define VIEW_TILEMAP_Y 5
#define COMPASS_X 3
#define COMPASS_Y 10
#define COMPASS_W 5
#define COMPASS_H 5
#define HUD_PANEL_X 4
#define HUD_PANEL_Y 21
#define HUD_PANEL_W 32
#define HUD_PANEL_H 7

// Doom-guy portrait sits in the recessed face slot at the centre of the status
// bar (panel tile columns ~14-17). 3x4 tiles, top of the recessed interior.
#define HUD_FACE_TILE_X (HUD_PANEL_X + 15)
#define HUD_FACE_TILE_Y (HUD_PANEL_Y + 1)

extern u32 g_view_tiles[VIEW_TILE_COUNT][8];
extern u32 g_base_view_tiles[VIEW_TILE_COUNT][8];
extern u32 g_view_dirty_bits[VIEW_DIRTY_WORD_COUNT];
extern u16 g_view_dirty_count;
extern u16 g_view_tilemap[VIEW_TILE_COUNT];
extern u16 g_compass_tilemap[COMPASS_W * COMPASS_H];

void set_view_pair_tile(u16 x, u16 y, u8 left_color, u8 right_color);
void set_view_column_color(u16 column, u16 y, u8 color);
void renderer_mark_tile_dirty(u16 tile_index);
void renderer_mark_overlay_tile(u16 tile_index);
void renderer_set_bg_pair_tile(u16 x, u16 y, u8 left_color, u8 right_color);
void renderer_scene_init(void);

#endif
