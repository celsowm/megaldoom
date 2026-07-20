#ifndef MEGALDOOM_RENDERER_INTERNAL_H
#define MEGALDOOM_RENDERER_INTERNAL_H

#include "renderer.h"
#include "fixed_math.h"
#include "generated_hud_assets.h"

#define RENDERER_VERSION_TEXT "MEGALDOOM REWRITE GATE 73"
// Tile-grid dimensions alias the BSP view geometry (single source of
// truth in raycast.h, included via renderer.h) so the renderer g_view_tiles
// layout can never drift from what the BSP caster/billboard/projector assume.
#define VIEW_TILE_W RAY_VIEW_TILE_W
#define VIEW_TILE_H RAY_VIEW_TILE_H
#define VIEW_TILE_COUNT (VIEW_TILE_W * VIEW_TILE_H)
// Column-major view buffer: a screen column's VIEW_TILE_H tiles are contiguous
// so a changed column uploads as one DMA run. screen (tile_x, tile_y) ->
// tile_x * VIEW_TILE_H + tile_y. Use this everywhere instead of the old
// (tile_y * VIEW_TILE_W + tile_x) row-major arithmetic.
static inline u16 view_tile_index(u16 tile_x, u16 tile_y) {
    return (u16)((tile_x * VIEW_TILE_H) + tile_y);
}
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
#define HUD_NUMBER_TILE_BASE (FACE_TILE_BASE + FREEDOOM_FACE_TILE_COUNT)
#define HUD_NUMBER_AMMO_TILE_W 6
#define HUD_NUMBER_HEALTH_TILE_W 7
#define HUD_NUMBER_FRAGS_TILE_W 5
#define HUD_NUMBER_ARMOR_TILE_W 8
#define HUD_NUMBER_TILE_H 3
#define HUD_NUMBER_AMMO_TILE_COUNT (HUD_NUMBER_AMMO_TILE_W * HUD_NUMBER_TILE_H)
#define HUD_NUMBER_HEALTH_TILE_COUNT (HUD_NUMBER_HEALTH_TILE_W * HUD_NUMBER_TILE_H)
#define HUD_NUMBER_FRAGS_TILE_COUNT (HUD_NUMBER_FRAGS_TILE_W * HUD_NUMBER_TILE_H)
#define HUD_NUMBER_ARMOR_TILE_COUNT (HUD_NUMBER_ARMOR_TILE_W * HUD_NUMBER_TILE_H)
#define HUD_NUMBER_TILE_COUNT (HUD_NUMBER_AMMO_TILE_COUNT + HUD_NUMBER_HEALTH_TILE_COUNT + HUD_NUMBER_FRAGS_TILE_COUNT + HUD_NUMBER_ARMOR_TILE_COUNT)
#define HUD_NUMBER_MAX_FIELD_TILES HUD_NUMBER_ARMOR_TILE_COUNT
#define WEAPON_TILE_BASE (HUD_NUMBER_TILE_BASE + HUD_NUMBER_TILE_COUNT)
#define HUD_VRAM_SAFE_TILE_LIMIT 1440
#define VIEW_TILEMAP_X 10
#define VIEW_TILEMAP_Y 5
#define COMPASS_X 3
#define COMPASS_Y 10
#define COMPASS_W 5
#define COMPASS_H 5
#define SCREEN_TILE_W 40
#define SCREEN_TILE_H 28
#define HUD_PANEL_X 0
#define HUD_PANEL_W SCREEN_TILE_W
#define HUD_PANEL_H FREEDOOM_HUD_TILE_H
#define HUD_PANEL_Y (SCREEN_TILE_H - HUD_PANEL_H)

// Doom-guy portrait sits in the recessed face slot at the centre of the status
// bar. The generated 4-tile block matches the 32px recess and centres the
// original 24px portrait with four transparent/background pixels per side.
#define HUD_FACE_TILE_X ((SCREEN_TILE_W - FREEDOOM_FACE_TILE_W) / 2)
#define HUD_FACE_TILE_Y HUD_PANEL_Y
#define HUD_FACE_CONTENT_PIXEL_X ((HUD_FACE_TILE_X * 8) + FREEDOOM_FACE_CONTENT_PAD_X)

#if FREEDOOM_HUD_PIXEL_W != (SCREEN_TILE_W * 8)
#error "HUD backdrop must fill the 320px screen width"
#endif
#if FREEDOOM_HUD_PIXEL_H != (HUD_PANEL_H * 8)
#error "HUD pixel and tile heights disagree"
#endif
#if HUD_PANEL_X != 0 || (HUD_PANEL_X + HUD_PANEL_W) != SCREEN_TILE_W
#error "HUD must touch both horizontal screen edges"
#endif
#if (HUD_PANEL_Y + HUD_PANEL_H) != SCREEN_TILE_H
#error "HUD must be flush with the bottom screen edge"
#endif
#if ((2 * HUD_FACE_CONTENT_PIXEL_X) + FREEDOOM_FACE_SOURCE_W) != (SCREEN_TILE_W * 8)
#error "Visible Doom face content must be exactly screen-centred"
#endif
#if (HUD_NUMBER_TILE_BASE + HUD_NUMBER_TILE_COUNT) > HUD_VRAM_SAFE_TILE_LIMIT
#error "HUD number tiles overlap the SGDK font VRAM region"
#endif

extern u32 g_view_tiles[VIEW_TILE_COUNT][8];
extern u32 g_view_bank_dirty_bits[VIEW_BANK_COUNT][VIEW_DIRTY_WORD_COUNT];
extern u16 g_view_bank_dirty_count[VIEW_BANK_COUNT];
extern u16 g_view_vram_bank;
extern u16 g_view_dirty_bank_mask;
extern u16 g_compass_tilemap[COMPASS_W * COMPASS_H];

#if DEBUG_PERF
void renderer_draw_perf_overlay(bool frame_complete);
#endif

void set_view_pair_tile(u16 x, u16 y, u8 left_color, u8 right_color);
void set_view_column_color(u16 column, u16 y, u8 color);
void renderer_mark_tile_dirty(u16 tile_index);
void renderer_mark_overlay_tile(u16 tile_index);
void renderer_overlay_reset(void);
void renderer_overlay_base_rebuilt(void);
void renderer_overlay_restore_previous(void);
void renderer_overlay_begin(void);
void renderer_overlay_finish(void);
u32 renderer_overlay_prev_columns(void);
bool renderer_overlay_requires_base_rebuild(void);
void renderer_set_bg_pair_tile(u16 x, u16 y, u8 left_color, u8 right_color);
void renderer_set_view_vram_bank(u16 bank);
void renderer_prepare_full_base_upload(void);
void renderer_scene_init(void);

#if DEBUG_PERF
// Per-frame "tiles modified" tracker: counts distinct view tiles whose CPU-side
// buffer changed this frame (deduplicated), independent of which VRAM bank the
// DMA targets. Reset at the start of renderer_render_scene.
void renderer_reset_frame_modified(void);
u16 renderer_get_frame_modified_count(void);
#endif


#endif
