#ifndef MEGALDOOM_RAYCAST_H
#define MEGALDOOM_RAYCAST_H

#include <genesis.h>

// View geometry — single source of truth for the render viewport. The raycaster,
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
#define RAY_PROJ_X 180
#define RAY_PROJ_Y RAY_VIEW_ROWS
#define RAY_WORLD_WALL_HEIGHT 256
#define RAY_CAMERA_HEIGHT (RAY_WORLD_WALL_HEIGHT / 2)
// Horizontal render granularity: cast/sample one wall column every N pixels and
// duplicate across the gap. 1 = full 1px detail (heaviest), 2 = 2px (~80 cols),
// 4 = 4px (~40 cols, original cost). Must divide 8. Lower = sharper but slower.
#define RAY_COL_STRIDE 4
#define RAY_SAMPLE_COLS (RAY_VIEW_COLS / RAY_COL_STRIDE)
#define PLAYER_HEIGHT 56
#define PLAYER_EYE_HEIGHT 41
#define PLAYER_MAX_STEP 24

// Wall/door/switch textures are WALL_TEX_DIM x WALL_TEX_DIM palette-index texels
// (see generated_assets.h). This is the single source of truth for the wall
// texture size; every consumer derives from it:
//   - RayColumn.tex_x and the vertical sampler wrap with WALL_TEX_DIM_MASK
//     (so WALL_TEX_DIM MUST stay a power of two),
//   - bsp_render.c derives each seg's horizontal ushift from it,
//   - renderer.c builds the x-WALL_TEX_DIM vertical sampling table from it,
//   - the generated arrays are declared [WALL_TEX_DIM][WALL_TEX_DIM].
// Exact texture IDs and source dimensions are generated from the active WAD map.
#define WALL_TEX_DIM 32
#define WALL_TEX_DIM_MASK (WALL_TEX_DIM - 1)

typedef struct {
    s32 x;
    s32 y;
    s16 z;
    s16 view_z;
    u16 sector_id;
    u16 angle;
} PlayerState;

typedef struct {
    u8 ceiling_color;
    u8 floor_color;
} RaySceneColors;

typedef struct {
    u16 height;
    u16 depth;
    u8 tex_x;
    u8 tex_y;
    u8 texture_id;
    u8 shade;
} RayColumn;

void player_init(PlayerState *player, u16 phase_index);
void player_try_move(PlayerState *player, s16 forward, s16 strafe);
void player_apply_world_push(PlayerState *player, s32 dx, s32 dy);

#endif
