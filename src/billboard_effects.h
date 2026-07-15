#ifndef MEGALDOOM_BILLBOARD_EFFECTS_H
#define MEGALDOOM_BILLBOARD_EFFECTS_H

#include "billboard.h"

void billboard_effects_reset(void);
void billboard_effects_spawn_puff(s32 x, s32 y);
void billboard_effects_spawn_blood(s32 x, s32 y);
bool billboard_update_effects(void);
u16 billboard_project_effects(const PlayerState *player,
                              ProjectedBillboard *objects,
                              u16 max_objects);

#endif
