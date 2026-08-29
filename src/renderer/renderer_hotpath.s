/*
 * Every constant this file needs comes from the shared C/asm header below --
 * SGDK assembles .s through `gcc -x assembler-with-cpp`, so there is no second
 * copy of the descriptor layout, the lane count or the texture masks here.
 * renderer_pack_internal.h _Static_asserts each of those against the real C
 * definition, which turns what used to be a silent texture-corruption bug into
 * a build failure.
 */
#include "renderer_pack_abi.h"

/* This is the stride-2 packer. renderer_pack.c declares and calls it only under
 * the same condition, and supplies a pure-C packer for stride 4, so a
 * comparison build (EXTRA_FLAGS="-DRAY_COL_STRIDE=4") assembles this file to
 * nothing rather than emitting a function whose lane count no longer matches. */
#if RAY_COL_STRIDE == 2

    .text
    .align  2
    .globl  renderer_write_mixed_stride2_span_asm

/*
 * void renderer_write_mixed_stride2_span_asm(
 *     u32 *tiles, u16 pixel_y, u16 row_count,
 *     const WallColumnDescriptor descriptors[4],
 *     const u8 *const packed_columns[4],
 *     const PackedFlatRows *flat_rows);
 *
 * The stack layout and the WallColumnDescriptor / PackedFlatRows offsets are
 * defined once in renderer_pack_abi.h and used symbolically below.
 *
 * `tiles` is the first tile of a run of consecutive tiles in one tile column,
 * and row_count spans all of them. That works because the view tilemap is
 * column-major (view_tile_index = tile_x * VIEW_TILE_H + tile_y), so a column's
 * tiles are contiguous, and within that block screen row y of lane L sits at
 * byte (y>>3)*32 + (y&7)*4 + L, which is identically 4*y + L. The tile boundary
 * is invisible to a stride-4 byte walk, so a whole column is one call: each
 * lane emits three posts total instead of three per 8-row tile, and the
 * movem/call overhead is paid once instead of once per tile.
 *
 * Each lane is emitted as at most three monotonic posts (ceiling, wall, floor),
 * so the inner loops contain no per-pixel material branch and no 32-bit
 * shift/or composition.
 *
 * Each post's length is computed once and the loop closed with DBRA. The
 * original form re-tested both the post bound and end_y every pixel
 * (cmp/bcc/cmp/bcs = 26 cycles) where DBRA costs 10; the wall post additionally
 * re-read tex_y from the descriptor every pixel, which is now held in d4 (the
 * `bottom` it replaces is dead once the post length is known). 104 -> 74
 * cycles per wall byte.
 *
 * Everything a post can hoist out of its body is then hoisted, because the pack
 * stage is ~50% of a moving frame and these three loops are nearly all of it:
 *
 *   - Destinations are (a6), not 0(a6). The zero displacement is not free --
 *     d16(An) costs 4 cycles more than (An) on every single byte written.
 *   - The wall post walks the DDA with (a4)+ instead of 0(a4,d0.w) plus an
 *     addq.w to keep d0 in step: 8 cycles against 18. d0 is not needed inside
 *     the loop at all, and its post-loop value is just min(bottom, end_y),
 *     which the length computation already produced.
 *   - The wall post's `moveq #0,d5` is loop-invariant, which is not obvious:
 *     `andi.w #127,d5` leaves bits 7-15 clear, `move.b` writes only bits 0-7,
 *     and `add.b` discards its carry, so d5's high byte is still zero at the
 *     top of the next iteration. One moveq per post, not per pixel.
 *   - The flat posts recomputed ((y&3)<<2)+lane every pixel (move/andi/lsl/add
 *     = 26 cycles) to index a 16-byte table whose period is 4. The lane term
 *     folds into the base pointer once, and the index just steps +4 mod 16.
 *
 * Wall byte 74 -> 56 cycles, flat byte 70 -> 48. Mixed tiles are the pack
 * stage's dominant cost (~42 subticks each over a flat tile, and a rotation
 * doubles their count: 149/rebuild spinning vs 75 translating).
 */
renderer_write_mixed_stride2_span_asm:
    movem.l d2-d7/a2-a6,-(sp)      /* PACK_ASM_SAVED_REGS registers */
    movea.l PACK_ARG_TILES(sp),a0  /* first tile of the run */
    move.w  PACK_ARG_PIXEL_Y(sp),d7 /* first screen y (right-aligned u16) */
    move.w  PACK_ARG_ROW_COUNT(sp),d1
    movea.l PACK_ARG_DESCRIPTORS(sp),a1
    movea.l PACK_ARG_PACKED_COLUMNS(sp),a2
    movea.l PACK_ARG_FLAT_ROWS(sp),a3
    add.w   d7,d1                  /* end_y; loop-invariant across all lanes */
    moveq   #0,d6                  /* byte lane 0..3 */

.Lmixed_lane:
    movea.l (a2)+,a5               /* preshaded wall pairs */
    movea.l a0,a6
    adda.w  d6,a6                  /* first byte for this lane */
    move.w  d7,d0                  /* y */
    /* WALL_DESC_OFF_TOP is asserted to be 0; spelled as (a1) rather than
     * 0(a1) because a zero displacement is not free -- d16(An) costs 4 cycles
     * more than (An), which is the same reason the posts below write to (a6). */
    move.w  (a1),d3                        /* top */
    move.w  WALL_DESC_OFF_BOTTOM(a1),d4    /* bottom */

    move.w  d3,d2                  /* min(top, end_y) */
    cmp.w   d1,d2
    bls.s   .Lceiling_ready
    move.w  d1,d2
.Lceiling_ready:
    cmp.w   d2,d0
    bcc.s   .Lwall_setup
    sub.w   d0,d2                  /* ceiling post length */
    subq.w  #1,d2
    move.w  d2,d5                  /* DBRA counter */
    movea.l PACK_FLAT_OFF_CEILING(a3),a4 /* ROM ceiling table (flat or sky) */
    adda.w  d6,a4                  /* ...offset to this lane's byte */
    move.w  d0,d2                  /* index = (y & 127) * PACK_TILE_ROW_BYTES */
    andi.w  #(PACK_CEILING_ROW_COUNT - 1),d2
    lsl.w   #2,d2
    add.w   d5,d0                  /* y advances past the whole post */
    addq.w  #1,d0
.Lceiling_loop:
    move.b  0(a4,d2.w),(a6)
    addq.l  #PACK_TILE_ROW_BYTES,a6
    addq.w  #PACK_TILE_ROW_BYTES,d2
    andi.w  #PACK_CEILING_INDEX_MASK,d2 /* 128 screen rows: flat OR sky */
    dbra    d5,.Lceiling_loop

.Lwall_setup:
    move.w  d4,d2                  /* min(bottom, end_y) */
    cmp.w   d1,d2
    bls.s   .Lwall_ready
    move.w  d1,d2
.Lwall_ready:
    cmp.w   d2,d0
    bcc.s   .Lfloor_setup
    movea.l WALL_DESC_OFF_VERTICAL_SAMPLES(a1),a4 /* vertical sample DDA */
    move.w  d0,d5
    sub.w   d3,d5                  /* y - top: where this tile enters the DDA */
    adda.w  d5,a4
    move.b  WALL_DESC_OFF_TEX_Y(a1),d4 /* vertical offset; bottom is dead */
    sub.w   d0,d2                  /* wall post length */
    add.w   d2,d0                  /* y after the post, for the floor setup */
    subq.w  #1,d2                  /* DBRA counter */
    moveq   #0,d5                  /* high byte stays clear across iterations */
.Lwall_loop:
    move.b  (a4)+,d5
    add.b   d4,d5
    andi.w  #WALL_TEX_HEIGHT_MASK,d5
    move.b  0(a5,d5.w),(a6)
    addq.l  #PACK_TILE_ROW_BYTES,a6
    dbra    d2,.Lwall_loop

.Lfloor_setup:
    cmp.w   d1,d0
    bcc.s   .Lnext_lane
    move.w  d1,d5                  /* floor post length */
    sub.w   d0,d5
    subq.w  #1,d5
    lea     PACK_FLAT_OFF_FLOOR(a3,d6.w),a4 /* floor rows for this lane */
    move.w  d0,d2                  /* flat index = (y & 3) * PACK_TILE_ROW_BYTES */
    andi.w  #3,d2
    lsl.w   #2,d2
.Lfloor_loop:
    move.b  0(a4,d2.w),(a6)
    addq.l  #PACK_TILE_ROW_BYTES,a6
    addq.w  #PACK_TILE_ROW_BYTES,d2
    andi.w  #PACK_FLOOR_INDEX_MASK,d2
    dbra    d5,.Lfloor_loop

.Lnext_lane:
    adda.w  #WALL_DESC_SIZE,a1
    addq.w  #1,d6
    cmpi.w  #PACK_LANES,d6
    bcs.w   .Lmixed_lane

    movem.l (sp)+,d2-d7/a2-a6
    rts

#endif /* RAY_COL_STRIDE == 2 */
