#ifndef MEGALDOOM_RENDERER_PACK_ABI_H
#define MEGALDOOM_RENDERER_PACK_ABI_H

// Single source of truth for every constant the hand-written pack hotpath
// (renderer_hotpath.s) used to spell out as a literal. SGDK assembles .s files
// with `gcc -x assembler-with-cpp` (makefile.gen), so the assembler consumes
// these #defines directly instead of carrying a second copy of the numbers.
//
// Why this file exists: the vertical wrap in the wall post used to be a bare
// `andi.w #63,d5`. When the wall texture grew to 128 rows, missing that literal
// would have produced silently corrupted textures rather than a build error --
// the failure mode is invisible to the compiler, the linker and the tests.
// Every value here is now checked against the C definition it mirrors by the
// _Static_assert block in renderer_pack_internal.h, so a drift is a build
// failure instead of a rendering artifact.
#include "raycast.h"

// A 4bpp tile row is 8 pixels in 4 bytes, and at stride 2 each sampled column
// owns exactly one of those bytes. PACK_LANES is therefore both the number of
// cast columns per tile column and the byte stride from one screen row to the
// next within a tile column (see the column-major note in renderer_hotpath.s).
#define PACK_LANES (8 / RAY_COL_STRIDE)
#define PACK_TILE_ROW_BYTES 4

// WallColumnDescriptor field offsets and size, as indexed by the asm.
#define WALL_DESC_OFF_TOP 0
#define WALL_DESC_OFF_BOTTOM 2
#define WALL_DESC_OFF_VERTICAL_SAMPLES 12
#define WALL_DESC_OFF_TEX_Y 17
#define WALL_DESC_SIZE 24

// PackedFlatRows: two 4-entry u32 row tables, ceiling first. The flat pattern
// has a 4-row period, so the byte index steps by PACK_TILE_ROW_BYTES and wraps
// with PACK_FLAT_INDEX_MASK.
#define PACK_FLAT_OFF_CEILING 0
#define PACK_FLAT_OFF_FLOOR 16
#define PACK_FLAT_ROWS_BYTES 16
#define PACK_FLAT_INDEX_MASK (PACK_FLAT_ROWS_BYTES - 1)

// Stack layout of renderer_write_mixed_stride2_span_asm after its prologue.
// GCC's m68k ABI gives every argument a 4-byte stack slot and right-aligns a
// u16 within its slot, so a word argument sits 2 bytes into its slot. The
// prologue pushes d2-d7/a2-a6 (11 registers, 44 bytes) on top of the 4-byte
// return address.
//
// These are used in displacement position (`PACK_ARG_TILES(sp)`), so they are
// deliberately written WITHOUT enclosing parentheses: a leading '(' there would
// look like the start of an addressing mode to the m68k assembler. Operator
// precedence already makes the unparenthesised form correct.
#define PACK_ASM_SAVED_REGS 11
#define PACK_ASM_FRAME 4 + PACK_ASM_SAVED_REGS * 4
#define PACK_ARG_TILES PACK_ASM_FRAME + 0
#define PACK_ARG_PIXEL_Y PACK_ASM_FRAME + 6
#define PACK_ARG_ROW_COUNT PACK_ASM_FRAME + 10
#define PACK_ARG_DESCRIPTORS PACK_ASM_FRAME + 12
#define PACK_ARG_PACKED_COLUMNS PACK_ASM_FRAME + 16
#define PACK_ARG_FLAT_ROWS PACK_ASM_FRAME + 20

#endif
