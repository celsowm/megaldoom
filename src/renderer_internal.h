#ifndef MEGALDOOM_RENDERER_INTERNAL_H
#define MEGALDOOM_RENDERER_INTERNAL_H

#include "renderer.h"
#include "fixed_math.h"

#define RENDERER_VERSION_TEXT "MEGALDOOM REWRITE GATE 73"
// Tile-grid dimensions alias the BSP view geometry (single source of
// truth in raycast.h, included via renderer.h) so the renderer g_view_tiles
// layout can never drift from what the BSP caster/billboard/projector assume.
#define VIEW_TILE_W RAY_VIEW_TILE_W
#define VIEW_TILE_H RAY_VIEW_TILE_H
#define VIEW_TILE_COUNT (VIEW_TILE_W * VIEW_TILE_H)
#define VIEW_PIXEL_H (VIEW_TILE_H * 8)
#define VIEW_TILE_BASE TILE_USER_INDEX
#define VIEW_BANK_COUNT 2
#define VIEW_DYNAMIC_TILE_COUNT (VIEW_TILE_COUNT * VIEW_BANK_COUNT)
#define VIEW_DIRTY_WORD_COUNT ((VIEW_TILE_COUNT + 31) / 32)
#define VIEW_DIRTY_FULL_THRESHOLD 220
#define VIEW_DIRTY_MAX_RUNS 24
#define VIEW_DMA_TILES_PER_VBLANK 150
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
extern u32 g_view_bank_dirty_bits[VIEW_BANK_COUNT][VIEW_DIRTY_WORD_COUNT];
extern u16 g_view_bank_dirty_count[VIEW_BANK_COUNT];
extern u16 g_view_vram_bank;
extern u16 g_compass_tilemap[COMPASS_W * COMPASS_H];

#if DEBUG_PERF
typedef struct {
    u16 upload_dirty_tiles;
    u16 upload_tiles;
    u16 upload_runs;
    bool upload_full;
    bool upload_swap;
    u32 gameplay_subticks;
    u32 cast_subticks;
    u32 pack_subticks;
    u32 projection_subticks;
    u32 billboard_subticks;
    u32 weapon_subticks;
    u32 upload_prepare_subticks;
    u32 dma_wait_subticks;
    u16 total_vblanks;
    u16 max_vblanks;
    u16 missed_deadlines;
} RendererPerfSnapshot;

RendererPerfSnapshot renderer_get_perf_snapshot(void);
void renderer_draw_perf_overlay(bool frame_complete);
#endif

void set_view_pair_tile(u16 x, u16 y, u8 left_color, u8 right_color);
void set_view_column_color(u16 column, u16 y, u8 color);
void renderer_mark_tile_dirty(u16 tile_index);
void renderer_mark_overlay_tile(u16 tile_index);
void renderer_set_bg_pair_tile(u16 x, u16 y, u8 left_color, u8 right_color);
void renderer_set_view_vram_bank(u16 bank);
void renderer_scene_init(void);

#if DEBUG_PERF
// Per-frame "tiles modified" tracker: counts distinct view tiles whose CPU-side
// buffer changed this frame (deduplicated), independent of which VRAM bank the
// DMA targets. Reset at the start of renderer_render_scene.
void renderer_reset_frame_modified(void);
u16 renderer_get_frame_modified_count(void);
#endif


#endif
