#include "renderer_redraw.h"

void renderer_redraw_init(RendererRedrawState *state) {
    state->base_dirty = TRUE;
    state->overlay_dirty = TRUE;
#if DEBUG_PERF
    state->reasons = RENDERER_REDRAW_BASE;
#endif
}

void renderer_redraw_request_base(RendererRedrawState *state, u16 reason) {
    state->base_dirty = TRUE;
    state->overlay_dirty = TRUE;
#if DEBUG_PERF
    state->reasons |= reason;
#else
    (void)reason;
#endif
}

void renderer_redraw_request_overlay(RendererRedrawState *state, u16 reason) {
    state->overlay_dirty = TRUE;
#if DEBUG_PERF
    state->reasons |= reason;
#else
    (void)reason;
#endif
}

bool renderer_redraw_is_pending(const RendererRedrawState *state) {
    return state->base_dirty || state->overlay_dirty;
}

bool renderer_redraw_base_is_dirty(const RendererRedrawState *state) {
    return state->base_dirty;
}

u16 renderer_redraw_reasons(const RendererRedrawState *state) {
#if DEBUG_PERF
    return state->reasons;
#else
    (void)state;
    return 0;
#endif
}

void renderer_redraw_consume(RendererRedrawState *state) {
    state->base_dirty = FALSE;
    state->overlay_dirty = FALSE;
#if DEBUG_PERF
    state->reasons = 0;
#endif
}
