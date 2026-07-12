#ifndef MEGALDOOM_BSP_RENDER_H
#define MEGALDOOM_BSP_RENDER_H

#include "raycast.h" // PlayerState, RayColumn, RAY_VIEW_COLS

// Lean BSP frame filler. Same signature and output contract as
// raycast_cast_frame(): it fills columns[RAY_VIEW_COLS] with wall
// height/depth/tex_x/texture_id/shade in the SAME units the raycaster uses, so
// the entire downstream tile-packing drawer (renderer_scene.c) and billboard
// depth occlusion are reused unchanged.
void bsp_init(void);
void bsp_cast_frame(const PlayerState *player, RayColumn *columns, RaySceneColors *scene_colors);
#if BSP_SECTOR_RENDERER
void bsp_sector_cast_frame(const PlayerState *player);
const u16 *bsp_sector_depth_block(u16 sample_x, u16 tile_y);
u16 bsp_sector_depth_at(u16 sample_x, u16 y);
#endif
void bsp_invalidate_node_cache(void);

#if DEBUG_PERF
// Temporary BSP traversal instrumentation for the DEBUG_PERF overlay.
u16 bsp_get_debug_nodes_visited(void);
u16 bsp_get_debug_boxes_rejected_cheap(void);
u16 bsp_get_debug_boxes_projected(void);
u16 bsp_get_debug_near_fallbacks(void);
u16 bsp_get_debug_segments_tested(void);
u16 bsp_get_debug_segments_drawn(void);
u32 bsp_get_debug_side_cache_subticks(void);
#if BSP_SECTOR_RENDERER
u32 bsp_sector_get_debug_flat_subticks(void);
u32 bsp_sector_get_debug_wall_subticks(void);
u32 bsp_sector_get_debug_floor_subticks(void);
u32 bsp_sector_get_debug_transform_subticks(void);
u32 bsp_sector_get_debug_setup_subticks(void);
u32 bsp_sector_get_debug_raster_subticks(void);
#endif
#endif

#endif
