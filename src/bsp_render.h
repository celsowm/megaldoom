#ifndef MEGALDOOM_BSP_RENDER_H
#define MEGALDOOM_BSP_RENDER_H

#include "raycast.h" // PlayerState, RayColumn, RAY_VIEW_COLS

// Lean BSP frame filler. Same signature and output contract as
// raycast_cast_frame(): it fills columns[RAY_VIEW_COLS] with wall
// height/depth/tex_x/texture_id/shade in the SAME units the raycaster uses, so
// the entire downstream tile-packing drawer (renderer_scene.c) and billboard
// depth occlusion are reused unchanged.
void bsp_init(void);
void bsp_cast_frame(const PlayerState *player, RayColumn *columns);

#endif
