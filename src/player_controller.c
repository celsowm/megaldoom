#include "player_controller.h"
#include "fixed_math.h"

// Turning uses a fixed-point ramp. At the locked 30fps cadence this keeps the
// first visible tap finer than before (1 angle unit instead of 2), while held
// turns ramp quickly to a higher top speed so the camera feels less stiff.
#define TURN_FP_SHIFT 8
#define TURN_FP_ONE (1 << TURN_FP_SHIFT)
#define TURN_MIN_FP (TURN_FP_ONE / 2)
#define TURN_MAX_FP (4 * TURN_FP_ONE)
#define TURN_ACCEL_FP ((3 * TURN_FP_ONE) / 2)
// Translation uses velocity ramps for inertia instead of instant on/off.
// Values doubled from the original 60fps-tuned speeds (30/24/6/8) because the
// locked cadence is now TARGET_FRAME_VSYNCS=2 (30fps): player_try_move runs once
// per iteration regardless of vsync count, so half the iterations/sec means half
// the real-world speed unless compensated here.
#define MOVE_MAX 120
#define STRAFE_MAX 96
#define MOVE_ACCEL 40 // per frame, ramping up toward the target speed
#define MOVE_DECEL 64 // per frame, ramping down when released or reversing

static u16 s_previous_joy = 0;
static s16 s_vel_forward = 0;
static s16 s_vel_strafe = 0;
static s16 s_turn_speed_fp = 0; // per-vsync fixed-point turn rate
static s16 s_turn_remainder_fp = 0;
static s16 s_turn_dir = 0;

// Move `vel` toward `target` by at most `accel` (ramp up) or `decel` (ramp down).
static s16 approach_velocity(s16 vel, s16 target, s16 accel, s16 decel) {
    const s16 abs_vel = (s16)((vel < 0) ? -vel : vel);
    const s16 abs_target = (s16)((target < 0) ? -target : target);
    const bool same_dir = (vel == 0) || ((vel < 0) == (target < 0));
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

void player_controller_reset(void) {
    s_previous_joy = 0;
    s_vel_forward = 0;
    s_vel_strafe = 0;
    s_turn_speed_fp = 0;
    s_turn_remainder_fp = 0;
    s_turn_dir = 0;
}

u16 player_controller_update(PlayerState *player, u16 elapsed_frames) {
    const u16 joy = JOY_readJoypad(JOY_1);
    const bool turning_left = ((joy & BUTTON_LEFT) != 0);
    const bool turning_right = ((joy & BUTTON_RIGHT) != 0);
    const s16 desired_turn = (turning_right && !turning_left) ? 1 : ((turning_left && !turning_right) ? -1 : 0);
    s16 target_forward = 0;
    s16 target_strafe = 0;
    u16 result = 0;

    // Ramp the turn rate: a fresh tap starts precise, then accelerates while held.
    // Opposite D-pad directions cancel instead of producing a no-op redraw.
    if (desired_turn != 0) {
        if (desired_turn != s_turn_dir) {
            s_turn_speed_fp = TURN_MIN_FP;
            s_turn_remainder_fp = 0;
        } else if (s_turn_speed_fp < TURN_MAX_FP) {
            s_turn_speed_fp = (s16)(s_turn_speed_fp + TURN_ACCEL_FP);
            if (s_turn_speed_fp > TURN_MAX_FP) {
                s_turn_speed_fp = TURN_MAX_FP;
            }
        }

        const s32 delta_fp = ((s32)s_turn_speed_fp * elapsed_frames) + s_turn_remainder_fp;
        const s16 turn = (s16)(delta_fp >> TURN_FP_SHIFT);

        s_turn_remainder_fp = (s16)(delta_fp & (TURN_FP_ONE - 1));

        if (turn != 0) {
            player->angle = (u16)((player->angle + (desired_turn * turn)) & ANGLE_MASK);
            result |= PLAYER_CONTROL_CHANGED;
        }
    } else {
        s_turn_speed_fp = 0;
        s_turn_remainder_fp = 0;
    }
    s_turn_dir = desired_turn;

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

    s_vel_forward = approach_velocity(s_vel_forward, target_forward, MOVE_ACCEL, MOVE_DECEL);
    s_vel_strafe = approach_velocity(s_vel_strafe, target_strafe, MOVE_ACCEL, MOVE_DECEL);

    if ((s_vel_forward != 0) || (s_vel_strafe != 0)) {
        player_try_move(player, s_vel_forward, s_vel_strafe);
        // Keep redrawing/moving while velocity decays after the button is released.
        result |= PLAYER_CONTROL_CHANGED;
    }

    if (((joy & BUTTON_START) != 0) && ((s_previous_joy & BUTTON_START) == 0)) {
        result |= PLAYER_CONTROL_USE;
    }

    if (((joy & BUTTON_B) != 0) && ((s_previous_joy & BUTTON_B) == 0)) {
        result |= PLAYER_CONTROL_FIRE;
    }

    s_previous_joy = joy;
    return result;
}
