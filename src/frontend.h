#ifndef MEGALDOOM_FRONTEND_H
#define MEGALDOOM_FRONTEND_H

#include <genesis.h>
#include "doom_skill.h"

typedef enum {
    FRONTEND_PAUSE_RESUME = 0,
    FRONTEND_PAUSE_QUIT_TO_TITLE
} FrontendPauseAction;

DoomSkill frontend_run(void);
FrontendPauseAction frontend_run_pause(u16 tile_base);

#endif
