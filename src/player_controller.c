#include "player_controller.h"
#include "fixed_math.h"

#define TURN_STEP 3
// Translation uses velocity ramps for inertia instead of instant on/off.
#define MOVE_MAX 30
#define STRAFE_MAX 24
#define MOVE_ACCEL 6  // per frame, ramping up toward the target speed
#define MOVE_DECEL 8  // per frame, ramping down when released or reversing

// Move `vel` toward `target` by at most `accel` (ramp up) or `decel` (ramp down).
static s16 approach_velocity(s16 vel, s16 target, s16 accel, s16 decel) {
    const s16 abs_vel = (s16)((vel < 0) ? -vel : vel);
    const s16 abs_target = (s16)((target < 0) ? -target : target);
    const bool same_dir = ((vel >= 0) == (target >= 0));
    // Ramp up only when building speed in the target's direction; otherwise (stop
    // or reverse) decelerate.
    const bool ramping_up = (target != 0) && same_dir && (abs_vel < abs_target);
    const s16 rate = ramping_up ? accel : decel;

    if (vel < target) {
        vel = (s16)(vel + rate);
        if (vel > target) {
            vel = target;
        }
    } else if (vel > target) {
        vel = (s16)(vel - rate);
        if (vel < target) {
            vel = target;
        }
    }
    return vel;
}

u16 player_controller_update(PlayerState *player, u16 elapsed_frames) {
    static u16 previous_joy = 0;
    static s16 vel_forward = 0;
    static s16 vel_strafe = 0;
    const u16 joy = JOY_readJoypad(JOY_1);
    // Turning stays responsive (instant); scaled by the fixed simulation cadence.
    const s16 turn = (s16)(TURN_STEP * elapsed_frames);
    s16 target_forward = 0;
    s16 target_strafe = 0;
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
        target_forward += MOVE_MAX;
    }
    if ((joy & BUTTON_DOWN) != 0) {
        target_forward -= MOVE_MAX;
    }
    if ((joy & BUTTON_A) != 0) {
        target_strafe -= STRAFE_MAX;
    }
    if ((joy & BUTTON_C) != 0) {
        target_strafe += STRAFE_MAX;
    }

    vel_forward = approach_velocity(vel_forward, target_forward, MOVE_ACCEL, MOVE_DECEL);
    vel_strafe = approach_velocity(vel_strafe, target_strafe, MOVE_ACCEL, MOVE_DECEL);

    if ((vel_forward != 0) || (vel_strafe != 0)) {
        player_try_move(player, vel_forward, vel_strafe);
        // Keep redrawing/moving while velocity decays after the button is released.
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
