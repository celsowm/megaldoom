#ifndef MEGALDOOM_PLAYER_CONTROLLER_H
#define MEGALDOOM_PLAYER_CONTROLLER_H

#include <genesis.h>
#include "raycast.h"

#define PLAYER_CONTROL_CHANGED 0x0001
#define PLAYER_CONTROL_USE 0x0002
#define PLAYER_CONTROL_FIRE 0x0004

u16 player_controller_update(PlayerState *player);

#endif
