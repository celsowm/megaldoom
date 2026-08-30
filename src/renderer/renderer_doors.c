#include "renderer_pack_internal.h"
#include "generated_assets.h"

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

// One frame post: the near slab's own texture. It reads the SAME ROM table the
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
static void paint_frame_rows(const WallColumnDescriptor *descriptor,
                             const u8 *packed, u16 lift_pixels,
                             u16 y_start, u16 y_end, u8 *col_base) {
    if (y_start >= y_end) return;
    // rel_y indexes the descriptor's vertical-sample DDA. It cannot run off the
    // end: y < y_end <= visible_bottom == bottom - lift_pixels, so
    // rel_y == y - top + lift_pixels < bottom - top. The old code carried a
    // runtime clamp for this; it was dead.
    u16 rel_y = (u16)(y_start - descriptor->top + lift_pixels);
    u8 *dst = col_base + ((u32)y_start * PACK_TILE_ROW_BYTES);
    for (u16 y = y_start; y < y_end; y++) {
        const u8 pair = packed[wall_packed_y(descriptor, rel_y)];
#if RAY_COL_STRIDE == 4
        // One sampled column owns two adjacent byte lanes at this stride; the
        // packed byte is already 2px, so storing it to both spreads it over 4.
        dst[0] = pair;
        dst[1] = pair;
#else
        dst[0] = pair;
#endif
        dst += PACK_TILE_ROW_BYTES;
        rel_y++;
    }
}

// The window sky post: the base pack's ceiling loop, in C. Same table layout
// (PACK_TILE_ROW_BYTES per row, PACK_CEILING_ROW_COUNT rows), same wrap by
// mask, same +4 walk -- this is renderer_hotpath.s's .Lceiling_loop with the
// destination lane fixed by the caller. See the LOG.md entry on the window
// pack-cost diagnosis (2026-08-29) for why the sky was split out of the frame
// loop in the first place.
static void paint_sky_rows(const u8 *sky_bytes, u16 lane, u16 y_start, u16 y_end,
                           u8 *col_base) {
    if (y_start >= y_end) return;
    u16 index = (u16)((((u16)(y_start & (PACK_CEILING_ROW_COUNT - 1)))
                       * PACK_TILE_ROW_BYTES) + lane);
    u8 *dst = col_base + ((u32)y_start * PACK_TILE_ROW_BYTES);
    for (u16 y = y_start; y < y_end; y++) {
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
            const WallColumnDescriptor behind = describe_wall_column(column);
            sky_bottom = behind.top;
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

        // Three monotonic posts instead of a per-row "frame or sky" branch:
        // [top, sky_start) is frame, [sky_start, sky_end) is sky, and
        // [frame_resume, visible_bottom) resumes the frame after the band.
        // [sky_end, frame_resume) is the pass-through gap (the far scene the
        // base pack already drew stays as-is -- nothing to paint), which is
        // why it has no post at all rather than an empty one.
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

        paint_frame_rows(&descriptor, packed, lift_pixels,
                         descriptor.top, sky_start, col_base);
        paint_sky_rows(sky_bytes, lane, sky_start, sky_end, col_base);
        paint_frame_rows(&descriptor, packed, lift_pixels,
                         frame_resume, visible_bottom, col_base);
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
