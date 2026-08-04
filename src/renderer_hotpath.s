    .text
    .align  2
    .globl  renderer_write_mixed_stride2_tile_asm

/*
 * void renderer_write_mixed_stride2_tile_asm(
 *     u32 *tile, u16 pixel_y,
 *     const WallColumnDescriptor descriptors[4],
 *     const u8 *const packed_columns[4],
 *     const PackedFlatRows *flat_rows);
 *
 * GCC's m68k ABI gives the u16 argument a four-byte stack slot and right-aligns
 * the word within it. After saving d2-d7/a2-a6 (44 bytes), pointers are at
 * sp+48,56,60,64 and pixel_y is the word at sp+54.
 * WallColumnDescriptor is 22 bytes: top=0, bottom=2, vertical_samples=12,
 * tex_y=17. PackedFlatRows has ceiling at 0 and floor at 16.
 *
 * One mixed tile is four byte lanes. Each lane is emitted as at most three
 * monotonic posts (ceiling, wall, floor), so the inner loops contain no
 * per-pixel material branch and no 32-bit shift/or composition.
 *
 * Each post's length is computed once and the loop closed with DBRA. The
 * previous form re-tested both the post bound and end_y every pixel
 * (cmp/bcc/cmp/bcs = 26 cycles) where DBRA costs 10; the wall post additionally
 * re-read tex_y from the descriptor every pixel, which is now held in d4 (the
 * `bottom` it replaces is dead once the post length is known). Wall post
 * measured ~104 -> ~72 cycles per byte. Mixed tiles are the pack stage's
 * dominant cost (~42 subticks each over a flat tile, and a rotation doubles
 * their count: 149/rebuild spinning vs 75 translating).
 */
renderer_write_mixed_stride2_tile_asm:
    movem.l d2-d7/a2-a6,-(sp)
    movea.l 48(sp),a0              /* tile base */
    move.w  54(sp),d7              /* first screen y (right-aligned u16) */
    movea.l 56(sp),a1              /* descriptor */
    movea.l 60(sp),a2              /* packed-column pointer table */
    movea.l 64(sp),a3              /* flat rows */
    moveq   #0,d6                  /* byte lane 0..3 */

.Lmixed_lane:
    movea.l (a2)+,a5               /* preshaded wall pairs */
    movea.l a0,a6
    adda.w  d6,a6                  /* tile byte for this lane */
    move.w  d7,d0                  /* y */
    move.w  d7,d1
    addq.w  #8,d1                  /* end_y */
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
.Lceiling_loop:
    move.w  d0,d2
    andi.w  #3,d2
    lsl.w   #2,d2
    add.w   d6,d2
    move.b  0(a3,d2.w),0(a6)
    addq.l  #4,a6
    addq.w  #1,d0
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
    sub.w   d0,d2                  /* wall post length */
    subq.w  #1,d2
    sub.w   d3,d0                  /* d0 = y - top: the DDA index */
    move.b  17(a1),d4              /* texture vertical offset; bottom is dead */
.Lwall_loop:
    moveq   #0,d5
    move.b  0(a4,d0.w),d5
    add.b   d4,d5
    andi.w  #63,d5
    move.b  0(a5,d5.w),0(a6)
    addq.l  #4,a6
    addq.w  #1,d0
    dbra    d2,.Lwall_loop
    add.w   d3,d0                  /* back to an absolute y for the floor post */

.Lfloor_setup:
    cmp.w   d1,d0
    bcc.s   .Lnext_lane
    move.w  d1,d5                  /* floor post length */
    sub.w   d0,d5
    subq.w  #1,d5
.Lfloor_loop:
    move.w  d0,d2
    andi.w  #3,d2
    lsl.w   #2,d2
    add.w   d6,d2
    move.b  16(a3,d2.w),0(a6)
    addq.l  #4,a6
    addq.w  #1,d0
    dbra    d5,.Lfloor_loop

.Lnext_lane:
    adda.w  #22,a1
    addq.w  #1,d6
    cmpi.w  #4,d6
    bcs.w   .Lmixed_lane

    movem.l (sp)+,d2-d7/a2-a6
    rts
