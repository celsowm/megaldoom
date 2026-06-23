#include "player_controller.h"
#include "fixed_math.h"

#define TURN_STEP 2
#define MOVE_STEP 18

bool player_controller_update(PlayerState *player) {
    const u16 joy = JOY_readJoypad(JOY_1);
    s16 move = 0;
    s16 strafe = 0;
    bool changed = FALSE;

    if ((joy & BUTTON_LEFT) != 0) {
        player->angle = (u16)((player->angle - TURN_STEP) & ANGLE_MASK);
        changed = TRUE;
    }
    if ((joy & BUTTON_RIGHT) != 0) {
        player->angle = (u16)((player->angle + TURN_STEP) & ANGLE_MASK);
        changed = TRUE;
    }
    if ((joy & BUTTON_UP) != 0) {
        move += MOVE_STEP;
    }
    if ((joy & BUTTON_DOWN) != 0) {
        move -= MOVE_STEP;
    }
    if ((joy & BUTTON_A) != 0) {
        strafe -= MOVE_STEP;
    }
    if ((joy & BUTTON_C) != 0) {
        strafe += MOVE_STEP;
    }

    if ((move != 0) || (strafe != 0)) {
        player_try_move(player, move, strafe);
        changed = TRUE;
    }

    return changed;
}
