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
 * GCC's m68k ABI gives each u16 argument a four-byte stack slot and right-aligns
 * the word within it. After saving d2-d7/a2-a6 (44 bytes), pointers are at
 * sp+60,64,68 and the two words are at sp+54 and sp+58.
 * WallColumnDescriptor is 22 bytes: top=0, bottom=2, vertical_samples=12,
 * tex_y=17. PackedFlatRows has ceiling at 0 and floor at 16.
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
 *     `andi.w #63,d5` leaves bits 6-15 clear, `move.b` writes only bits 0-7,
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
    movem.l d2-d7/a2-a6,-(sp)
    movea.l 48(sp),a0              /* first tile of the run */
    move.w  54(sp),d7              /* first screen y (right-aligned u16) */
    move.w  58(sp),d1              /* row count */
    movea.l 60(sp),a1              /* descriptor */
    movea.l 64(sp),a2              /* packed-column pointer table */
    movea.l 68(sp),a3              /* flat rows */
    add.w   d7,d1                  /* end_y; loop-invariant across all lanes */
    moveq   #0,d6                  /* byte lane 0..3 */

.Lmixed_lane:
    movea.l (a2)+,a5               /* preshaded wall pairs */
    movea.l a0,a6
    adda.w  d6,a6                  /* first byte for this lane */
    move.w  d7,d0                  /* y */
    move.w  (a1),d3                /* top */
    move.w  2(a1),d4               /* bottom */

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
    lea     0(a3,d6.w),a4          /* ceiling row bytes for this lane */
    move.w  d0,d2                  /* flat index = (y & 3) * 4 */
    andi.w  #3,d2
    lsl.w   #2,d2
    add.w   d5,d0                  /* y advances past the whole post */
    addq.w  #1,d0
.Lceiling_loop:
    move.b  0(a4,d2.w),(a6)
    addq.l  #4,a6
    addq.w  #4,d2
    andi.w  #15,d2                 /* the flat pattern repeats every 4 rows */
    dbra    d5,.Lceiling_loop

.Lwall_setup:
    move.w  d4,d2                  /* min(bottom, end_y) */
    cmp.w   d1,d2
    bls.s   .Lwall_ready
    move.w  d1,d2
.Lwall_ready:
    cmp.w   d2,d0
    bcc.s   .Lfloor_setup
    movea.l 12(a1),a4              /* vertical sample DDA */
    move.w  d0,d5
    sub.w   d3,d5                  /* y - top: where this tile enters the DDA */
    adda.w  d5,a4
    move.b  17(a1),d4              /* texture vertical offset; bottom is dead */
    sub.w   d0,d2                  /* wall post length */
    add.w   d2,d0                  /* y after the post, for the floor setup */
    subq.w  #1,d2                  /* DBRA counter */
    moveq   #0,d5                  /* high byte stays clear across iterations */
.Lwall_loop:
    move.b  (a4)+,d5
    add.b   d4,d5
    andi.w  #63,d5
    move.b  0(a5,d5.w),(a6)
    addq.l  #4,a6
    dbra    d2,.Lwall_loop

.Lfloor_setup:
    cmp.w   d1,d0
    bcc.s   .Lnext_lane
    move.w  d1,d5                  /* floor post length */
    sub.w   d0,d5
    subq.w  #1,d5
    lea     16(a3,d6.w),a4         /* floor row bytes for this lane */
    move.w  d0,d2                  /* flat index = (y & 3) * 4 */
    andi.w  #3,d2
    lsl.w   #2,d2
.Lfloor_loop:
    move.b  0(a4,d2.w),(a6)
    addq.l  #4,a6
    addq.w  #4,d2
    andi.w  #15,d2
    dbra    d5,.Lfloor_loop

.Lnext_lane:
    adda.w  #22,a1
    addq.w  #1,d6
    cmpi.w  #4,d6
    bcs.w   .Lmixed_lane

    movem.l (sp)+,d2-d7/a2-a6
    rts
