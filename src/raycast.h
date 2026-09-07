#ifndef MEGALDOOM_RAYCAST_H
#define MEGALDOOM_RAYCAST_H

// The geometry below is the single source of truth for BOTH the C renderer and
// the hand-written pack hotpath (renderer_hotpath.s). SGDK assembles .s through
// `gcc -x assembler-with-cpp`, so the assembler sees these #defines verbatim and
// never has to spell a copy of them out. Everything that is C-only lives behind
// __ASSEMBLER__ so the include stays cheap for the assembler.
#ifndef __ASSEMBLER__
#include <genesis.h>
#endif

// View geometry — single source of truth for the render viewport. The BSP caster,
// billboard and renderer all derive from these: VIEW_TILE_W/H in
// renderer_internal.h alias RAY_VIEW_TILE_W/H, and the billboard projector
// centres sprites on RAY_VIEW_COLS/ROWS. 8px tiles (Mega Drive hardware).
//
// The viewport is RUNTIME-RESIZABLE (the VIEW SIZE row in the frontend OPTIONS
// menu). Two layers:
//
//   * RAY_VIEW_TILE_W_MAX / _H_MAX are compile-time and size every buffer, so
//     the work-RAM cost of the option is paid in full at every size. Work RAM
//     is the binding budget here -- g_view_tiles alone is 32 bytes per tile --
//     so these are the numbers to weigh against tools/check-rom.ps1, never the
//     currently selected size.
//   * RAY_VIEW_TILE_W / _H are the CURRENT size and are plain variables. Every
//     existing call site keeps its old spelling; only array declarations (which
//     take the _MAX forms) and preprocessor tests had to change.
//
// Both maxima are pinned by a real budget, not by taste:
//
//   _H_MAX = 16 is the CEILING TABLE. renderer_pack_internal.h asserts
//   PACK_CEILING_ROW_COUNT (64) covers the highest centred wall top,
//   (VIEW_PIXEL_H - 1) / 2, which is exactly 63 at 16 tiles. Going taller means
//   regenerating the baked ceiling/sky bands and widening the asm ceiling
//   post's PACK_CEILING_INDEX_MASK -- not just raising this number.
//
//   _W_MAX = 22 is WORK RAM. Each extra tile column costs about 800 bytes
//   (g_view_tiles 16*32, plus 8 RayColumns, the pack coherence row and the
//   tilemap entries), and the binding limit is the SGDK heap the frontend
//   unpacks its boot cards through, not the VDP. Measured on this ROM: 20492
//   bytes free boots and completes the checkpoints route; 18076 bytes free
//   crashes in the boot fade with a wild read, well ABOVE the ~13.7 KB where
//   SGDK's own "not enough memory to reset VDP" panic was seen. Treat
//   tools/check-rom.ps1's 20480-byte recommendation as the real floor and
//   re-measure with the route before raising this.
#define RAY_VIEW_TILE_W_MAX 22
#define RAY_VIEW_TILE_H_MAX 16
#define RAY_VIEW_COLS_MAX (RAY_VIEW_TILE_W_MAX * 8)
#define RAY_VIEW_ROWS_MAX (RAY_VIEW_TILE_H_MAX * 8)

// Shared camera geometry. Wall and billboard projection must use these exact
// values or world objects drift against the BSP as the player turns.
//
// These stay COMPILE-TIME CONSTANT across every viewport size, and that is a
// deliberate choice rather than an oversight. Tying the projection to the view
// width (the old RAY_PROJ_X == RAY_VIEW_CENTER_X) would make a wider viewport
// MAGNIFY the same 90-degree field, which costs far more than its extra area
// -- wall and sprite raster both scale with projected height -- and would
// invalidate g_billboard_recip_proj_lut, whose 1535 entries bake
// K == RAY_PROJ_X << 12. Holding them fixed instead makes a bigger viewport
// show MORE WORLD at an unchanged pixel scale: cost grows linearly with area,
// the baked LUT stays exact, and the hot projection multiplies keep an
// immediate operand instead of a memory load. The horizontal field widens from
// 90 degrees at 20 tiles to ~100 degrees at 24.
#define RAY_PROJ_X 80
#define RAY_PROJ_Y 80
#if RAY_PROJ_X != ((20 * 8) / 2)
#error "RAY_PROJ_X is the baked billboard LUT's K; it is the 20-tile half-width"
#endif

#ifndef __ASSEMBLER__
// The CURRENT viewport size, set through raycast_set_view_size(). Spelled as
// macros over the variables so the ~200 existing RAY_VIEW_* / VIEW_TILE_* call
// sites did not have to be rewritten one by one.
extern u16 g_view_tile_w;
extern u16 g_view_tile_h;
extern u16 g_view_cols;
extern u16 g_view_rows;
extern u16 g_view_center_x;
extern u16 g_view_center_y;

#define RAY_VIEW_TILE_W g_view_tile_w
#define RAY_VIEW_TILE_H g_view_tile_h
#define RAY_VIEW_COLS g_view_cols
#define RAY_VIEW_ROWS g_view_rows
#define RAY_VIEW_CENTER_X g_view_center_x
#define RAY_VIEW_CENTER_Y g_view_center_y
#endif
#define RAY_WORLD_WALL_HEIGHT 128
#define RAY_CAMERA_HEIGHT (RAY_WORLD_WALL_HEIGHT / 2)
// Doom's player is 32 map units wide. Using that value as a radius makes real
// Doom doorways (notably E1M2) geometrically impossible, so runtime and offline
// certification share the canonical 16-unit radius.
#define PLAYER_COLLISION_RADIUS 16
// Horizontal render granularity: cast/sample one wall column every N pixels and
// duplicate across the gap. 1 = full 1px detail (heaviest), 2 = 2px (80 cols),
// 4 = 4px (40 cols). Must divide 8. The shipped quality profile is stride 2:
// 80 sampled wall columns across the 160px viewport. Stride 4 shipped briefly
// (a959edd) and was reverted on 2026-07-27 — not for a bug, but because the
// user judged the 4px-replicated walls too pixelated in motion. There is no
// horizontal interpolation between samples, so this constant IS the wall's
// horizontal resolution; raising it is a visual decision, not a free win.
// Guarded so a comparison build can override it (EXTRA_FLAGS="-DRAY_COL_STRIDE=4").
#ifndef RAY_COL_STRIDE
#define RAY_COL_STRIDE 2
#endif
#define RAY_SAMPLE_COLS (RAY_VIEW_COLS / RAY_COL_STRIDE)
#define RAY_SAMPLE_COLS_MAX (RAY_VIEW_COLS_MAX / RAY_COL_STRIDE)
#define PLAYER_HEIGHT 56
#define PLAYER_EYE_HEIGHT 41
#define PLAYER_MAX_STEP 24

// Wall/door/switch textures use independent power-of-two runtime axes. The
// horizontal axis stays at 64 because RAY_COL_STRIDE==2 supplies 80 samples;
// the vertical axis is 128 so a near wall can retain its source row structure.
// Exact source dimensions and repeat scales are generated from the active WAD.
#define WALL_TEX_WIDTH 64
#define WALL_TEX_WIDTH_MASK (WALL_TEX_WIDTH - 1)
#define WALL_TEX_HEIGHT 128
#define WALL_TEX_HEIGHT_MASK (WALL_TEX_HEIGHT - 1)
// The near clip can project a 128-unit wall to 640 pixels (depth 16). Keep
// this unclipped height for vertical texture lookup even though only 120 rows
// can reach the viewport; otherwise a wall touching the camera remaps its
// entire texture into the visible screen and appears stretched.
#define RAY_MAX_PROJECTED_WALL_HEIGHT 640

#ifndef __ASSEMBLER__

typedef struct {
    s32 x;
    s32 y;
    u16 angle;
} PlayerState;

typedef struct {
    u8 primary;
    u8 secondary;
    u8 secondary_coverage; // 0..16 Bayer cells
} RayFlatColor;

typedef struct {
    RayFlatColor ceiling;
    RayFlatColor floor;
    // Player's current sector id (computed by bsp_cast_frame). Used by the
    // sparse framebuffer to pick the resident ceiling atlas tile. Zero-cost at
    // runtime: set once per cast, read by the renderer.
    u16 sector;
    // TRUE when that sector's Doom ceiling flat is F_SKY1. The pack stage then
    // sources its ceiling run from the baked sky bands instead of `ceiling`.
    bool sky;
    // Heading as a sky tile-column offset (0..MEGALDOOM_SKY_TILE_COLUMNS-1):
    // which column of the baked sky sits at the left edge of the viewport. This
    // is what makes the horizon slide when the player turns, and it is set on
    // EVERY frame, not just sky ones -- a window looked through from an indoor
    // sector paints sky inside its band and needs the same offset to agree with
    // the courtyard you see when you walk out there.
    u8 sky_offset;
} RaySceneColors;

// The near-surface overlay: one surface composited in front of whatever the
// BSP pass left in the column, at its own depth. Two things use it.
//
//   * A MOVING DOOR -- a slab whose raised lower gap reveals the scene behind.
//   * A WINDOW -- a wall with a see-through band, revealing the scene behind
//     between band_top and band_bottom.
//
// `lift` discriminates them: a door overlay is only ever written while its
// lift is 1..255 (a closed door draws as an ordinary wall, and an open one is
// skipped entirely), so lift == 0 means WINDOW. bsp_cast_frame clears
// `height` for every sampled column each frame, so a stale door overlay can
// never be misread as a window.
//
// Field widths are chosen to keep this struct at 10 bytes: it is embedded in
// every one of the RAY_VIEW_COLS_MAX RayColumns, and on a 64 KB machine growing
// it by two bytes costs 384 bytes of work RAM. `height` is clipped to
// RAY_VIEW_ROWS and `lift` to 255 before they are stored, so both fit a byte
// (RAY_VIEW_ROWS_MAX is 128).
typedef struct {
    u16 depth;
    u8 height;      // visible slab height after viewport clipping
    u8 lift;        // door: Q8 1..255. 0 marks a window (see above).
    u8 tex_x;
    u8 tex_y;
    u8 texture_id;
    u8 shade;
    // Absolute viewport rows, not Q8 fractions: bsp_draw_seg resolves the seg's
    // Q8 band against the slab it just projected, so neither the compositor nor
    // the billboard clip has to multiply. Both are <= RAY_VIEW_ROWS.
    // Variant storage: for a window these are its absolute band rows. For a
    // moving door band_top carries RAY_OVERLAY_FLAG_* and band_bottom is zero.
    // The variants are disjoint via lift, preserving this 10-byte structure.
    u8 band_top;    // window: first see-through row; door: overlay flags
    u8 band_bottom; // window: first opaque row below the band
} RayDoorOverlay;

#define RAY_COLUMN_FLAG_DOOR 0x01u
#define RAY_COLUMN_FLAG_FLOOR_ALIGNED 0x02u
#define RAY_OVERLAY_FLAG_PLAIN_DOOR 0x01u

// See the lift discriminator note on RayDoorOverlay.
static inline bool ray_overlay_is_window(const RayDoorOverlay *overlay) {
    return (bool)(overlay->lift == 0);
}

static inline bool ray_overlay_is_plain_door(const RayDoorOverlay *overlay) {
    return (bool)(overlay->lift != 0 &&
                  (overlay->band_top & RAY_OVERLAY_FLAG_PLAIN_DOOR));
}

typedef struct {
    u16 height; // visible slab height after viewport clipping
    u16 projected_height; // unclipped height used for vertical texture lookup
    u16 depth;
    u8 tex_x;
    u8 tex_y;
    u8 texture_id;
    u8 shade;
    u8 flags;
    RayDoorOverlay door;
} RayColumn;

// Guards the packing note above: this struct is instantiated RAY_VIEW_COLS
// times, so a silent growth here is 160x the cost.
_Static_assert(sizeof(RayDoorOverlay) == 10,
               "RayDoorOverlay must stay 10 bytes (see the field-width note)");

// ---- Viewport size presets ------------------------------------------------
// The VIEW SIZE row in the frontend OPTIONS menu selects one of these. Index 0
// is the historical viewport and stays the default. Widths must be EVEN so the
// view centres exactly on the 40-tile screen; heights are bounded by
// RAY_VIEW_TILE_H_MAX (see its note). Larger sizes do not magnify the world --
// RAY_PROJ_X/Y are fixed -- they widen the field and show more of it.
#define RAY_VIEW_SIZE_COUNT 3
#define RAY_VIEW_SIZE_DEFAULT 0

// Spelled as macros rather than only as table rows so the guards in raycast.c
// can prove at COMPILE TIME that no preset outgrows the buffers. A preset wider
// or taller than the _MAX pair would overrun g_view_tiles at runtime.
#define RAY_VIEW_SIZE_0_W 20
#define RAY_VIEW_SIZE_0_H 15
#define RAY_VIEW_SIZE_1_W 22
#define RAY_VIEW_SIZE_1_H 15
#define RAY_VIEW_SIZE_2_W 22
#define RAY_VIEW_SIZE_2_H 16

typedef struct {
    u8 tile_w;
    u8 tile_h;
} RayViewSize;

extern const RayViewSize g_view_sizes[RAY_VIEW_SIZE_COUNT];

// Currently selected preset index. Change it through renderer_set_view_size(),
// which also rebuilds the VDP state that depends on the geometry; calling the
// raycast-level setter alone leaves the tilemap and window plane stale.
u16 raycast_view_size(void);
bool raycast_set_view_size(u16 size_index);

void player_init(PlayerState *player, u16 phase_index);
void player_try_move(PlayerState *player, s16 forward, s16 strafe);
void player_apply_world_push(PlayerState *player, s32 dx, s32 dy);

#endif // __ASSEMBLER__

#endif
