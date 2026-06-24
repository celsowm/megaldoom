#ifndef MEGALDOOM_RENDERER_H
#define MEGALDOOM_RENDERER_H

#include <genesis.h>
#include "raycast.h"

void renderer_init(void);
void renderer_draw_static_screen(void);
void renderer_render_scene(const RayColumn *columns, const PlayerState *player);
void renderer_draw_frame_counter(u32 frame);
void renderer_draw_pickup_counter(u16 count);

#endif
