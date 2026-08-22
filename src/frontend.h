#ifndef MEGALDOOM_FRONTEND_H
#define MEGALDOOM_FRONTEND_H

#include <genesis.h>
#include "doom_skill.h"

typedef enum {
    FRONTEND_PAUSE_RESUME = 0,
    FRONTEND_PAUSE_QUIT_TO_TITLE
} FrontendPauseAction;

typedef struct {
    u16 completed_level;
    u16 next_level;
    u16 kills;
    u16 kill_total;
    u16 items;
    u16 item_total;
    u16 secrets;
    u16 secret_total;
    u32 time_vblanks;
    u16 par_seconds;
} FrontendIntermissionStats;

typedef enum {
    FRONTEND_INTERMISSION_CONTINUE = 0,
    FRONTEND_INTERMISSION_TITLE
} FrontendIntermissionAction;

DoomSkill frontend_run(void);
FrontendPauseAction frontend_run_pause(u16 tile_base);
FrontendIntermissionAction frontend_run_intermission(
    const FrontendIntermissionStats *stats);

// Death-screen "PRESS FIRE" prompt, drawn on BG_A over the frozen/red-tinted
// gameplay view (see main.c's death handling). Mirrors the title screen's
// PRESS START prompt: load once, then toggle visibility to blink it. Callers
// pass renderer_get_menu_tile_base() as tile_base, same as frontend_run_pause.
void frontend_load_death_prompt(u16 tile_base);
void frontend_set_death_prompt(u16 tile_base, bool visible);

#endif
