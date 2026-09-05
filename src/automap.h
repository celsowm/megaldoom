#ifndef MEGALDOOM_AUTOMAP_H
#define MEGALDOOM_AUTOMAP_H

#include <genesis.h>
#include "raycast.h"

#define AUTOMAP_SCALE_MIN_SHIFT 2
#define AUTOMAP_SCALE_MAX_SHIFT 6

#define AUTOMAP_INPUT_REDRAW       0x0001u
#define AUTOMAP_INPUT_TOGGLED      0x0002u

// The automap receives raw edge events before gameplay does.  It reports
// exactly which buttons it claimed so a map chord cannot turn into a gameplay
// action on the same frame (notably C is both USE and an automap control).
typedef struct {
    u16 flags;
    u16 consumed_buttons;
} AutomapInput;

typedef struct {
    s32 center_x;
    s32 center_y;
    s32 saved_center_x;
    s32 saved_center_y;
    u8 scale_shift;
    u8 saved_scale_shift;
    s8 three_button_zoom_dir;
    bool active;
    bool follow;
    bool grid;
    bool full_view;
} AutomapState;

void automap_reset(AutomapState *state, const PlayerState *player);
AutomapInput automap_update_input(AutomapState *state, const PlayerState *player,
                                   u16 joy, u16 pressed, bool six_button_pad,
                                   u16 elapsed_frames);
void automap_close(AutomapState *state);

#endif
