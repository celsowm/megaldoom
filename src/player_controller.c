#include "player_controller.h"
#include "fixed_math.h"

// Turning uses a fixed-point ramp that is scaled by the real elapsed-vblank count
// (see player_controller_update), so the angular velocity per real second is
// frame-rate-independent. The constants below express a per-vblank rate; at the
// locked 20fps cadence (TARGET_FRAME_VSYNCS=3) a held turn applies speed*3 each
// iteration, and at 30fps it would apply speed*2 — both yield the same degrees/sec
// (speed * 60 / 256 angle-units per second), so a steady tap stays fine (1 angle
// unit on the first visible step) while held turns ramp to a snappy top speed.
#define TURN_FP_SHIFT 8
#define TURN_FP_ONE (1 << TURN_FP_SHIFT)
#define TURN_MIN_FP (TURN_FP_ONE / 2)
#define TURN_MAX_FP (4 * TURN_FP_ONE)
#define TURN_ACCEL_FP ((3 * TURN_FP_ONE) / 2)
// Translation uses velocity ramps for inertia instead of instant on/off. These are
// per-iteration values: player_try_move runs once per main-loop iteration regardless
// of vsync count, so fewer iterations/sec means less real-world speed unless
// compensated. The cadence follows TARGET_FRAME_VSYNCS (currently 2 = 30 iterations/sec,
// 2/3 of the old 30fps), so the magnitudes are scaled by 3/2 relative to the 30fps
// baseline to preserve the same real-world walk/strafe speed. player_try_move
// sub-steps the displacement (PLAYER_MOVE_SUBSTEP), so the larger per-iteration step
// cannot tunnel through walls.
#define MOVE_MAX 180
#define STRAFE_MAX 144
#define MOVE_ACCEL 60 // per iteration, ramping up toward the target speed
#define MOVE_DECEL 96 // per iteration, ramping down when released or reversing
#define RUN_SPEED_NUMERATOR 3
#define RUN_SPEED_DENOMINATOR 2
#define RUN_SPEED(value) (((value) * RUN_SPEED_NUMERATOR) / RUN_SPEED_DENOMINATOR)

#define THREE_BUTTON_AUTOMAP_CHORD (BUTTON_A | BUTTON_B | BUTTON_C)

static u16 s_previous_joy = 0;
static bool s_three_button_map_chord_active = FALSE;
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
    s_three_button_map_chord_active = FALSE;
    s_vel_forward = 0;
    s_vel_strafe = 0;
    s_turn_speed_fp = 0;
    s_turn_remainder_fp = 0;
    s_turn_dir = 0;
}

u16 player_controller_update(PlayerState *player, u16 elapsed_frames) {
    const u16 joy = JOY_readJoypad(JOY_1);
    const bool six_button_pad = (JOY_getJoypadType(JOY_1) == JOY_TYPE_PAD6);
    const bool three_button_map_chord =
        !six_button_pad && ((joy & THREE_BUTTON_AUTOMAP_CHORD) == THREE_BUTTON_AUTOMAP_CHORD);
    const bool strafing = ((joy & BUTTON_C) != 0) &&
                          ((joy & (BUTTON_LEFT | BUTTON_RIGHT)) != 0);
    // C modifies lateral D-pad input into strafe, so it must suppress turn and
    // use for the whole chord rather than leaking an action while strafing.
    const bool turning_left = !strafing && ((joy & BUTTON_LEFT) != 0);
    const bool turning_right = !strafing && ((joy & BUTTON_RIGHT) != 0);
    const bool running = ((joy & BUTTON_A) != 0) && !three_button_map_chord;
    const s16 desired_turn = (turning_right && !turning_left) ? 1 : ((turning_left && !turning_right) ? -1 : 0);
    const s16 move_max = running ? RUN_SPEED(MOVE_MAX) : MOVE_MAX;
    const s16 strafe_max = running ? RUN_SPEED(STRAFE_MAX) : STRAFE_MAX;
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

    if (!three_button_map_chord && ((joy & BUTTON_UP) != 0)) {
        target_forward += move_max;
    }
    if (!three_button_map_chord && ((joy & BUTTON_DOWN) != 0)) {
        target_forward -= move_max;
    }
    if (!three_button_map_chord && strafing) {
        if ((joy & BUTTON_LEFT) != 0) {
            target_strafe -= strafe_max;
        }
        if ((joy & BUTTON_RIGHT) != 0) {
            target_strafe += strafe_max;
        }
    }

    s_vel_forward = approach_velocity(s_vel_forward, target_forward, MOVE_ACCEL, MOVE_DECEL);
    s_vel_strafe = approach_velocity(s_vel_strafe, target_strafe, MOVE_ACCEL, MOVE_DECEL);

    if ((s_vel_forward != 0) || (s_vel_strafe != 0)) {
        player_try_move(player, s_vel_forward, s_vel_strafe);
        // Keep redrawing/moving while velocity decays after the button is released.
        result |= PLAYER_CONTROL_CHANGED;
    }

    if (three_button_map_chord) {
        if (!s_three_button_map_chord_active) {
            result |= PLAYER_CONTROL_TOGGLE_AUTOMAP;
        }
        s_three_button_map_chord_active = TRUE;
    } else {
        s_three_button_map_chord_active = FALSE;

        if (((joy & BUTTON_C) != 0) && ((s_previous_joy & BUTTON_C) == 0) && !strafing) {
            result |= PLAYER_CONTROL_USE;
        }
        if (((joy & BUTTON_B) != 0) && ((s_previous_joy & BUTTON_B) == 0)) {
            result |= PLAYER_CONTROL_FIRE;
        }
    }

    if (six_button_pad) {
        if (((joy & BUTTON_X) != 0) && ((s_previous_joy & BUTTON_X) == 0)) {
            result |= PLAYER_CONTROL_PREVIOUS_WEAPON;
        }
        if (((joy & BUTTON_Y) != 0) && ((s_previous_joy & BUTTON_Y) == 0)) {
            result |= PLAYER_CONTROL_NEXT_WEAPON;
        }
        if (((joy & BUTTON_Z) != 0) && ((s_previous_joy & BUTTON_Z) == 0)) {
            result |= PLAYER_CONTROL_TOGGLE_AUTOMAP;
        }
    }

    s_previous_joy = joy;
    return result;
}
