#ifndef MEGALDOOM_RENDERER_H
#define MEGALDOOM_RENDERER_H

#include <genesis.h>
#include "raycast.h"

void renderer_init(void);
void renderer_draw_static_screen(void);
void renderer_render_scene(const RayColumn *columns, u16 angle);
void renderer_draw_frame_counter(u32 frame);

#endif
