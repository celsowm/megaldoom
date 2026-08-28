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

// --- Doom-style weapon bob (P_CalcBob analogue) ------------------------------
// Doom derives bob magnitude from |momentum|^2 clamped to MAXBOB, so running
// pins the bob and walking sits a little below it, and the weapon keeps moving
// while friction bleeds momentum off after the stick is released. MegaLDOOM
// reproduces the *observable* motion without the 64-bit FixedMul: the momentum
// magnitude is approximated with the alpha-max-plus-beta-min octagonal norm
// (no sqrt, no extra table, no __mulsi3 in the hot loop), which also makes the
// amplitude linear in speed so walk vs run land near Doom's ~1:2 ratio.
//
// Phase advances once per 35 Hz movement tic (never per rendered frame). The
// horizontal offset is amplitude*cos(phase); the vertical offset folds the
// phase onto a half circle so it runs at twice the frequency and stays on the
// positive lobe, matching Doom's `angle &= FINEANGLES/2 - 1` trick.
//
// Every constant below is a visual tuning target, not a gameplay contract:
// gameplay momentum stays authoritative and untouched.
#define BOB_MOMENTUM_SHIFT 12   // s_momentum_* (Q16.16) -> compact per-axis word
#define BOB_MAG_MAX        360  // ~running speed; amplitude saturates from here
#define BOB_X_NUM          14   // horizontal amplitude numerator
#define BOB_Y_NUM          14   // vertical amplitude numerator
#define BOB_TRIG_SHIFT     17   // (mag * fx_trig * NUM) >> shift -> pixels
// Doom sways the weapon ~+/-16px horizontally and dips it ~0..16px at 320x200;
// MegaLDOOM's view is 120px tall so the throw scales to ~+/-10 / 0..10. The
// vertical dip only ever moves the gun DOWN (Doom's positive-lobe wave), and the
// 3D view is parked flush against the status bar so the gun's bottom dips into
// the WINDOW/HUD region where plane A is suppressed -- the cut-off edge is never
// seen. Lifting the gun (negative dip) would float it, so the wave stays >= 0.
#define BOB_MAX_X          10   // horizontal swing, +/- pixels
#define BOB_MAX_Y          10   // vertical dip, 0..pixels (downward only)
// One full horizontal cos cycle every 20 Doom tics, like Doom's FINEANGLES/20.
#define BOB_TICS_PER_CYCLE 20
#define BOB_PHASE_STEP     ((ANGLE_STEPS + BOB_TICS_PER_CYCLE / 2) / BOB_TICS_PER_CYCLE)
// How many tics the bob keeps running after the last whole-pixel move. Bridges
// the sub-pixel gaps of a slow walk, but short enough that shoving into a wall
// (momentum stays high, but the player stops translating) eases the weapon back
// to neutral instead of racing the walk cadence on the spot.
#define BOB_MOVE_GRACE_TICS 4

static u16 s_previous_joy = 0;
static bool s_three_button_map_chord_active = FALSE;
// Last direction the C+UP/C+DOWN weapon chord was held in (0 = not held), so
// the selection advances once per press instead of once per frame.
static s16 s_weapon_chord_dir = 0;
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
static u16 s_weapon_bob_phase = 0;
static s16 s_weapon_bob_x = 0;
static s16 s_weapon_bob_y = 0;
static u8 s_weapon_bob_move_grace = 0;

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

// Recompute the weapon bob from the momentum left after this tic's friction.
// Runs at the 35 Hz movement rate; the renderer only ever reads the result.
// `moved` is whether the player crossed a whole world pixel this tic.
static void update_weapon_bob(bool moved) {
    if (moved) {
        s_weapon_bob_move_grace = BOB_MOVE_GRACE_TICS;
    } else if (s_weapon_bob_move_grace > 0) {
        s_weapon_bob_move_grace--;
    }

    s32 mx = s_momentum_x >> BOB_MOMENTUM_SHIFT;
    s32 my = s_momentum_y >> BOB_MOMENTUM_SHIFT;
    if (mx < 0) mx = -mx;
    if (my < 0) my = -my;
    const s32 hi = (mx > my) ? mx : my;
    const s32 lo = (mx > my) ? my : mx;
    s32 mag = hi + (lo >> 1);
    if (mag > BOB_MAG_MAX) mag = BOB_MAG_MAX;

    if (mag == 0 || s_weapon_bob_move_grace == 0) {
        // Stopped, or pinned against a wall with the stick still held: ease the
        // weapon back to neutral 1px/tic (holding the phase so a walk that
        // resumes picks up mid-cycle) instead of running the cadence in place.
        if (s_weapon_bob_x > 0) s_weapon_bob_x--;
        else if (s_weapon_bob_x < 0) s_weapon_bob_x++;
        if (s_weapon_bob_y > 0) s_weapon_bob_y--;
        if (s_weapon_bob_x == 0 && s_weapon_bob_y == 0) s_weapon_bob_phase = 0;
        return;
    }

    s_weapon_bob_phase = (u16)((s_weapon_bob_phase + BOB_PHASE_STEP) & ANGLE_MASK);

    // mag and fx_* both fit a word, so keep the products muls.w-sized (like the
    // thrust path) and let GCC lower the small *NUM into shift/add.
    s32 bob_x = (player_muls_word((s16)mag, fx_cos(s_weapon_bob_phase)) * BOB_X_NUM)
                >> BOB_TRIG_SHIFT;
    if (bob_x > BOB_MAX_X) bob_x = BOB_MAX_X;
    else if (bob_x < -BOB_MAX_X) bob_x = -BOB_MAX_X;

    // Fold the phase onto a half circle: sin over [0, pi) is the positive lobe,
    // repeated twice per horizontal cycle -> 2x frequency, offset stays >= 0.
    s16 vwave = fx_sin((u16)(s_weapon_bob_phase & (ANGLE_STEPS / 2 - 1)));
    if (vwave < 0) vwave = (s16)-vwave;
    s32 bob_y = (player_muls_word((s16)mag, vwave) * BOB_Y_NUM) >> BOB_TRIG_SHIFT;
    if (bob_y > BOB_MAX_Y) bob_y = BOB_MAX_Y;

    s_weapon_bob_x = (s16)bob_x;
    s_weapon_bob_y = (s16)bob_y;
}

s16 player_controller_weapon_bob_x(void) { return s_weapon_bob_x; }
s16 player_controller_weapon_bob_y(void) { return s_weapon_bob_y; }

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
    const bool moved = (player->x != old_x) || (player->y != old_y);
    update_weapon_bob(moved);
    return moved;
}

void player_controller_reset(void) {
    s_previous_joy = 0;
    s_three_button_map_chord_active = FALSE;
    s_weapon_chord_dir = 0;
    s_momentum_x = 0;
    s_momentum_y = 0;
    s_position_remainder_x = 0;
    s_position_remainder_y = 0;
    s_doom_tic_accumulator = 0;
    s_turn_speed_fp = 0;
    s_turn_remainder_fp = 0;
    s_turn_dir = 0;
    s_weapon_bob_phase = 0;
    s_weapon_bob_x = 0;
    s_weapon_bob_y = 0;
    s_weapon_bob_move_grace = 0;
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
    // C is already the modifier that turns lateral D-pad input into strafe, so
    // C+UP / C+DOWN is the natural free chord for weapon selection and is the
    // only one available at all on a 3-button pad (A runs, B fires, C uses,
    // START pauses). Emitted on both pad types so the habit carries over.
    const bool weapon_chord = ((joy & BUTTON_C) != 0) &&
                              ((joy & (BUTTON_UP | BUTTON_DOWN)) != 0);
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

    if (!three_button_map_chord && !weapon_chord && ((joy & BUTTON_UP) != 0)) {
        target_forward += move_command;
    }
    if (!three_button_map_chord && !weapon_chord && ((joy & BUTTON_DOWN) != 0)) {
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

    const s16 bob_x_before = s_weapon_bob_x;
    const s16 bob_y_before = s_weapon_bob_y;
    s_doom_tic_accumulator = (u16)(s_doom_tic_accumulator +
        (elapsed_frames << 5) + (elapsed_frames << 1) + elapsed_frames);
    while (s_doom_tic_accumulator >= VIDEO_VBLANKS_PER_SECOND) {
        s_doom_tic_accumulator -= VIDEO_VBLANKS_PER_SECOND;
        if (simulate_doom_movement_tic(player, target_forward, target_strafe)) {
            result |= PLAYER_CONTROL_CHANGED;
        }
    }
    // The bob can advance (or settle back to neutral during the friction tail)
    // on a tic that moved the player less than a whole world pixel, so ask for a
    // weapon-overlay frame whenever the offset actually changed.
    if (s_weapon_bob_x != bob_x_before || s_weapon_bob_y != bob_y_before) {
        result |= PLAYER_CONTROL_WEAPON_BOB;
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
        if ((((joy & BUTTON_C) != 0) && ((s_previous_joy & BUTTON_C) == 0) &&
             !strafing && !weapon_chord) ||
            (((latched_pressed & BUTTON_C) != 0) && !strafing && !weapon_chord)) {
            result |= PLAYER_CONTROL_USE;
        }
        if ((((joy & BUTTON_B) != 0) && ((s_previous_joy & BUTTON_B) == 0)) ||
            ((latched_pressed & BUTTON_B) != 0)) {
            result |= PLAYER_CONTROL_FIRE;
        }
        if ((joy & BUTTON_B) != 0) {
            result |= PLAYER_CONTROL_FIRE_HELD;
        }
    }

    // Weapon chord, edge-triggered on the direction so holding it does not spin
    // through the whole arsenal. UP selects the next weapon, DOWN the previous,
    // matching the D-pad direction of Doom's own weapon list.
    if (weapon_chord) {
        const s16 direction = ((joy & BUTTON_UP) != 0) ? 1 : -1;
        if (direction != s_weapon_chord_dir) {
            result |= (direction > 0) ? PLAYER_CONTROL_NEXT_WEAPON
                                      : PLAYER_CONTROL_PREVIOUS_WEAPON;
        }
        s_weapon_chord_dir = direction;
    } else {
        s_weapon_chord_dir = 0;
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
