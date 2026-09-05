#include "renderer_pack_internal.h"
#include "renderer_perf.h"
#include "generated_assets.h"

// 0 = ship the hand-written renderer_hotpath.s overlay posts; 1 = ship the C
// reference below. Same switch, and same purpose, as
// RENDERER_HOTPATH_C_REFERENCE in renderer_pack.c: the DEBUG_PERF harness at
// the bottom of this file runs BOTH implementations into guarded scratch every
// overlay column regardless of which one ships, so flipping this cannot disarm
// the check -- it only isolates which side a reported mismatch came from.
// Override with EXTRA_FLAGS="-DRENDERER_OVERLAY_C_REFERENCE=1".
#ifndef RENDERER_OVERLAY_C_REFERENCE
#define RENDERER_OVERLAY_C_REFERENCE 0
#endif

#if RAY_COL_STRIDE == 2
// Defined in renderer_hotpath.s, which assembles to nothing at any other
// stride; the argument offsets are OVL_ARG_*/SKY_ARG_* in renderer_pack_abi.h.
void renderer_write_overlay_frame_post_asm(u8 *dst, u16 row_count,
                                           const u8 *dda, const u8 *packed,
                                           u16 tex_y);
void renderer_write_overlay_sky_post_asm(u8 *dst, u16 row_count,
                                         const u8 *sky_bytes, u16 index);
#endif

// The door's interactive silhouette -- a dark metal frame plus a yellow/black
// moving safety edge around the real WAD texture -- used to be re-derived here
// per pixel by a style_wall_texel() helper. It is not gone, it moved: the same
// rules are baked into FREEDOOM_WALL_DOOR_PACKED_PAIRS by tools/world_assets.py,
// which packed_wall_column() selects for a RAY_COLUMN_FLAG_DOOR descriptor. The
// overlay now reads that table, so the frame costs one indexed byte load instead
// of two software 32-bit multiplies, a 2-D texture index and a branch chain.

// Screen rows a window's see-through band spans, as absolute viewport rows.
// bsp_draw_seg resolves the seg's Q8 band against the slab it just projected and
// stores absolute rows, so this is two clamps against the slab -- no multiply.
// Both consumers (this file's compositor and the billboard clip below) go
// through it so they can never disagree about which rows see through.
static void window_band_rows(u16 slab_top, u16 slab_bottom,
                             const RayDoorOverlay *window,
                             u16 *band_top, u16 *band_bottom) {
    u16 top = window->band_top;
    u16 bottom = window->band_bottom;
    if (top < slab_top) top = slab_top;
    if (bottom > slab_bottom) bottom = slab_bottom;
    if (bottom < top) bottom = top;
    *band_top = top;
    *band_bottom = bottom;
}

// Both posts below write ONE BYTE per screen row, to a pointer that steps by a
// constant PACK_TILE_ROW_BYTES. That is the same addressing renderer_hotpath.s
// uses and it rests on the same fact (AGENTS.md): the view tilemap is
// column-major, so a tile column's tiles are contiguous and screen row y of
// byte lane L sits at (y>>3)*32 + (y&7)*4 + L == 4*y + L from the top of the
// column. The old form recomputed view_tile_index() (a multiply) every row and
// composed the byte into a u32 with a VARIABLE 32-bit shift plus a
// read-modify-write -- and at stride 2 that whole dance provably resolves to
// this single byte store, because x is even, so shift is one of {24,16,8,0} and
// keep_mask is exactly the complement of byte (x&7)>>1. `lsl.l #24` alone is 56
// cycles on a 68000. See draw_door_overlays() for how the caller derives the
// starting pointer.

// The two reference bodies below take exactly the arguments the asm posts take,
// and nothing more: a resolved destination byte, an exact row count, and the
// DDA entry (or ceiling index) the first row reads. Holding both sides to one
// signature is what lets the harness at the bottom of this file run them
// against each other.

// The frame post: the near slab's own texture. It reads the SAME ROM table the
// base pack's wall post reads (packed_wall_column + wall_packed_y, one add and
// one mask), rather than re-deriving the texel from FREEDOOM_WALL_TEXTURES with
// wall_source_y()'s two 32-bit multiplies, a shade-LUT lookup and a REP2[]
// lookup. The 68000 has no 32x32 multiply, so each of those was a __mulsi3
// software loop, per pixel. Sharing the table also means an overlay renders a
// texture identically to the same texture drawn as an ordinary wall.
//
// It also fixes a real out-of-bounds read. wall_source_y() returns EARLY,
// unmasked, for a full-height texture, so it could yield tex_y up to
// 127 + tex_v_offset; sample_door_overlay then indexed
// FREEDOOM_WALL_TEXTURES[tid][tex_y] past that texture's 128 rows and into the
// next texture in the array. wall_packed_y() masks with WALL_TEX_HEIGHT_MASK,
// which is what every ordinary wall has always done. Reachable on four moving
// doors -- the only overlay segs with a nonzero tex_v_offset: E1M1 seg 261
// (BROWN96, offset 72) and E1M2 segs 48/52/933 (offsets 112/56/8).
#if DEBUG_PERF || RENDERER_OVERLAY_C_REFERENCE || RAY_COL_STRIDE != 2
static __attribute__((noinline)) void write_overlay_frame_post_reference(
    u8 *dst, u16 row_count, const u8 *dda, const u8 *packed, u16 tex_y) {
    for (u16 i = 0; i < row_count; i++) {
        const u8 pair = packed[(u16)((dda[i] + tex_y) & WALL_TEX_HEIGHT_MASK)];
#if RAY_COL_STRIDE == 4
        // One sampled column owns two adjacent byte lanes at this stride; the
        // packed byte is already 2px, so storing it to both spreads it over 4.
        dst[0] = pair;
        dst[1] = pair;
#else
        dst[0] = pair;
#endif
        dst += PACK_TILE_ROW_BYTES;
    }
}

// The window sky post: the base pack's ceiling loop. Same table layout
// (PACK_TILE_ROW_BYTES per row, PACK_CEILING_ROW_COUNT rows), same wrap by
// mask, same +4 walk -- this is renderer_hotpath.s's .Lceiling_loop with the
// destination lane already folded into `index` by the caller. See the LOG.md
// entry on the window pack-cost diagnosis (2026-08-29) for why the sky was
// split out of the frame loop in the first place.
static __attribute__((noinline)) void write_overlay_sky_post_reference(
    u8 *dst, u16 row_count, const u8 *sky_bytes, u16 index) {
    for (u16 i = 0; i < row_count; i++) {
        const u8 pair = sky_bytes[index];
#if RAY_COL_STRIDE == 4
        dst[0] = pair;
        dst[1] = pair;
#else
        dst[0] = pair;
#endif
        dst += PACK_TILE_ROW_BYTES;
        index = (u16)((index + PACK_TILE_ROW_BYTES) & PACK_CEILING_INDEX_MASK);
    }
}
#endif

#if RAY_COL_STRIDE == 2 && !RENDERER_OVERLAY_C_REFERENCE
#define write_overlay_frame_post renderer_write_overlay_frame_post_asm
#define write_overlay_sky_post renderer_write_overlay_sky_post_asm
#else
#define write_overlay_frame_post write_overlay_frame_post_reference
#define write_overlay_sky_post write_overlay_sky_post_reference
#endif

#if DEBUG_PERF
// Set only by the differential harness at the bottom of this file, to run the
// SAME argument resolution below through the other implementation. It exists so
// the harness exercises the production emitter rather than a second copy of it
// -- a copy is how a differential quietly stops being one. Compiles out
// entirely when DEBUG_PERF is off.
static bool s_overlay_use_reference;
#endif

// Resolve a post's [y_start, y_end) span onto the arguments both
// implementations take, then emit it. An empty post is dropped HERE, not inside
// the loops: the asm closes with DBRA, which on a zero count would wrap and run
// 65536 times.
static void paint_frame_rows(const WallColumnDescriptor *descriptor,
                             const u8 *packed, u16 lift_pixels,
                             u16 y_start, u16 y_end, u8 *col_base) {
    if (y_start >= y_end) return;
    // rel_y indexes the descriptor's vertical-sample DDA. It cannot run off the
    // end: y < y_end <= visible_bottom == bottom - lift_pixels, so
    // rel_y == y - top + lift_pixels < bottom - top. The old code carried a
    // runtime clamp for this; it was dead.
    const u16 rel_y = (u16)(y_start - descriptor->top + lift_pixels);
    u8 *const dst = col_base + ((u32)y_start * PACK_TILE_ROW_BYTES);
    const u16 rows = (u16)(y_end - y_start);
    const u8 *const dda = &descriptor->vertical_samples[rel_y];
#if DEBUG_PERF
    if (s_overlay_use_reference) {
        write_overlay_frame_post_reference(dst, rows, dda, packed,
                                           descriptor->tex_y);
        return;
    }
#endif
    write_overlay_frame_post(dst, rows, dda, packed, descriptor->tex_y);
}

static void paint_sky_rows(const u8 *sky_bytes, u16 lane, u16 y_start,
                           u16 y_end, u8 *col_base) {
    if (y_start >= y_end) return;
    const u16 index = (u16)((((u16)(y_start & (PACK_CEILING_ROW_COUNT - 1)))
                             * PACK_TILE_ROW_BYTES) + lane);
    u8 *const dst = col_base + ((u32)y_start * PACK_TILE_ROW_BYTES);
    const u16 rows = (u16)(y_end - y_start);
#if DEBUG_PERF
    if (s_overlay_use_reference) {
        write_overlay_sky_post_reference(dst, rows, sky_bytes, index);
        return;
    }
#endif
    write_overlay_sky_post(dst, rows, sky_bytes, index);
}

// The three monotonic posts a single overlay column emits, in order. Factored
// out so the DEBUG_PERF harness can replay exactly this sequence into scratch
// instead of reimplementing it: see compare_overlay_posts_asm below.
//
// [top, sky_start) is frame, [sky_start, sky_end) is sky, and
// [frame_resume, visible_bottom) resumes the frame after the band.
// [sky_end, frame_resume) is the pass-through gap -- the far scene the base
// pack already drew stays as-is, which is why it has no post at all rather
// than an empty one.
static void paint_overlay_column(const WallColumnDescriptor *descriptor,
                                 const u8 *packed, u16 lift_pixels, u16 lane,
                                 const u8 *sky_bytes, u16 sky_start,
                                 u16 sky_end, u16 frame_resume,
                                 u16 visible_bottom, u8 *col_base) {
    paint_frame_rows(descriptor, packed, lift_pixels,
                     descriptor->top, sky_start, col_base);
    paint_sky_rows(sky_bytes, lane, sky_start, sky_end, col_base);
    paint_frame_rows(descriptor, packed, lift_pixels,
                     frame_resume, visible_bottom, col_base);
}

#if DEBUG_PERF
// Differential harness for the two asm overlay posts, in the shape
// compare_stride2_column_asm established in renderer_pack.c -- and reshaped the
// same way for the same reason (LOG, 2026-08-04). BOTH sides are computed HERE,
// into guarded scratch, so whichever implementation ships cannot disarm the
// check. Taking the framebuffer draw_door_overlays just wrote as the "C side"
// would compare asm against asm the moment the asm became the shipped path,
// which is exactly the bug that let the pack harness report success for months.
//
// It replays paint_overlay_column, the production emitter, with only the
// destination and s_overlay_use_reference changed. So it verifies the loop
// bodies against each other while the argument resolution -- rel_y, the ceiling
// index, the empty-post drop that keeps DBRA off a zero count -- stays shared
// and single-sourced with the shipped path.
//
// Results land in the same asm_mismatches / asm_canary_failures /
// asm_checked_tiles counters the pack harness feeds; AGENTS.md names those
// three. A non-zero mismatch therefore means "an asm post disagrees with its C
// reference" without saying which harness: isolate by rebuilding with
// -DRENDERER_OVERLAY_C_REFERENCE=1 (silences this file's asm) or
// -DRENDERER_HOTPATH_C_REFERENCE=1 (silences the packer's).
//
// Every overlay column is checked, not one per frame like the pack harness:
// overlay columns are sparse and their post boundaries depend on the band, the
// lift and the wall behind, so a 1-in-20 cursor would sample the interesting
// cases rarely. The cost is a doubled overlay pass in a build that is already
// ~7x slower than release.
#define OVERLAY_PROBE_BYTES (VIEW_PIXEL_H * PACK_TILE_ROW_BYTES)
#define OVERLAY_PROBE_CANARY_A 0x51A7C0DEu
#define OVERLAY_PROBE_CANARY_B 0xC001D00Du
typedef struct {
    u32 before[2];
    u8 lane[OVERLAY_PROBE_BYTES];
    u32 after[2];
} OverlayPostProbe;

static OverlayPostProbe g_overlay_asm_probe;
static OverlayPostProbe g_overlay_c_probe;

static void overlay_probe_arm(OverlayPostProbe *probe) {
    probe->before[0] = OVERLAY_PROBE_CANARY_A;
    probe->before[1] = OVERLAY_PROBE_CANARY_B;
    probe->after[0] = OVERLAY_PROBE_CANARY_B;
    probe->after[1] = OVERLAY_PROBE_CANARY_A;
    for (u16 i = 0; i < OVERLAY_PROBE_BYTES; i++) probe->lane[i] = 0xA5u;
}

static bool overlay_probe_broken(const OverlayPostProbe *probe) {
    return (bool)(probe->before[0] != OVERLAY_PROBE_CANARY_A ||
                  probe->before[1] != OVERLAY_PROBE_CANARY_B ||
                  probe->after[0] != OVERLAY_PROBE_CANARY_B ||
                  probe->after[1] != OVERLAY_PROBE_CANARY_A);
}

static void compare_overlay_posts_asm(const WallColumnDescriptor *descriptor,
                                      const u8 *packed, u16 lift_pixels,
                                      u16 lane, const u8 *sky_bytes,
                                      u16 sky_start, u16 sky_end,
                                      u16 frame_resume, u16 visible_bottom,
                                      u16 tile_x) {
    overlay_probe_arm(&g_overlay_asm_probe);
    overlay_probe_arm(&g_overlay_c_probe);

    s_overlay_use_reference = FALSE;
    paint_overlay_column(descriptor, packed, lift_pixels, lane, sky_bytes,
                         sky_start, sky_end, frame_resume, visible_bottom,
                         &g_overlay_asm_probe.lane[lane]);
    s_overlay_use_reference = TRUE;
    paint_overlay_column(descriptor, packed, lift_pixels, lane, sky_bytes,
                         sky_start, sky_end, frame_resume, visible_bottom,
                         &g_overlay_c_probe.lane[lane]);
    s_overlay_use_reference = FALSE;

    bool mismatch = FALSE;
    for (u16 i = 0; i < OVERLAY_PROBE_BYTES; i++) {
        if (g_overlay_asm_probe.lane[i] != g_overlay_c_probe.lane[i]) {
            mismatch = TRUE;
        }
    }
    const bool canary_failure = (bool)(overlay_probe_broken(&g_overlay_asm_probe) ||
                                       overlay_probe_broken(&g_overlay_c_probe));
    renderer_perf_record_asm_compare(tile_x, mismatch, canary_failure, FALSE);
}
#endif

// Moving doors and windows are both translucent only in the geometric sense:
// part of the near slab is omitted so the fully rendered BSP scene behind
// shows through, while the rest is composited at its own depth without
// rescaling its texture. A door omits a raised gap at the BOTTOM; a window
// omits a band in the MIDDLE.
void draw_door_overlays(const RayColumn *columns,
                        const RaySceneColors *scene_colors,
                        u32 target[][8]) {
    for (u16 x = 0; x < RAY_VIEW_COLS; x += RAY_COL_STRIDE) {
        const RayColumn *column = &columns[x];
        const RayDoorOverlay *door = &column->door;
        if (door->height == 0 || door->depth >= column->depth) continue;

        const WallColumnDescriptor descriptor = describe_door_overlay(door);
        const bool window = ray_overlay_is_window(door);
        const u16 lift_pixels = window ? 0u :
            (u16)(((u32)door->height * door->lift) >> 8);
        const u16 visible_bottom = (u16)(descriptor.bottom - lift_pixels);
        if (visible_bottom <= descriptor.top) continue;

        u16 band_top = visible_bottom;
        u16 band_bottom = visible_bottom;
        u16 sky_bottom = 0;
        if (window) {
            window_band_rows(descriptor.top, descriptor.bottom, door,
                             &band_top, &band_bottom);
            // Inside the band the base pack already drew the far geometry, but
            // its ceiling run used the PLAYER's sector -- indoors, looking out.
            // Repaint just the ceiling part of the band with the sky. This is
            // the only per-column ceiling in the renderer, and it stays here in
            // C rather than reaching the asm pack post.
            // A normal far wall is centred, so avoid building a descriptor just
            // to read its top. A courtyard parapet is floor-aligned, however;
            // its `height` alone cannot recover that top, and the rare window
            // view must use the same descriptor calculation as the base pack.
            sky_bottom = (column->flags & RAY_COLUMN_FLAG_FLOOR_ALIGNED) ?
                describe_wall_column(column).top :
                (u16)((VIEW_PIXEL_H - column->height) / 2);
            if (sky_bottom > band_bottom) sky_bottom = band_bottom;
        }

        const u16 tile_x = (u16)(x >> 3);
#if RAY_COL_STRIDE == 4
        const u16 lane = (u16)((x & 4) ? 2 : 0);
#else
        const u16 lane = (u16)((x & 7) >> 1);
#endif
        // Byte address of screen row 0 in this sampled column's lane. Both posts
        // step it by PACK_TILE_ROW_BYTES per row; see the note above them for
        // why the tile boundary is invisible to that walk.
        u8 *const col_base =
            (u8 *)&target[view_tile_index(tile_x, 0)][0] + lane;
        // The ROM pair column this slab samples -- the same table, and the same
        // door/wall selection, the base pack's wall post uses.
        const u8 *const packed = packed_wall_column(&descriptor);
        // The same sky column the pack stage would use for this tile if the
        // player were standing outside. Looking out through a window and then
        // walking out there has to show the same piece of horizon, so both
        // paths go through sky_column_rows with the same heading offset.
        const u8 *const sky_bytes =
            (const u8 *)sky_column_rows(tile_x, scene_colors->sky_offset);

        // Three monotonic posts instead of a per-row "frame or sky" branch;
        // paint_overlay_column above emits them and documents the split.
        //
        // For a door (not a window), band_top/band_bottom/sky_bottom are all
        // still their see-nothing defaults from above (band_top == band_bottom
        // == visible_bottom, sky_bottom == 0), and every clamp below reduces
        // to sky_start == sky_end == frame_resume == visible_bottom: one frame
        // post spanning the whole column, byte-for-byte what a door painted
        // before this split existed.
        u16 sky_start = band_top;
        if (sky_start < descriptor.top) sky_start = descriptor.top;
        if (sky_start > visible_bottom) sky_start = visible_bottom;

        u16 sky_end = band_bottom;
        if (sky_end > sky_bottom) sky_end = sky_bottom;
        if (sky_end < sky_start) sky_end = sky_start;
        if (sky_end > visible_bottom) sky_end = visible_bottom;

        u16 frame_resume = band_bottom;
        if (frame_resume < sky_end) frame_resume = sky_end;
        if (frame_resume > visible_bottom) frame_resume = visible_bottom;

        paint_overlay_column(&descriptor, packed, lift_pixels, lane, sky_bytes,
                             sky_start, sky_end, frame_resume, visible_bottom,
                             col_base);
#if DEBUG_PERF
        compare_overlay_posts_asm(&descriptor, packed, lift_pixels, lane,
                                  sky_bytes, sky_start, sky_end, frame_resume,
                                  visible_bottom, tile_x);
#endif
    }
}

bool door_overlay_blocks_pixel(const RayColumn *column,
                               u16 object_depth,
                               u16 y) {
    const RayDoorOverlay *door = &column->door;
    if (door->height == 0 || door->depth >= column->depth ||
        object_depth < door->depth) {
        return FALSE;
    }
    const u16 top = (u16)((VIEW_PIXEL_H - door->height) / 2);
    const u16 bottom = (u16)(top + door->height);
    if (ray_overlay_is_window(door)) {
        // Mirror draw_door_overlays: the band is see-through, so a billboard
        // behind a window stays visible through it instead of being clipped
        // by the frame.
        //
        // `top`/`bottom` above are bit-identical to the descriptor
        // describe_door_overlay() would build -- describe_textured_column()
        // computes exactly (VIEW_PIXEL_H - wall_h) / 2 from the same
        // door->height -- so the slab bounds are passed straight in. That
        // matters: the three billboard raster sites call this once PER BYTE PER
        // ROW of every sprite overlapping an overlay column, and building a
        // 20-byte WallColumnDescriptor there (a [641][120] table index, a fog
        // level, a struct returned by value) to read back two u16s it already
        // has was the whole cost.
        u16 band_top, band_bottom;
        window_band_rows(top, bottom, door, &band_top, &band_bottom);
        if (y >= band_top && y < band_bottom) return FALSE;
        return (bool)(y >= top && y < bottom);
    }
    const u16 lift_pixels = (u16)(((u32)door->height * door->lift) >> 8);
    return (bool)(y >= top && y < (u16)(bottom - lift_pixels));
}
