#ifndef MEGALDOOM_RENDERER_REDRAW_H
#define MEGALDOOM_RENDERER_REDRAW_H

#include <genesis.h>

enum {
    RENDERER_REDRAW_BASE = 1u << 0,
    RENDERER_REDRAW_ENEMY_MOVE = 1u << 1,
    RENDERER_REDRAW_ENEMY_POSE = 1u << 2,
    RENDERER_REDRAW_EFFECT = 1u << 3,
    RENDERER_REDRAW_WEAPON = 1u << 4,
    RENDERER_REDRAW_DAMAGE = 1u << 5,
    RENDERER_REDRAW_BARREL = 1u << 6,
    RENDERER_REDRAW_OTHER = 1u << 7
};

typedef struct {
    bool base_dirty;
    bool overlay_dirty;
#if DEBUG_PERF
    u16 reasons;
#endif
} RendererRedrawState;

void renderer_redraw_init(RendererRedrawState *state);
void renderer_redraw_request_base(RendererRedrawState *state, u16 reason);
void renderer_redraw_request_overlay(RendererRedrawState *state, u16 reason);
bool renderer_redraw_is_pending(const RendererRedrawState *state);
bool renderer_redraw_base_is_dirty(const RendererRedrawState *state);
u16 renderer_redraw_reasons(const RendererRedrawState *state);
void renderer_redraw_consume(RendererRedrawState *state);

#endif
