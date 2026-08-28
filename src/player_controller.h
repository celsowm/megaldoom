#ifndef MEGALDOOM_PLAYER_CONTROLLER_H
#define MEGALDOOM_PLAYER_CONTROLLER_H

#include <genesis.h>
#include "raycast.h"

// Locked frame cadence (vblanks per main-loop iteration): 1 = 60fps, 2 = 30fps,
// 3 = 20fps. Shared by the main-loop pad wait (main.c) and the movement ramp
// (player_controller.c) so the time-correct dt scaling below stays in sync with
// the real cadence. See main.c for the tuning rationale (turning is the worst
// case: full cast + 300-tile bank-swap upload). The single textured BSP path
// keeps two vblanks a sustainable target;
// the uploader may consume one of them while splitting a full bank safely.
#define TARGET_FRAME_VSYNCS 2

#define PLAYER_CONTROL_CHANGED 0x0001
#define PLAYER_CONTROL_USE 0x0002
#define PLAYER_CONTROL_FIRE 0x0004
// Weapon selection: X/Y on a 6-button pad, and C+UP / C+DOWN on either pad so
// 3-button players can still cycle. The automap is still reserved.
#define PLAYER_CONTROL_PREVIOUS_WEAPON 0x0008
#define PLAYER_CONTROL_NEXT_WEAPON 0x0010
#define PLAYER_CONTROL_TOGGLE_AUTOMAP 0x0020
// B held, as opposed to PLAYER_CONTROL_FIRE's rising edge. Only the automatic
// weapons (chaingun, chainsaw) act on this; everything else stays semi-auto.
#define PLAYER_CONTROL_FIRE_HELD 0x0040
// The weapon bob offset moved this update (momentum bob advanced or decayed to
// neutral) without the player necessarily crossing a whole-pixel world step, so
// the renderer still needs a frame to re-apply the BG_A scroll. Cheaper than
// PLAYER_CONTROL_CHANGED: it only asks for a weapon-overlay redraw, no re-cast.
#define PLAYER_CONTROL_WEAPON_BOB 0x0080

void player_controller_reset(void);
u16 player_controller_update(PlayerState *player, u16 elapsed_frames, u16 latched_pressed);

// Doom-style weapon bob, in screen pixels, recomputed on the 35 Hz movement
// simulation from player momentum (never from raw D-pad state). The renderer
// applies these as an offset from the weapon's neutral origin; a stationary
// player reads (0, 0). Horizontal is +/-, vertical is 0..N (positive lobe).
s16 player_controller_weapon_bob_x(void);
s16 player_controller_weapon_bob_y(void);

// Called from the V-Int callback while poll is active: refreshes the pad and
// ORs any newly-pressed bits into a latch, so a tap shorter than one main-loop
// iteration is never dropped and input is never more than one vblank stale.
void player_controller_vint_poll(void);
// Gate the ISR poll. Deactivate before code that does its own JOY_update
// (e.g. the pause menu) to avoid racing it; reactivating reseeds the edge
// baseline from the current cached pad state (not zero) so a button already
// held does not phantom-fire, and clears any stale latch.
void player_controller_set_poll_active(bool active);
// Atomically read and clear the latch. Call once per main-loop iteration.
u16 player_controller_consume_latched(void);

#endif
