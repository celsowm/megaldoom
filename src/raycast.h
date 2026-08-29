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
#define RAY_VIEW_TILE_W 20
#define RAY_VIEW_TILE_H 15
#define RAY_VIEW_COLS (RAY_VIEW_TILE_W * 8)
#define RAY_VIEW_ROWS (RAY_VIEW_TILE_H * 8)
#define RAY_VIEW_CENTER_X (RAY_VIEW_COLS / 2)
#define RAY_VIEW_CENTER_Y (RAY_VIEW_ROWS / 2)
// Shared camera geometry. Wall and billboard projection must use these exact
// values or world objects drift against the BSP as the player turns.
#define RAY_PROJ_X RAY_VIEW_CENTER_X
#define RAY_PROJ_Y RAY_VIEW_CENTER_X
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
} RaySceneColors;

typedef struct {
    u16 height;   // visible slab height after viewport clipping
    u16 depth;
    u16 lift;     // Q8, 1..255 while the overlay is active
    u8 tex_x;
    u8 tex_y;
    u8 texture_id;
    u8 shade;
} RayDoorOverlay;

#define RAY_COLUMN_FLAG_DOOR 0x01u

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

void player_init(PlayerState *player, u16 phase_index);
void player_try_move(PlayerState *player, s16 forward, s16 strafe);
void player_apply_world_push(PlayerState *player, s32 dx, s32 dy);

#endif // __ASSEMBLER__

#endif
