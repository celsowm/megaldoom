#ifndef MEGALDOOM_RAYCAST_H
#define MEGALDOOM_RAYCAST_H

#include <genesis.h>

#define RAY_VIEW_COLS 160
// Horizontal render granularity: cast/sample one wall column every N pixels and
// duplicate across the gap. 1 = full 1px detail (heaviest), 2 = 2px (~80 cols),
// 4 = 4px (~40 cols, original cost). Must divide 8. Lower = sharper but slower.
#define RAY_COL_STRIDE 4

// Wall/door/switch textures are WALL_TEX_DIM x WALL_TEX_DIM palette-index texels
// (see generated_assets.h). This is the single source of truth for the wall
// texture size; every consumer derives from it:
//   - RayColumn.tex_x and the vertical sampler wrap with WALL_TEX_DIM_MASK
//     (so WALL_TEX_DIM MUST stay a power of two),
//   - bsp_render.c derives each seg's horizontal ushift from it,
//   - renderer.c builds the x-WALL_TEX_DIM vertical sampling table from it,
//   - the generated arrays are declared [WALL_TEX_DIM][WALL_TEX_DIM].
// To change it: set this macro AND $WallDim in tools/convert-freedoom-assets.ps1
// to the same power-of-two value, then regenerate. A mismatch fails to compile
// (wrong number of array initializers), so the two can't silently drift.
#define WALL_TEX_DIM 32
#define WALL_TEX_DIM_MASK (WALL_TEX_DIM - 1)

typedef struct {
    s32 x;
    s32 y;
    u16 angle;
} PlayerState;

typedef enum {
    RAY_TEXTURE_WALL = 0,
    RAY_TEXTURE_DOOR = 1,
    RAY_TEXTURE_LOCKED_DOOR = 2,
    RAY_TEXTURE_EXIT = 3,
    RAY_TEXTURE_WALL_BROWN = 4,
    RAY_TEXTURE_WALL_GRAY = 5,
    RAY_TEXTURE_WALL_METAL = 6,
    RAY_TEXTURE_WALL_BRICK = 7,
    RAY_TEXTURE_WALL_TECH = 8
} RayTextureId;

// Number of distinct wall textures (RayTextureId 0..8); sizes the WALL_TEX table,
// the pre-shaded copies and the source-width table, all keyed by texture_id.
#define RAY_TEXTURE_COUNT 9

typedef struct {
    u16 height;
    u16 depth;
    u8 tex_x;
    u8 texture_id;
    u8 shade;
} RayColumn;

void player_init(PlayerState *player, u16 phase_index);
void player_try_move(PlayerState *player, s16 forward, s16 strafe);
void player_apply_world_push(PlayerState *player, s32 dx, s32 dy);

#endif
