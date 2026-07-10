#include "renderer_internal.h"
#include "generated_assets.h"
#include "generated_hud_assets.h"
#include "generated_renderer_assets.h"

u32 g_view_tiles[VIEW_TILE_COUNT][8];
u32 g_base_view_tiles[VIEW_TILE_COUNT][8];
u16 g_view_tilemap[VIEW_TILE_COUNT];
u16 g_compass_tilemap[COMPASS_W * COMPASS_H];
u32 g_view_dirty_bits[VIEW_DIRTY_WORD_COUNT];
u16 g_view_dirty_count;

static void init_video(void) {
    VDP_setScreenWidth320();
    VDP_setScreenHeight224();
    VDP_setHInterrupt(FALSE);
    VDP_setHilightShadow(FALSE);
    VDP_setTextPlane(BG_A);

    PAL_setColor(0, RGB24_TO_VDPCOLOR(0x000000));
    PAL_setColor(1, RGB24_TO_VDPCOLOR(0xD8D8D8));
    PAL_setColor(2, RGB24_TO_VDPCOLOR(0x181410));
    PAL_setColor(3, RGB24_TO_VDPCOLOR(0x383030));
    PAL_setColor(4, RGB24_TO_VDPCOLOR(0x585048));
    PAL_setColor(5, RGB24_TO_VDPCOLOR(0x888078));
    PAL_setColor(6, RGB24_TO_VDPCOLOR(0xB4ACA0));
    PAL_setColor(7, RGB24_TO_VDPCOLOR(0xE8E0D0));
    PAL_setColor(8, RGB24_TO_VDPCOLOR(0x301E10));
    PAL_setColor(9, RGB24_TO_VDPCOLOR(0x4878A8));
    PAL_setColor(10, RGB24_TO_VDPCOLOR(0x78502C));
    PAL_setColor(11, RGB24_TO_VDPCOLOR(0xD8B048));
    PAL_setColor(12, RGB24_TO_VDPCOLOR(0x982818));
    PAL_setColor(13, RGB24_TO_VDPCOLOR(0xA86838));
    PAL_setColor(14, RGB24_TO_VDPCOLOR(0x484038));
    PAL_setColor(15, RGB24_TO_VDPCOLOR(0x4C6028));

    // Palette line 1, foreground slot (index 15): Doom-red status-bar numerals.
    PAL_setColor(31, RGB24_TO_VDPCOLOR(0xF04028));

    // Palette line 2: dedicated skin/brown/red ramp for the Doom-guy portrait so
    // the face keeps proper flesh tones instead of going gold under PAL0.
    for (u16 i = 0; i < 16; i++) {
        PAL_setColor((u16)(32 + i), RGB24_TO_VDPCOLOR(FREEDOOM_FACE_PALETTE[i]));
    }

    // Palette line 3 is dedicated to the dynamic 3D view. Keeping it separate
    // from the status bar gives E1M1's walls, weapon and actors all 16 entries.
    for (u16 i = 0; i < 16; i++) {
        PAL_setColor((u16)(48 + i), RGB24_TO_VDPCOLOR(FREEDOOM_WORLD_PALETTE[i]));
    }

    VDP_clearPlane(BG_A, TRUE);
    VDP_clearPlane(BG_B, TRUE);
    VDP_setBackgroundColor(0);
}

static u32 make_pair_tile_row(u8 left_color, u8 right_color) {
    u32 row = 0;
    left_color &= 0x0F;
    right_color &= 0x0F;

    for (u16 x = 0; x < 4; x++) {
        row = (row << 4) | left_color;
    }

    for (u16 x = 0; x < 4; x++) {
        row = (row << 4) | right_color;
    }

    return row;
}

static void init_pair_tiles(void) {
    VDP_loadTileData((const u32 *)MEGALDOOM_PAIR_TILES, PAIR_TILE_BASE, PAIR_TILE_COUNT, DMA);
}

static void init_hud_tiles(void) {
    VDP_loadTileData((const u32 *)FREEDOOM_HUD_TILES, HUD_TILE_BASE, FREEDOOM_HUD_TILE_COUNT, DMA);
    VDP_loadTileData((const u32 *)FREEDOOM_FACE_TILES, FACE_TILE_BASE, FREEDOOM_FACE_TILE_COUNT, DMA);
}

static void init_view_tilemap(void) {
    for (u16 y = 0; y < VIEW_TILE_H; y++) {
        for (u16 x = 0; x < VIEW_TILE_W; x++) {
            const u16 index = (u16)((y * VIEW_TILE_W) + x);
            g_view_tilemap[index] = TILE_ATTR_FULL(PAL3, FALSE, FALSE, FALSE, VIEW_TILE_BASE + index);
        }
    }

    // The view tilemap is static (each cell points at a fixed tile index); only the
    // tile pixel data changes per frame. Upload the map once here so the per-frame
    // path can skip the redundant CPU tilemap copy.
    VDP_setTileMapDataRect(BG_B,
                           g_view_tilemap,
                           VIEW_TILEMAP_X,
                           VIEW_TILEMAP_Y,
                           VIEW_TILE_W,
                           VIEW_TILE_H,
                           VIEW_TILE_W,
                           CPU);
}

void renderer_mark_tile_dirty(u16 tile_index) {
    const u16 word = (u16)(tile_index >> 5);
    const u32 mask = (u32)1u << (tile_index & 31);

    if ((g_view_dirty_bits[word] & mask) == 0) {
        g_view_dirty_bits[word] |= mask;
        g_view_dirty_count++;
    }
}

void set_view_pair_tile(u16 x, u16 y, u8 left_color, u8 right_color) {
    const u32 row = make_pair_tile_row(left_color, right_color);
    const u16 map_index = (u16)((y * VIEW_TILE_W) + x);

    for (u16 row_index = 0; row_index < 8; row_index++) {
        g_view_tiles[map_index][row_index] = row;
    }
    renderer_mark_tile_dirty(map_index);
}

void set_view_column_color(u16 column, u16 y, u8 color) {
    const u16 tile_x = (u16)(column >> 3);
    const u16 tile_y = (u16)(y / 8);
    const u16 row_y = (u16)(y & 7);
    const u16 map_index = (u16)(tile_y * VIEW_TILE_W + tile_x);
    const u16 shift = (u16)((7 - (column & 7)) * 4);
    u32 row = g_view_tiles[map_index][row_y];

    row &= ~((u32)0x0F << shift);
    row |= ((u32)(color & 0x0F)) << shift;

    g_view_tiles[map_index][row_y] = row;
    renderer_mark_overlay_tile(map_index);
}

void renderer_set_bg_pair_tile(u16 x, u16 y, u8 left_color, u8 right_color) {
    const u16 pair_index = (u16)(((left_color & 0x0F) << 4) | (right_color & 0x0F));

    VDP_setTileMapXY(BG_B, TILE_ATTR_FULL(PAL0, FALSE, FALSE, FALSE, PAIR_TILE_BASE + pair_index), x, y);
}

void renderer_init(void) {
    init_video();
    init_pair_tiles();
    init_hud_tiles();
    init_view_tilemap();
    renderer_scene_init();
}
