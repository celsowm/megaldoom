#ifndef MEGALDOOM_DOOM_RANDOM_H
#define MEGALDOOM_DOOM_RANDOM_H

#include <genesis.h>

// Doom's P_Random: the fixed 256-entry rndtable (m_random.c) walked by one
// index that the player's weapons and the monsters' attacks share, exactly as
// in Doom. It is a table, not a PRNG, so combat stays reproducible:
// enter_level resets the index as M_ClearRandom does, and the BlastEm route
// harness replays fixed input.
u8 doom_random(void);
void doom_random_reset(void);

// An aim offset rolled the way Doom rolls one, (P_Random() - P_Random()) <<
// shift of angle, returned as tan(offset) in Q12 (a trace's view-space slope).
// Doom uses shift 18 for the player's guns (at most +-5.6 degrees) and 20 for
// the monsters' (at most +-22.4 degrees). The difference of two uniform rolls
// makes the spread triangular: most shots land near the centre.
s16 doom_random_spread_q12(u8 shift);

#endif
