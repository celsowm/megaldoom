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

// PackedFlatRows: two u32 row tables indexed by screen row, stepping by
// PACK_TILE_ROW_BYTES and wrapping with the table's own mask, so the asm's two
// flat posts are the same instruction sequence at two different widths.
//
// The FLOOR keeps the original 4-row period: it is always a repeating flat.
// The CEILING is 128 rows because a sky sector sources it from
// MEGALDOOM_SKY_CEILING_ROWS, one band per screen row (see renderer_flats.c).
// An indoor ceiling simply replicates its 4-row pattern across all 128, so
// there is no second code path and no branch in the hot post -- the indoor
// case executes exactly the instructions it did before, against a wider table.
//
// The FLOOR rows are embedded; the CEILING is a POINTER to one of two ROM
// tables (MEGALDOOM_FLAT_CEILING_ROWS / MEGALDOOM_SKY_CEILING_ROWS), so
// stepping outdoors swaps a pointer instead of rebuilding 64 rows of work RAM
// every frame. The ceiling post pays one extra MOVEA.L per post -- never per
// pixel -- to load it. Floor stays first so its `lea PACK_FLAT_OFF_FLOOR(a3,
// d6.w),a4` keeps the 8-bit displacement the 68000 indexed mode allows.
// 64, not the full 120-row viewport: walls are centred
// (top = (VIEW_PIXEL_H - wall_h) / 2 with wall_h >= 1), so a ceiling run can
// never start below row 59 and the table only has to cover the top half. Kept
// a power of two so the asm post can keep masking with an immediate. The
// _Static_asserts in renderer_pack_internal.h pin both facts.
#define PACK_CEILING_ROW_COUNT 64
#define PACK_CEILING_ROWS_BYTES (PACK_CEILING_ROW_COUNT * PACK_TILE_ROW_BYTES)
#define PACK_FLOOR_ROWS_BYTES 16
#define PACK_FLAT_OFF_FLOOR 0
#define PACK_FLAT_OFF_CEILING PACK_FLOOR_ROWS_BYTES
#define PACK_CEILING_INDEX_MASK (PACK_CEILING_ROWS_BYTES - 1)
#define PACK_FLOOR_INDEX_MASK (PACK_FLOOR_ROWS_BYTES - 1)

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

// Stack layout of the two door/window overlay posts (renderer_hotpath.s), by
// the same GCC m68k rule as the block above: every argument gets a 4-byte slot
// and a u16 sits 2 bytes into its own. Written without enclosing parentheses
// for the same reason.
//
// The overlay compositor writes ONE byte lane of ONE tile column, so unlike the
// mixed-tile packer these take a byte pointer already resolved to their first
// row and never look at the descriptor: the caller hands over the exact DDA
// entry the post starts at. That keeps the frame post's body identical to
// .Lwall_loop and the sky post's to .Lceiling_loop, which is the whole point --
// an overlay texel and a wall texel two pixels to its left are now produced by
// the same instructions, not merely the same table.
//
// The frame post saves d2/a2; the sky post needs no callee-saved register at
// all, so its frame is just the return address.
#define OVL_ASM_SAVED_REGS 2
#define OVL_ASM_FRAME 4 + OVL_ASM_SAVED_REGS * 4
#define OVL_ARG_DST OVL_ASM_FRAME + 0
#define OVL_ARG_ROWS OVL_ASM_FRAME + 6
#define OVL_ARG_DDA OVL_ASM_FRAME + 8
#define OVL_ARG_PACKED OVL_ASM_FRAME + 12
#define OVL_ARG_TEX_Y OVL_ASM_FRAME + 18

#define SKY_ASM_FRAME 4
#define SKY_ARG_DST SKY_ASM_FRAME + 0
#define SKY_ARG_ROWS SKY_ASM_FRAME + 6
#define SKY_ARG_BYTES SKY_ASM_FRAME + 8
#define SKY_ARG_INDEX SKY_ASM_FRAME + 14

#endif
