#include "controls.h"

#define CONTROLS_REMAPPABLE (BUTTON_A | BUTTON_B | BUTTON_C | \
                             BUTTON_X | BUTTON_Y | BUTTON_Z)
#define CONTROLS_GROUP_SIZE 3
// FIRE, USE, RUN, PREV WEAPON, NEXT WEAPON, AUTOMAP: the shipped layout.
#define CONTROLS_DEFAULT_BINDINGS \
    { BUTTON_B, BUTTON_C, BUTTON_A, BUTTON_X, BUTTON_Y, BUTTON_Z }

static const u16 DEFAULT_BINDING[CONTROL_ACTION_COUNT] = CONTROLS_DEFAULT_BINDINGS;
// Buttons in glyph-sheet order; row 0 serves the first three actions, row 1 the
// last three.
static const u16 GROUP_BUTTONS[2][CONTROLS_GROUP_SIZE] = {
    { BUTTON_A, BUTTON_B, BUTTON_C },
    { BUTTON_X, BUTTON_Y, BUTTON_Z },
};

static u16 s_binding[CONTROL_ACTION_COUNT] = CONTROLS_DEFAULT_BINDINGS;
// The default layout is the identity translation, so gameplay pays nothing for
// remapping until the player actually changes something.
static bool s_identity = TRUE;

static u16 action_group(ControlAction action) {
    return (action < CONTROL_PREV_WEAPON) ? 0 : 1;
}

static u16 button_slot(u16 group, u16 button) {
    for (u16 slot = 0; slot < CONTROLS_GROUP_SIZE; slot++) {
        if (GROUP_BUTTONS[group][slot] == button) return slot;
    }
    return 0;
}

static void refresh_identity(void) {
    s_identity = TRUE;
    for (u16 i = 0; i < CONTROL_ACTION_COUNT; i++) {
        if (s_binding[i] != DEFAULT_BINDING[i]) s_identity = FALSE;
    }
}

u16 controls_button(ControlAction action) {
    return (action < CONTROL_ACTION_COUNT) ? s_binding[action] : 0;
}

u16 controls_button_glyph(ControlAction action) {
    if (action >= CONTROL_ACTION_COUNT) return 0;
    const u16 group = action_group(action);
    return (u16)(group * CONTROLS_GROUP_SIZE + button_slot(group, s_binding[action]));
}

void controls_cycle(ControlAction action, s16 delta) {
    if (action >= CONTROL_ACTION_COUNT) return;
    const u16 group = action_group(action);
    const u16 first = (u16)(group * CONTROLS_GROUP_SIZE);
    const u16 current = s_binding[action];
    const u16 slot = button_slot(group, current);
    const u16 step = (delta < 0) ? (CONTROLS_GROUP_SIZE - 1) : 1;
    const u16 target = GROUP_BUTTONS[group][(slot + step) % CONTROLS_GROUP_SIZE];

    for (u16 i = first; i < first + CONTROLS_GROUP_SIZE; i++) {
        if (s_binding[i] == target) s_binding[i] = current;
    }
    s_binding[action] = target;
    refresh_identity();
}

void controls_reset_defaults(void) {
    for (u16 i = 0; i < CONTROL_ACTION_COUNT; i++) {
        s_binding[i] = DEFAULT_BINDING[i];
    }
    s_identity = TRUE;
}

u16 controls_to_logical(u16 raw) {
    if (s_identity) return raw;
    u16 logical = (u16)(raw & ~CONTROLS_REMAPPABLE);
    for (u16 i = 0; i < CONTROL_ACTION_COUNT; i++) {
        if ((raw & s_binding[i]) != 0) logical |= DEFAULT_BINDING[i];
    }
    return logical;
}
