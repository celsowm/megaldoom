#ifndef MEGALDOOM_DEBUG_LIGHT_H
#define MEGALDOOM_DEBUG_LIGHT_H

#include <genesis.h>
#include "raycast.h"

// Player-facing "debug light" mode, switched from the OPTIONS menu, for remote
// testers who can only send a screenshot. Unlike DEBUG_PERF and the BlastEm
// mailboxes it is compiled into release ROMs and costs one flag test per
// movement hop while it is off.
//
// While on, two text rows in the top letterbox (BG_B, never scrolled) show the
// map, pose, sector, how many solid walls the player has been carried through,
// and the last knockback. A detected wall crossing freezes the last pose that
// was still inside, and holding A+B+C for a second puts the player back there.

// Boot state, for capture builds that cannot open the OPTIONS menu
// (EXTRA_FLAGS="-DDEBUG_LIGHT_BOOT=1").
#ifndef DEBUG_LIGHT_BOOT
#define DEBUG_LIGHT_BOOT 0
#endif

bool debug_light_enabled(void);
void debug_light_set_enabled(bool enabled);

// A new level (or rebirth) placed the player: forget crossings, re-seed the
// safe pose, and repaint on the next draw.
void debug_light_level_start(const PlayerState *player);
// Something cleared BG_B's letterbox (renderer_init, a menu return).
void debug_light_invalidate(void);

// player_apply_world_push reports every committed hop while the mode is on.
void debug_light_note_hop(s32 from_x, s32 from_y, s32 to_x, s32 to_y);
// Enemy-hit / barrel knockback as applied, in world units.
void debug_light_note_knockback(s32 dx, s32 dy);

// Feed the held pad state once per gameplay iteration. Returns TRUE when it
// moved the player back to the last safe pose; the caller must then drop
// momentum and request a base redraw.
bool debug_light_update_rescue(PlayerState *player, u16 held_joy, u16 elapsed_vblanks);

void debug_light_draw(const PlayerState *player, u16 phase_index);

#endif
