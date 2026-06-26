#include "player_controller.h"
#include "fixed_math.h"

#define TURN_STEP 3
#define MOVE_STEP 30
#define STRAFE_STEP 24

u16 player_controller_update(PlayerState *player, u16 elapsed_frames) {
    static u16 previous_joy = 0;
    const u16 joy = JOY_readJoypad(JOY_1);
    // Scale per-frame steps by the caller-provided simulation cadence. Keep this
    // stable to avoid converting occasional render delays into visible camera jumps.
    const s16 turn = (s16)(TURN_STEP * elapsed_frames);
    const s16 move_step = (s16)(MOVE_STEP * elapsed_frames);
    const s16 strafe_step = (s16)(STRAFE_STEP * elapsed_frames);
    s16 move = 0;
    s16 strafe = 0;
    u16 result = 0;

    if ((joy & BUTTON_LEFT) != 0) {
        player->angle = (u16)((player->angle - turn) & ANGLE_MASK);
        result |= PLAYER_CONTROL_CHANGED;
    }
    if ((joy & BUTTON_RIGHT) != 0) {
        player->angle = (u16)((player->angle + turn) & ANGLE_MASK);
        result |= PLAYER_CONTROL_CHANGED;
    }
    if ((joy & BUTTON_UP) != 0) {
        move += move_step;
    }
    if ((joy & BUTTON_DOWN) != 0) {
        move -= move_step;
    }
    if ((joy & BUTTON_A) != 0) {
        strafe -= strafe_step;
    }
    if ((joy & BUTTON_C) != 0) {
        strafe += strafe_step;
    }

    if ((move != 0) || (strafe != 0)) {
        player_try_move(player, move, strafe);
        result |= PLAYER_CONTROL_CHANGED;
    }

    if (((joy & BUTTON_START) != 0) && ((previous_joy & BUTTON_START) == 0)) {
        result |= PLAYER_CONTROL_USE;
    }

    if (((joy & BUTTON_B) != 0) && ((previous_joy & BUTTON_B) == 0)) {
        result |= PLAYER_CONTROL_FIRE;
    }

    previous_joy = joy;
    return result;
}
