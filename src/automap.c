#include "automap.h"
#include "bsp_map.h"

#define AUTOMAP_PAN_PIXELS_PER_VBLANK 2

static u8 automap_fit_scale(void) {
    const s32 span_x = bsp_map_max_x - bsp_map_min_x;
    const s32 span_y = bsp_map_max_y - bsp_map_min_y;
    u8 shift = AUTOMAP_SCALE_MIN_SHIFT;
    // Leave eight pixels around the map and room for the player arrow.
    while (shift < AUTOMAP_SCALE_MAX_SHIFT &&
           (((span_x >> shift) > (RAY_VIEW_COLS - 16)) ||
            ((span_y >> shift) > (RAY_VIEW_ROWS - 16)))) {
        shift++;
    }
    return shift;
}

static void automap_toggle_full(AutomapState *state, const PlayerState *player) {
    if (!state->full_view) {
        state->saved_center_x = state->follow ? player->x : state->center_x;
        state->saved_center_y = state->follow ? player->y : state->center_y;
        state->saved_scale_shift = state->scale_shift;
        state->center_x = (bsp_map_min_x + bsp_map_max_x) / 2;
        state->center_y = (bsp_map_min_y + bsp_map_max_y) / 2;
        state->scale_shift = automap_fit_scale();
        state->full_view = TRUE;
    } else {
        state->center_x = state->saved_center_x;
        state->center_y = state->saved_center_y;
        state->scale_shift = state->saved_scale_shift;
        state->full_view = FALSE;
    }
}

static bool automap_zoom(AutomapState *state, s16 direction,
                         const PlayerState *player) {
    const u8 old = state->scale_shift;
    if (state->full_view) {
        automap_toggle_full(state, player);
    }
    if (direction > 0 && state->scale_shift > AUTOMAP_SCALE_MIN_SHIFT) {
        state->scale_shift--;
    } else if (direction < 0 && state->scale_shift < AUTOMAP_SCALE_MAX_SHIFT) {
        state->scale_shift++;
    }
    return (bool)(old != state->scale_shift);
}

void automap_reset(AutomapState *state, const PlayerState *player) {
    state->center_x = player->x;
    state->center_y = player->y;
    state->saved_center_x = player->x;
    state->saved_center_y = player->y;
    state->scale_shift = 4;
    state->saved_scale_shift = 4;
    state->three_button_zoom_dir = 1;
    state->active = FALSE;
    state->follow = TRUE;
    state->grid = FALSE;
    state->full_view = FALSE;
}

void automap_close(AutomapState *state) {
    state->active = FALSE;
}

AutomapInput automap_update_input(AutomapState *state, const PlayerState *player,
                                   u16 joy, u16 pressed, bool six_button_pad,
                                   u16 elapsed_frames) {
    AutomapInput result = {0, 0};
    const u16 chord_state = (u16)(joy | pressed);
    const bool three_toggle = (bool)(!six_button_pad &&
        ((chord_state & (BUTTON_C | BUTTON_START)) == (BUTTON_C | BUTTON_START)) &&
        ((pressed & (BUTTON_C | BUTTON_START)) != 0));
    const bool six_toggle = (bool)(six_button_pad && (pressed & BUTTON_Z));

    if (three_toggle || six_toggle) {
        state->active = (bool)!state->active;
        state->center_x = player->x;
        state->center_y = player->y;
        state->full_view = FALSE;
        result.flags |= AUTOMAP_INPUT_REDRAW | AUTOMAP_INPUT_TOGGLED;
        result.consumed_buttons = three_toggle ? (BUTTON_C | BUTTON_START) : BUTTON_Z;
        return result;
    }
    if (!state->active) return result;

    if (!six_button_pad &&
        ((chord_state & (BUTTON_A | BUTTON_START)) == (BUTTON_A | BUTTON_START)) &&
        ((pressed & (BUTTON_A | BUTTON_START)) != 0)) {
        automap_toggle_full(state, player);
        result.flags = AUTOMAP_INPUT_REDRAW;
        result.consumed_buttons = BUTTON_A | BUTTON_START;
        return result;
    }

    if (six_button_pad) {
        if (pressed & BUTTON_X) {
            state->follow = (bool)!state->follow;
            state->full_view = FALSE;
            state->center_x = player->x;
            state->center_y = player->y;
            result.flags |= AUTOMAP_INPUT_REDRAW;
            result.consumed_buttons |= BUTTON_X;
        }
        if (pressed & BUTTON_Y) {
            automap_toggle_full(state, player);
            result.flags |= AUTOMAP_INPUT_REDRAW;
            result.consumed_buttons |= BUTTON_Y;
        }
        if (pressed & BUTTON_C) {
            state->grid = (bool)!state->grid;
            result.flags |= AUTOMAP_INPUT_REDRAW;
            result.consumed_buttons |= BUTTON_C;
        }
        if (pressed & BUTTON_A) {
            if (automap_zoom(state, 1, player)) {
                result.flags |= AUTOMAP_INPUT_REDRAW;
            }
            result.consumed_buttons |= BUTTON_A;
        }
        if (pressed & BUTTON_B) {
            if (automap_zoom(state, -1, player)) {
                result.flags |= AUTOMAP_INPUT_REDRAW;
            }
            result.consumed_buttons |= BUTTON_B;
        }
    } else {
        if (pressed & BUTTON_B) {
            state->follow = (bool)!state->follow;
            state->full_view = FALSE;
            state->center_x = player->x;
            state->center_y = player->y;
            result.flags |= AUTOMAP_INPUT_REDRAW;
            result.consumed_buttons |= BUTTON_B;
        }
        if (pressed & BUTTON_C) {
            state->grid = (bool)!state->grid;
            result.flags |= AUTOMAP_INPUT_REDRAW;
            result.consumed_buttons |= BUTTON_C;
        }
        if (pressed & BUTTON_A) {
            if (automap_zoom(state, state->three_button_zoom_dir, player)) {
                result.flags |= AUTOMAP_INPUT_REDRAW;
            }
            result.consumed_buttons |= BUTTON_A;
            state->three_button_zoom_dir = (s8)-state->three_button_zoom_dir;
        }
    }

    if (state->follow && !state->full_view) {
        if (state->center_x != player->x || state->center_y != player->y) {
            state->center_x = player->x;
            state->center_y = player->y;
            result.flags |= AUTOMAP_INPUT_REDRAW;
        }
    } else {
        const s32 step = (s32)(AUTOMAP_PAN_PIXELS_PER_VBLANK * elapsed_frames)
                         << state->scale_shift;
        const s32 old_x = state->center_x;
        const s32 old_y = state->center_y;
        if ((joy & BUTTON_LEFT) && !(joy & BUTTON_RIGHT)) state->center_x -= step;
        if ((joy & BUTTON_RIGHT) && !(joy & BUTTON_LEFT)) state->center_x += step;
        if ((joy & BUTTON_UP) && !(joy & BUTTON_DOWN)) state->center_y -= step;
        if ((joy & BUTTON_DOWN) && !(joy & BUTTON_UP)) state->center_y += step;
        if (state->center_x != old_x || state->center_y != old_y) {
            state->full_view = FALSE;
            result.flags |= AUTOMAP_INPUT_REDRAW;
        }
    }
    return result;
}
