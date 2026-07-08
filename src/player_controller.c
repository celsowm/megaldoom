#include "player_controller.h"
#include "fixed_math.h"

// Turning ramps from a small first-tap step up to TURN_MAX while a turn button is
// held. A quick tap now nudges the view by TURN_MIN (fine aiming) instead of
// jumping the full step, while sustained turning still reaches the same top speed
// as before (TURN_MAX * elapsed_frames == the old TURN_STEP * elapsed_frames = 6).
// Values are per-vsync and scaled by elapsed_frames, matching the locked cadence.
#define TURN_MIN 1
#define TURN_MAX 3
#define TURN_ACCEL 1
// Translation uses velocity ramps for inertia instead of instant on/off.
// Values doubled from the original 60fps-tuned speeds (30/24/6/8) because the
// locked cadence is now TARGET_FRAME_VSYNCS=2 (30fps): player_try_move runs once
// per iteration regardless of vsync count, so half the iterations/sec means half
// the real-world speed unless compensated here.
#define MOVE_MAX 120
#define STRAFE_MAX 96
#define MOVE_ACCEL 24 // per frame, ramping up toward the target speed
#define MOVE_DECEL 32 // per frame, ramping down when released or reversing

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
    static s16 turn_speed = 0; // per-vsync turn rate, ramps while a turn key is held
    const u16 joy = JOY_readJoypad(JOY_1);
    const bool turning_left = ((joy & BUTTON_LEFT) != 0);
    const bool turning_right = ((joy & BUTTON_RIGHT) != 0);
    s16 target_forward = 0;
    s16 target_strafe = 0;
    u16 result = 0;

    // Ramp the turn rate: a fresh tap starts at TURN_MIN for fine aim, then
    // accelerates toward TURN_MAX while held. Reset the moment turning stops so the
    // next tap is precise again.
    if (turning_left != turning_right) {
        if (turn_speed < TURN_MIN) {
            turn_speed = TURN_MIN;
        } else if (turn_speed < TURN_MAX) {
            turn_speed = (s16)(turn_speed + TURN_ACCEL);
            if (turn_speed > TURN_MAX) {
                turn_speed = TURN_MAX;
            }
        }
    } else {
        turn_speed = 0;
    }

    const s16 turn = (s16)(turn_speed * elapsed_frames);

    if (turning_left) {
        player->angle = (u16)((player->angle - turn) & ANGLE_MASK);
        result |= PLAYER_CONTROL_CHANGED;
    }
    if (turning_right) {
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
