#include "player_controller.h"
#include "fixed_math.h"

#define TURN_STEP 2
#define MOVE_STEP 18

u16 player_controller_update(PlayerState *player) {
    static u16 previous_joy = 0;
    const u16 joy = JOY_readJoypad(JOY_1);
    s16 move = 0;
    s16 strafe = 0;
    u16 result = 0;

    if ((joy & BUTTON_LEFT) != 0) {
        player->angle = (u16)((player->angle - TURN_STEP) & ANGLE_MASK);
        result |= PLAYER_CONTROL_CHANGED;
    }
    if ((joy & BUTTON_RIGHT) != 0) {
        player->angle = (u16)((player->angle + TURN_STEP) & ANGLE_MASK);
        result |= PLAYER_CONTROL_CHANGED;
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
        result |= PLAYER_CONTROL_CHANGED;
    }

    if (((joy & BUTTON_B) != 0) && ((previous_joy & BUTTON_B) == 0)) {
        result |= PLAYER_CONTROL_USE;
    }

    previous_joy = joy;
    return result;
}
