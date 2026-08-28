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

// Moving doors are translucent only in the geometric sense: their raised
// lower gap reveals the fully rendered BSP scene behind, while the remaining
// slab is composited at its own depth without rescaling its texture.
void draw_door_overlays(const RayColumn *columns, u32 target[][8]) {
    for (u16 x = 0; x < RAY_VIEW_COLS; x += RAY_COL_STRIDE) {
        const RayColumn *column = &columns[x];
        const RayDoorOverlay *door = &column->door;
        if (door->height == 0 || door->depth >= column->depth) continue;

        const WallColumnDescriptor descriptor = describe_door_overlay(door);
        const u16 lift_pixels = (u16)(((u32)door->height * door->lift) >> 8);
        const u16 visible_bottom = (u16)(descriptor.bottom - lift_pixels);
        if (visible_bottom <= descriptor.top) continue;

        const u16 tile_x = (u16)(x >> 3);
#if RAY_COL_STRIDE == 4
        const u16 shift = (u16)((x & 4) ? 0 : 16);
        const u32 keep_mask = ~((u32)0xFFFFu << shift);
#else
        const u16 shift = (u16)((6 - (x & 7)) * 4);
        const u32 keep_mask = ~((u32)0xFFu << shift);
#endif
        for (u16 y = descriptor.top; y < visible_bottom; y++) {
            const u8 color = sample_door_overlay(&descriptor, y, lift_pixels);
#if RAY_COL_STRIDE == 4
            const u32 value = (u32)REP4[color] << shift;
#else
            const u32 value = (u32)REP2[color] << shift;
#endif
            u32 *row = &target[view_tile_index(tile_x, (u16)(y >> 3))][y & 7];
            *row = (*row & keep_mask) | value;
        }
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
    const u16 lift_pixels = (u16)(((u32)door->height * door->lift) >> 8);
    return (bool)(y >= top && y < (u16)(top + door->height - lift_pixels));
}
