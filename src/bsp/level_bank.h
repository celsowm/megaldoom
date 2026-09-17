#ifndef MEGALDOOM_LEVEL_BANK_H
#define MEGALDOOM_LEVEL_BANK_H

#include <genesis.h>

// Banked level window (Sega SSF mapper). Each level's wall pack -- the
// shade-ready pair columns for exactly the textures that level draws -- lives
// in its own physical banks and is mapped into 0x280000-0x3FFFFF while the
// level is loaded (tools/md_banked.ld). Everything else stays resident below
// 0x280000 and is never remapped, so interrupts and a soft reset are safe.
//
// This is the only module that writes the mapper registers.

#define MEGALDOOM_LEVEL_WINDOW_BASE 0x00280000UL

// The active level's block addresses, indexed by global texture id: the
// window address of that texture's 32 KB [shade][tex_x][tex_y] block. Wall
// entries always point at a block (an unused id at the fallback); a door
// entry is NULL when the texture has no door framing in this level. Valid to
// dereference only while the level's banks are mapped, i.e. always between
// level loads.
extern const u8 *const *g_level_wall_bases;
extern const u8 *const *g_level_door_bases;

// Map level_index's pack into the window and select its address tables.
// Called on every level load; level 0 is also the power-on mapping.
void level_bank_select(u16 level_index);

#endif
