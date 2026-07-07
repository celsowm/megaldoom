#include "bsp_map.h"

#if BSP_USE_HAND_MAP

#include "fixed_math.h" // FX_ONE
#include "raycast.h"    // RayTextureId

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
    {0, 4, 0, 1, RAY_TEXTURE_WALL_GRAY, BSP_SEG_WALL},
    {4, 5, -1, 0, RAY_TEXTURE_WALL_METAL, BSP_SEG_WALL},
    {5, 6, -1, 0, RAY_TEXTURE_DOOR, BSP_SEG_DOOR},
    {6, 7, -1, 0, RAY_TEXTURE_WALL_METAL, BSP_SEG_WALL},
    {3, 7, 0, -1, RAY_TEXTURE_WALL_GRAY, BSP_SEG_WALL},
    {0, 3, 1, 0, RAY_TEXTURE_WALL_BRICK, BSP_SEG_WALL},
    // --- East subsector (interior center ~ (1408,1024)) ---
    {4, 1, 0, 1, RAY_TEXTURE_WALL_TECH, BSP_SEG_WALL},
    {1, 2, -1, 0, RAY_TEXTURE_EXIT, BSP_SEG_EXIT},
    {7, 2, 0, -1, RAY_TEXTURE_WALL_TECH, BSP_SEG_WALL},
    {4, 5, 1, 0, RAY_TEXTURE_WALL_METAL, BSP_SEG_WALL},
    {5, 6, 1, 0, RAY_TEXTURE_DOOR, BSP_SEG_DOOR},
    {6, 7, 1, 0, RAY_TEXTURE_WALL_METAL, BSP_SEG_WALL},
};

const BspSubsector bsp_subsectors[] = {
    {0, 6}, // 0 west
    {6, 6}, // 1 east
};

const BspNode bsp_nodes[] = {
    {1024, 256, 0, 1536,
     (u16)(BSP_CHILD_LEAF_BIT | 1),  // front = east subsector
     (u16)(BSP_CHILD_LEAF_BIT | 0)}, // back  = west subsector
};

const u16 bsp_root_node = 0;
const u16 bsp_seg_count = 12;

const s32 bsp_player_start_x = 2 * FX_ONE + (FX_ONE / 2); // 640
const s32 bsp_player_start_y = 4 * FX_ONE;                // 1024, aligned with door
const u16 bsp_player_start_angle = 0;                     // face east toward door

#endif // BSP_USE_HAND_MAP
