#include "bsp_map.h"

#if BSP_USE_HAND_MAP

#include "fixed_math.h" // FX_ONE
#include "generated_assets.h"

// Hand-authored two-room test map (debug fallback for the E1M1 import).
// Two rectangular rooms sharing a vertical middle wall (x = 1024) with a central
// DOOR (y = 896..1152, starts closed); the east room's east wall is the EXIT
// switch. One root BSP node splits west/east into two subsectors.

const BspVertex bsp_vertices[] = {
    {256, 256},   // 0 NW
    {1792, 256},  // 1 NE
    {1792, 1792}, // 2 SE
    {256, 1792},  // 3 SW
    {1024, 256},  // 4 middle top
    {1024, 896},  // 5 doorway top
    {1024, 1152}, // 6 doorway bottom
    {1024, 1792}, // 7 middle bottom
};

const BspSeg bsp_segs[] = {
    // --- West subsector (interior center ~ (640,1024)) ---
    {0, 4, 0, 1, 0, 0, MEGALDOOM_TEX_STARTAN3, BSP_SEG_WALL},
    {4, 5, -1, 0, 0, 0, MEGALDOOM_TEX_SUPPORT2, BSP_SEG_WALL},
    {5, 6, -1, 0, 0, 0, MEGALDOOM_TEX_DOOR3, BSP_SEG_DOOR},
    {6, 7, -1, 0, 0, 0, MEGALDOOM_TEX_SUPPORT2, BSP_SEG_WALL},
    {3, 7, 0, -1, 0, 0, MEGALDOOM_TEX_STARTAN3, BSP_SEG_WALL},
    {0, 3, 1, 0, 0, 0, MEGALDOOM_TEX_BROWN1, BSP_SEG_WALL},
    // --- East subsector (interior center ~ (1408,1024)) ---
    {4, 1, 0, 1, 0, 0, MEGALDOOM_TEX_COMPTILE, BSP_SEG_WALL},
    {1, 2, -1, 0, 0, 0, MEGALDOOM_TEX_SW1STRTN, BSP_SEG_EXIT},
    {7, 2, 0, -1, 0, 0, MEGALDOOM_TEX_COMPTILE, BSP_SEG_WALL},
    {4, 5, 1, 0, 0, 0, MEGALDOOM_TEX_SUPPORT2, BSP_SEG_WALL},
    {5, 6, 1, 0, 0, 0, MEGALDOOM_TEX_DOOR3, BSP_SEG_DOOR},
    {6, 7, 1, 0, 0, 0, MEGALDOOM_TEX_SUPPORT2, BSP_SEG_WALL},
};

const BspSubsector bsp_subsectors[] = {
    {0, 6, 0}, // 0 west
    {6, 6, 1}, // 1 east
};

const BspSectorVisual bsp_sector_visuals[] = {
    {6, 4},
    {11, 6},
};

const BspNode bsp_nodes[] = {
    {1024, 256, 0, 1536,
     {1024, 256, 1792, 1792}, // front/east child bounds
     {256, 256, 1024, 1792},  // back/west child bounds
     (u16)(BSP_CHILD_LEAF_BIT | 1),  // front = east subsector
     (u16)(BSP_CHILD_LEAF_BIT | 0)}, // back  = west subsector
};

const u16 bsp_root_node = 0;
const u16 bsp_seg_count = 12;
const u16 bsp_node_count = 1u;

// Precomputed |bx-ax| + |by-ay| per seg (see bsp_map.h).
const u16 bsp_seg_wall_len[] = {
    768, 640, 256, 640, 768, 1536, 768, 1536, 768, 640, 256, 640,
};

// Conservative hand-map blockmap. Keeping one cell per 256 world units mirrors
// the generated-map contract and allows the same indexed query code in tests.
const s16 bsp_grid_min_x = 256;
const s16 bsp_grid_min_y = 256;
const u16 bsp_grid_width = 7u;
const u16 bsp_grid_height = 7u;
const u16 bsp_grid_cell_offsets[50] = {
    0,2,3,4,8,9,10,12,13,13,13,15,15,15,16,17,17,17,21,21,21,22,23,23,23,
    27,27,27,28,29,29,29,31,31,31,32,33,33,33,35,35,35,36,38,39,40,44,45,46,48,
};
const u16 bsp_grid_seg_indices[48] = {
    0,5,0,0,0,1,6,9,6,6,6,7,5,1,9,7,5,1,2,9,10,7,5,2,3,10,11,7,5,3,11,7,
    5,3,11,7,4,5,4,4,3,4,8,11,8,8,7,8,
};

const s32 bsp_player_start_x = 2 * FX_ONE + (FX_ONE / 2); // 640
const s32 bsp_player_start_y = 4 * FX_ONE;                // 1024, aligned with door
const u16 bsp_player_start_angle = 0;                     // face east toward door

#endif // BSP_USE_HAND_MAP
