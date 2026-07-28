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
// Doom movement runs at 35 game tics regardless of video cadence. Commands,
// thrust and friction are the original integer contracts from linuxdoom 1.10.
#define DOOM_TICS_PER_SECOND 35
#define VIDEO_VBLANKS_PER_SECOND 60
#define DOOM_FORWARD_WALK 25
#define DOOM_FORWARD_RUN 50
#define DOOM_STRAFE_WALK 24
#define DOOM_STRAFE_RUN 40
#define DOOM_THRUST_SCALE 2048
#define DOOM_FRICTION 0xE800L
#define DOOM_STOP_SPEED 0x1000L
#define DOOM_MAX_MOVE (30L << 16)

#define THREE_BUTTON_AUTOMAP_CHORD (BUTTON_A | BUTTON_B | BUTTON_C)

static u16 s_previous_joy = 0;
static bool s_three_button_map_chord_active = FALSE;
static volatile u16 s_latched_pressed = 0;
static u16 s_poll_prev = 0;
static volatile bool s_poll_active = FALSE;
static s32 s_momentum_x = 0;
static s32 s_momentum_y = 0;
static s32 s_position_remainder_x = 0;
static s32 s_position_remainder_y = 0;
static u16 s_doom_tic_accumulator = 0;
static s16 s_turn_speed_fp = 0; // per-vsync fixed-point turn rate
static s16 s_turn_remainder_fp = 0;
static s16 s_turn_dir = 0;

static s32 player_muls_word(s16 left, s16 right) {
    s32 result = left;
    __asm__ volatile (
        "muls.w %1,%0"
        : "+d" (result)
        : "d" (right)
        : "cc");
    return result;
}

static s32 clamp_momentum(s32 value) {
    if (value > DOOM_MAX_MOVE) return DOOM_MAX_MOVE;
    if (value < -DOOM_MAX_MOVE) return -DOOM_MAX_MOVE;
    return value;
}

// DOOM_FRICTION is exactly 29/32. This form stays inside signed 32-bit range
// on the 68000 and avoids pulling in a 64-bit multiplication helper.
static s32 apply_doom_friction(s32 value) {
    return value - ((value * 3) >> 5);
}

static s32 consume_position_delta(s32 momentum, s32 *remainder) {
    const s32 total = momentum + *remainder;
    // C division truncates toward zero. Spell the power-of-two case explicitly
    // so GCC cannot pull the 32-bit __divsi3 helper into every Doom tic.
    const s32 whole = (total >= 0) ? (total >> 16) : -((-total) >> 16);
    *remainder = (total >= 0) ? (total & 0xFFFFL) : -((-total) & 0xFFFFL);
    return whole;
}

static bool simulate_doom_movement_tic(PlayerState *player, s16 forward_command,
                                       s16 strafe_command) {
    const s16 dir_x = fx_cos(player->angle);
    const s16 dir_y = fx_sin(player->angle);
    const s16 side_x = (s16)-dir_y;
    const s16 side_y = dir_x;
    const s32 old_x = player->x;
    const s32 old_y = player->y;

    // (basis * (command * 2048)) >> 8 == (basis * command) << 3.
    // Keeping the actual multiply word-sized avoids four __mulsi3 calls/tic.
    const s16 thrust_x = (s16)(player_muls_word(dir_x, forward_command) +
                               player_muls_word(side_x, strafe_command));
    const s16 thrust_y = (s16)(player_muls_word(dir_y, forward_command) +
                               player_muls_word(side_y, strafe_command));
    s_momentum_x = clamp_momentum(s_momentum_x + player_muls_word(thrust_x, 8));
    s_momentum_y = clamp_momentum(s_momentum_y + player_muls_word(thrust_y, 8));

    player_apply_world_push(player,
        consume_position_delta(s_momentum_x, &s_position_remainder_x),
        consume_position_delta(s_momentum_y, &s_position_remainder_y));

    if (forward_command == 0 && strafe_command == 0 &&
        s_momentum_x > -DOOM_STOP_SPEED && s_momentum_x < DOOM_STOP_SPEED &&
        s_momentum_y > -DOOM_STOP_SPEED && s_momentum_y < DOOM_STOP_SPEED) {
        s_momentum_x = 0;
        s_momentum_y = 0;
        s_position_remainder_x = 0;
        s_position_remainder_y = 0;
    } else {
        s_momentum_x = apply_doom_friction(s_momentum_x);
        s_momentum_y = apply_doom_friction(s_momentum_y);
    }
    return player->x != old_x || player->y != old_y;
}

void player_controller_reset(void) {
    s_previous_joy = 0;
    s_three_button_map_chord_active = FALSE;
    s_momentum_x = 0;
    s_momentum_y = 0;
    s_position_remainder_x = 0;
    s_position_remainder_y = 0;
    s_doom_tic_accumulator = 0;
    s_turn_speed_fp = 0;
    s_turn_remainder_fp = 0;
    s_turn_dir = 0;
}

void player_controller_vint_poll(void) {
    if (!s_poll_active) return;
    JOY_update();
    const u16 now = JOY_readJoypad(JOY_1);
    s_latched_pressed |= (u16)(now & (u16)~s_poll_prev);
    s_poll_prev = now;
}

void player_controller_set_poll_active(bool active) {
    if (active) {
        s_poll_prev = JOY_readJoypad(JOY_1);
        s_latched_pressed = 0;
    }
    s_poll_active = active;
}

u16 player_controller_consume_latched(void) {
    SYS_disableInts();
    const u16 latched = s_latched_pressed;
    s_latched_pressed = 0;
    SYS_enableInts();
    return latched;
}

u16 player_controller_update(PlayerState *player, u16 elapsed_frames, u16 latched_pressed) {
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
    const s16 move_command = running ? DOOM_FORWARD_RUN : DOOM_FORWARD_WALK;
    const s16 strafe_command = running ? DOOM_STRAFE_RUN : DOOM_STRAFE_WALK;
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

        const s32 delta_fp = player_muls_word(s_turn_speed_fp, (s16)elapsed_frames) +
                             s_turn_remainder_fp;
        const s16 turn = (s16)(delta_fp >> TURN_FP_SHIFT);

        s_turn_remainder_fp = (s16)(delta_fp & (TURN_FP_ONE - 1));

        if (turn != 0) {
            player->angle = (u16)((player->angle +
                ((desired_turn < 0) ? -turn : turn)) & ANGLE_MASK);
            result |= PLAYER_CONTROL_CHANGED;
        }
    } else {
        s_turn_speed_fp = 0;
        s_turn_remainder_fp = 0;
    }
    s_turn_dir = desired_turn;

    if (!three_button_map_chord && ((joy & BUTTON_UP) != 0)) {
        target_forward += move_command;
    }
    if (!three_button_map_chord && ((joy & BUTTON_DOWN) != 0)) {
        target_forward -= move_command;
    }
    if (!three_button_map_chord && strafing) {
        if ((joy & BUTTON_LEFT) != 0) {
            target_strafe -= strafe_command;
        }
        if ((joy & BUTTON_RIGHT) != 0) {
            target_strafe += strafe_command;
        }
    }

    s_doom_tic_accumulator = (u16)(s_doom_tic_accumulator +
        (elapsed_frames << 5) + (elapsed_frames << 1) + elapsed_frames);
    while (s_doom_tic_accumulator >= VIDEO_VBLANKS_PER_SECOND) {
        s_doom_tic_accumulator -= VIDEO_VBLANKS_PER_SECOND;
        if (simulate_doom_movement_tic(player, target_forward, target_strafe)) {
            result |= PLAYER_CONTROL_CHANGED;
        }
    }

    if (three_button_map_chord) {
        if (!s_three_button_map_chord_active) {
            result |= PLAYER_CONTROL_TOGGLE_AUTOMAP;
        }
        s_three_button_map_chord_active = TRUE;
    } else {
        s_three_button_map_chord_active = FALSE;

        // Cached-pad edge OR the ISR latch: a tap that starts and ends between
        // main-loop iterations still fires, since the latch caught its rising
        // edge even though this iteration's cached joy reads no button held.
        if ((((joy & BUTTON_C) != 0) && ((s_previous_joy & BUTTON_C) == 0) && !strafing) ||
            (((latched_pressed & BUTTON_C) != 0) && !strafing)) {
            result |= PLAYER_CONTROL_USE;
        }
        if ((((joy & BUTTON_B) != 0) && ((s_previous_joy & BUTTON_B) == 0)) ||
            ((latched_pressed & BUTTON_B) != 0)) {
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
