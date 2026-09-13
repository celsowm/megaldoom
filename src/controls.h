#ifndef MEGALDOOM_CONTROLS_H
#define MEGALDOOM_CONTROLS_H

#include <genesis.h>

// Remappable gameplay buttons, chosen from OPTIONS > CONTROLS. The layout is
// kept for the session only (there is no SRAM), like the other OPTIONS rows.
//
// FIRE/USE/RUN share the A/B/C group and PREV/NEXT/AUTOMAP the X/Y/Z group.
// A layout is always a permutation inside each group: changing one action's
// button swaps it with the action that held that button, so no action can be
// left unbound or share a button. The order below is also the CONTROLS menu row
// order.
typedef enum {
    CONTROL_FIRE = 0,
    CONTROL_USE,
    CONTROL_RUN,
    CONTROL_PREV_WEAPON,
    CONTROL_NEXT_WEAPON,
    CONTROL_AUTOMAP,
    CONTROL_ACTION_COUNT
} ControlAction;

// Physical button mask currently bound to `action`.
u16 controls_button(ControlAction action);
// Index of that button in the A B C X Y Z order (the menu glyph sheet).
u16 controls_button_glyph(ControlAction action);
// Move `action` to the next (+1) or previous (-1) button of its group, swapping
// with whichever action held it.
void controls_cycle(ControlAction action, s16 delta);
void controls_reset_defaults(void);

// Translate a raw pad read into the DEFAULT layout's bits: the physical button
// bound to FIRE comes out as BUTTON_B, USE as BUTTON_C, RUN as BUTTON_A, PREV as
// BUTTON_X, NEXT as BUTTON_Y and AUTOMAP as BUTTON_Z. The D-pad, START and MODE
// pass through, so gameplay code keeps reading its original button names.
u16 controls_to_logical(u16 raw);

#endif
