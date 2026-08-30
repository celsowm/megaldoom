#ifndef MEGALDOOM_RENDERER_INTERNAL_H
#define MEGALDOOM_RENDERER_INTERNAL_H

#include "renderer.h"
#include "fixed_math.h"
#include "generated_hud_assets.h"
#include "generated_assets.h"
#include "generated_renderer_assets.h"

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
// Sparse Semantic Framebuffer (Phase 3): max dynamic tiles the sparse uploader
// will push in a single vblank. Above this the caller falls back to the legacy
// full-base upload. Dead code while RENDERER_SPARSE_FB == 0.
#define SPARSE_ONE_VBLANK_BUDGET 120
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
// The 3D view is vertically CENTRED in the play area above the status bar:
// HUD_PANEL_Y (24) rows hold a VIEW_TILE_H (15) row view, so the 9 rows of slack
// split 5 above / 4 below (the odd row goes on top, where it balances the visual
// weight of the status bar). See the static check below.
//
// The weapon bob used to require the view be parked flush on the status bar, so
// that a downward dip pushed the gun into the WINDOW region where plane A is
// suppressed and its cut-off bottom edge was never seen. That is why centring it
// costs nothing: the window does not have to BE the status bar. It is pinned
// from VIEW_WINDOW_TOP_Y (the row right below the view) instead, so the
// plane-A-suppressed region still starts exactly at the view's bottom edge and
// the dip is clipped on the same line it always was -- just 32px higher up.
#define VIEW_TILEMAP_Y 5
#define SCREEN_TILE_W 40
#define SCREEN_TILE_H 28
#define HUD_PANEL_X 0
#define HUD_PANEL_W SCREEN_TILE_W
#define HUD_PANEL_H FREEDOOM_HUD_TILE_H
#define HUD_PANEL_Y (SCREEN_TILE_H - HUD_PANEL_H)
// The window plane spans the black gutter under the view plus the status bar.
// Its gutter rows are transparent tile 0, so BG_B's black shows through and the
// band reads as plain letterbox -- but plane A is suppressed across the whole
// window region regardless of tile content, which is what clips the weapon dip.
#define VIEW_WINDOW_TOP_Y (VIEW_TILEMAP_Y + VIEW_TILE_H)
#define VIEW_WINDOW_TILE_H (SCREEN_TILE_H - VIEW_WINDOW_TOP_Y)
#define VIEW_GUTTER_TILE_H (HUD_PANEL_Y - VIEW_WINDOW_TOP_Y)

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
// Centred, with the odd row of slack on top: 2*Y == (play area - view + 1).
#if (2 * VIEW_TILEMAP_Y) != (HUD_PANEL_Y - VIEW_TILE_H + 1)
#error "3D view must be vertically centred in the play area above the status bar"
#endif
// The weapon-bob anchor: the window (plane A suppressed) must start on the row
// immediately below the view, or a downward dip shows the gun's cut-off bottom
// edge against the letterbox. A gutter is what makes centring possible at all.
#if VIEW_WINDOW_TOP_Y != (VIEW_TILEMAP_Y + VIEW_TILE_H)
#error "Window must start at the view's bottom edge (weapon-bob dip clip)"
#endif
#if VIEW_GUTTER_TILE_H < 1
#error "Centred view needs at least one gutter row between it and the status bar"
#endif

extern u32 g_view_tiles[VIEW_TILE_COUNT][8];
extern u32 g_view_bank_dirty_bits[VIEW_BANK_COUNT][VIEW_DIRTY_WORD_COUNT];
extern u16 g_view_bank_dirty_count[VIEW_BANK_COUNT];
extern u16 g_view_vram_bank;
extern u16 g_view_dirty_bank_mask;

// === Sparse Semantic Framebuffer (Phase 1, Task 1 scaffolding) =============
// When 0 the legacy full-upload path in renderer.c is the single source of
// truth and sparse_classify_frame() compiles to a no-op stub. Flip to 1 in a
// later phase to enable the per-frame semantic classify that builds a
// SparseFrameBuild (dynamic union mask, run list, slot allocation, mixed
// tilemap) instead of uploading all VIEW_TILE_COUNT tiles every frame.
// NOTE: the dynamic union is ONLY walls + doors + overlay COW. Ceiling/floor
// are static (one atlas tile per distinct RayFlatColor ceiling, uploaded once
// at level init; floor is one ROM-constant tile forever) so they cost ZERO
// per-frame DMA — see AGENTS.md "Ceiling/floor cost ZERO DMA during motion".
// Default 0 (legacy full-upload path is the single source of truth). A local
// flag-1 TEST build is produced with
//   EXTRA_FLAGS="-DRENDERER_SPARSE_FB=1"
// so the committed source always defaults to 0 (release never ships sparse
// until its route metrics pass in Phase 5/6).
#ifndef RENDERER_SPARSE_FB
#define RENDERER_SPARSE_FB 0
#endif

// === Static ceiling/floor atlas (Phase 2, Task 2) ==========================
// One VRAM tile holds the ROM-constant floor (MEGALDOOM_WORLD_COLOR_FLOOR,
// uploaded once and never rebaked), and MEGALDOOM_CEILING_TILE_COUNT atlas
// tiles hold one 8x8 tile per distinct (primary, secondary, secondary_coverage)
// ceiling key (see MEGALDOOM_CEILING_TILES in generated_renderer_assets.h). The
// whole atlas is uploaded ONCE at level init; a sector change only repoints a
// tilemap cell at the already-resident atlas tile, costing ZERO per-frame DMA.
// The atlas lives immediately below the SGDK system font region so it can never
// collide with the pair/HUD/weapon tiles above (those grow upward from
// TILE_USER_INDEX). g_sector_ceiling_tile[sector] maps a sector to its atlas
// tile index (per-sector selection is runtime-only; the tiles themselves are
// static). All of this is dead code while RENDERER_SPARSE_FB == 0.
#define STATIC_FLOOR_TILE_BASE (HUD_VRAM_SAFE_TILE_LIMIT - 1)
#define STATIC_CEILING_ATLAS_BASE (STATIC_FLOOR_TILE_BASE - MEGALDOOM_CEILING_TILE_COUNT)
#define STATIC_ATLAS_TILE_COUNT (MEGALDOOM_CEILING_TILE_COUNT + 1)

// Per-frame selection: which resident atlas tile each sector's ceiling maps to.
// Populated once at level init from MEGALDOOM_SECTOR_CEILING_TILE_INDEX.
extern u8 g_sector_ceiling_tile[FREEDOOM_SECTOR_VISUAL_COUNT];

// One vertical run of dynamic tiles copied from a source column into a
// contiguous destination VRAM slot range.
typedef struct {
    u16 source_y;   // source column index x (0..VIEW_TILE_W-1)
    u16 tile_count; // number of contiguous dynamic tiles in the run
    u16 dest_slot;  // destination VRAM slot (column-major index)
} SparseTileRun;

// Per-frame classification of which view tiles actually changed. The packer
// emits three per-column 15-bit masks (wall/door/overlay); OR-ing them gives
// the dynamic mask per column. dynamic_rows[x] is the per-column union of
// changed tiles; runs[] is the packed run list used by the sparse upload.
typedef struct {
    u16 dynamic_mask[VIEW_TILE_W];        // per-column: any dynamic bit set
    u16 dynamic_rows[VIEW_TILE_W];        // per-column 15-bit union of dynamic tiles
    u16 dynamic_tile_count;               // total dynamic tiles this frame
    u16 dynamic_run_count;                // number of runs in runs[]
    SparseTileRun runs[VIEW_TILE_COUNT];  // max runs = every tile its own run
    u16 slot_allocation[VIEW_TILE_COUNT]; // destination slot per dynamic tile
    u16 mixed_tilemap[VIEW_TILE_W];       // mixed tilemap column selection
} SparseFrameBuild;

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
