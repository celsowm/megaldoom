#ifndef MEGALDOOM_BILLBOARD_H
#define MEGALDOOM_BILLBOARD_H

#include <genesis.h>
#include "raycast.h"

#define BILLBOARD_MAX_SPANS 24

typedef enum {
    BILLBOARD_VISUAL_BONUS = 0,
    BILLBOARD_VISUAL_KEY = 1,
    BILLBOARD_VISUAL_DECOR = 2
} BillboardVisualId;

typedef struct {
    s16 column;
    s16 top;
    s16 bottom;
    u16 depth;
    u8 tex_x;
    u8 visual_id;
} BillboardSpan;

void billboard_init(void);
bool billboard_collect_near(s32 x, s32 y);
u16 billboard_get_collected_count(void);
u16 billboard_project_scene(const PlayerState *player, BillboardSpan *spans, u16 max_spans);

#endif
