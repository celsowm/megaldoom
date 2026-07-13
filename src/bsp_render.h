#ifndef MEGALDOOM_BSP_RENDER_H
#define MEGALDOOM_BSP_RENDER_H

#include "raycast.h" // PlayerState, RayColumn, RAY_VIEW_COLS

// Lean flat-BSP frame filler. It writes columns[RAY_VIEW_COLS] with the wall
// height, depth, texture coordinate, texture ID and shade consumed by the
// single downstream tile packer (renderer_scene.c) and billboard occlusion.
void bsp_init(void);
void bsp_cast_frame(const PlayerState *player, RayColumn *columns, RaySceneColors *scene_colors);
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
#endif

#endif
