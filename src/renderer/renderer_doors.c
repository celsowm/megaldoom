#include "renderer_pack_internal.h"
#include "generated_assets.h"

#define DOOR_FRAME_TEXELS (WALL_TEX_WIDTH / 16)

// Preserve a stable interactive silhouette around the original BIGDOOR art: a
// dark metal frame plus a yellow/black moving safety edge, with the real WAD
// texture in the centre. This is stride-independent -- it works in texel space,
// not in sampled-column space -- so it is one function, not one per stride.
static u8 style_wall_texel(const WallColumnDescriptor *descriptor,
                           u8 tex_y,
                           u8 texel) {
    if ((descriptor->flags & RAY_COLUMN_FLAG_DOOR) == 0) return texel;

    const u8 frame_height = (u8)(descriptor->texture_height / 16);
    const u8 safety = (u8)(descriptor->texture_height / 8);
    if (descriptor->tex_x < DOOR_FRAME_TEXELS ||
        descriptor->tex_x >= (WALL_TEX_WIDTH - DOOR_FRAME_TEXELS) ||
        tex_y < frame_height) {
        return 0;
    }
    if (tex_y >= (descriptor->texture_height - safety)) {
        return (descriptor->tex_x & safety) ?
                   MEGALDOOM_WORLD_COLOR_WARNING : 0;
    }
    return texel;
}

static u8 sample_door_overlay(const WallColumnDescriptor *descriptor,
                              u16 y,
                              u16 lift_pixels) {
    u16 source_y = (u16)(y - descriptor->top + lift_pixels);
    const u16 full_height = (u16)(descriptor->bottom - descriptor->top);
    if (source_y >= full_height) source_y = (u16)(full_height - 1);
    const u8 tex_y = (u8)wall_source_y(descriptor, source_y);
    const u8 texel = descriptor->texture[(tex_y * WALL_TEX_WIDTH) +
                                          descriptor->tex_x] & 0x0F;
    return descriptor->shade_map[style_wall_texel(descriptor, tex_y, texel)];
}

// Screen rows a window's see-through band spans, as absolute viewport rows.
// The band arrives as Q8 fractions of the drawn slab (tools/doom_map.py's
// window_band), because the engine has no per-sector heights to project.
static void window_band_rows(const WallColumnDescriptor *descriptor,
                             const RayDoorOverlay *window,
                             u16 *band_top, u16 *band_bottom) {
    const u16 height = (u16)(descriptor->bottom - descriptor->top);
    u16 top = (u16)(descriptor->top + (((u32)height * window->band_top) >> 8));
    u16 bottom = (u16)(descriptor->top + (((u32)height * window->band_bottom) >> 8));
    if (top < descriptor->top) top = descriptor->top;
    if (bottom > descriptor->bottom) bottom = descriptor->bottom;
    if (bottom < top) bottom = top;
    *band_top = top;
    *band_bottom = bottom;
}

// One frame post: texture-sampled, same as the whole column used to be
// before this was split. Pulled out so both the pre-band and post-band frame
// ranges share one body instead of two copies of a per-pixel loop.
static void paint_frame_rows(const WallColumnDescriptor *descriptor, u16 lift_pixels,
                             u16 y_start, u16 y_end, u16 tile_x,
                             u16 shift, u32 keep_mask, u32 target[][8]) {
    for (u16 y = y_start; y < y_end; y++) {
        const u8 color = sample_door_overlay(descriptor, y, lift_pixels);
#if RAY_COL_STRIDE == 4
        const u32 value = (u32)REP4[color] << shift;
#else
        const u32 value = (u32)REP2[color] << shift;
#endif
        u32 *row = &target[view_tile_index(tile_x, (u16)(y >> 3))][y & 7];
        *row = (*row & keep_mask) | value;
    }
}

// The window sky post: a plain per-row table lookup with no per-pixel branch,
// sharing its table layout (4 bytes per row, PACK_CEILING_ROW_COUNT wrap)
// with the ceiling post the base pack's asm hot path already runs -- see the
// LOG.md entry on the window pack-cost diagnosis (2026-08-29) for why this
// was split out of the frame loop: a same-pose, heading-only A/B showed this
// loop alone was ~91% of a window's extra pack-stage cost over a plain wall,
// because it used to re-decide "frame or sky" every row instead of once per
// post. Isolated here so it can become the asm hot path's next post without
// reshaping this function again.
static void paint_sky_rows(const u8 *sky_bytes, u16 lane, u16 y_start, u16 y_end,
                           u16 tile_x, u16 shift, u32 keep_mask, u32 target[][8]) {
    for (u16 y = y_start; y < y_end; y++) {
#if RAY_COL_STRIDE == 4
        const u8 pair = sky_bytes[(((u16)(y & (PACK_CEILING_ROW_COUNT - 1))) << 2) + lane];
        const u32 value = (u32)((pair << 8) | pair) << shift;
#else
        const u32 value = (u32)sky_bytes[(((u16)(y & (PACK_CEILING_ROW_COUNT - 1))) << 2) + lane] << shift;
#endif
        u32 *row = &target[view_tile_index(tile_x, (u16)(y >> 3))][y & 7];
        *row = (*row & keep_mask) | value;
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
            window_band_rows(&descriptor, door, &band_top, &band_bottom);
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
        const u16 shift = (u16)((x & 4) ? 0 : 16);
        const u32 keep_mask = ~((u32)0xFFFFu << shift);
        const u16 lane = (u16)((x & 4) ? 2 : 0);
#else
        const u16 shift = (u16)((6 - (x & 7)) * 4);
        const u32 keep_mask = ~((u32)0xFFu << shift);
        const u16 lane = (u16)((x & 7) >> 1);
#endif
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

        paint_frame_rows(&descriptor, lift_pixels, descriptor.top, sky_start,
                         tile_x, shift, keep_mask, target);
        paint_sky_rows(sky_bytes, lane, sky_start, sky_end, tile_x, shift,
                       keep_mask, target);
        paint_frame_rows(&descriptor, lift_pixels, frame_resume, visible_bottom,
                         tile_x, shift, keep_mask, target);
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
        const WallColumnDescriptor descriptor = describe_door_overlay(door);
        u16 band_top, band_bottom;
        window_band_rows(&descriptor, door, &band_top, &band_bottom);
        if (y >= band_top && y < band_bottom) return FALSE;
        return (bool)(y >= top && y < bottom);
    }
    const u16 lift_pixels = (u16)(((u32)door->height * door->lift) >> 8);
    return (bool)(y >= top && y < (u16)(bottom - lift_pixels));
}
