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

void player_controller_reset(void);
u16 player_controller_update(PlayerState *player, u16 elapsed_frames);

#endif
