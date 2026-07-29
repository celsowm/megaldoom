#ifndef MEGALDOOM_DOOM_SKILL_H
#define MEGALDOOM_DOOM_SKILL_H

/* The five player-facing Doom skill names. The map stores only three THING
 * population flags, so the runtime maps these selections to easy/normal/hard
 * when it spawns level objects. */
typedef enum {
    DOOM_SKILL_IM_TOO_YOUNG_TO_DIE = 0,
    DOOM_SKILL_HEY_NOT_TOO_ROUGH = 1,
    DOOM_SKILL_HURT_ME_PLENTY = 2,
    DOOM_SKILL_ULTRA_VIOLENCE = 3,
    DOOM_SKILL_NIGHTMARE = 4
} DoomSkill;

#endif
