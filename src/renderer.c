#include "renderer_internal.h"
#include "generated_hud_assets.h"

u32 g_pair_tiles[PAIR_TILE_COUNT][8];
u32 g_view_tiles[VIEW_TILE_COUNT][8];
u16 g_view_tilemap[VIEW_TILE_COUNT];
u16 g_compass_tilemap[COMPASS_W * COMPASS_H];
u8 g_wall_tex_y_by_height[VIEW_PIXEL_H + 1][VIEW_PIXEL_H];

static void init_video(void) {
    VDP_setScreenWidth320();
    VDP_setScreenHeight224();
    VDP_setHInterrupt(FALSE);
    VDP_setHilightShadow(FALSE);
    VDP_setTextPlane(BG_A);

    PAL_setColor(0, RGB24_TO_VDPCOLOR(0x000000));
    PAL_setColor(1, RGB24_TO_VDPCOLOR(0xE0E0E0));
    PAL_setColor(2, RGB24_TO_VDPCOLOR(0x202020));
    PAL_setColor(3, RGB24_TO_VDPCOLOR(0x404040));
    PAL_setColor(4, RGB24_TO_VDPCOLOR(0x686868));
    PAL_setColor(5, RGB24_TO_VDPCOLOR(0x909090));
    PAL_setColor(6, RGB24_TO_VDPCOLOR(0xB8B8B8));
    PAL_setColor(7, RGB24_TO_VDPCOLOR(0xE0E0E0));
    PAL_setColor(8, RGB24_TO_VDPCOLOR(0x203050));
    PAL_setColor(9, RGB24_TO_VDPCOLOR(0x3068A0));
    PAL_setColor(10, RGB24_TO_VDPCOLOR(0x50A0D0));
    PAL_setColor(11, RGB24_TO_VDPCOLOR(0xE0E070));
    PAL_setColor(12, RGB24_TO_VDPCOLOR(0x904040));
    PAL_setColor(13, RGB24_TO_VDPCOLOR(0xC06848));
    PAL_setColor(14, RGB24_TO_VDPCOLOR(0x305020));
    PAL_setColor(15, RGB24_TO_VDPCOLOR(0x507030));

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
    for (u16 left = 0; left < 16; left++) {
        for (u16 right = 0; right < 16; right++) {
            const u16 tile = (u16)((left << 4) | right);
            const u32 row = make_pair_tile_row((u8)left, (u8)right);

            for (u16 y = 0; y < 8; y++) {
                g_pair_tiles[tile][y] = row;
            }
        }
    }

    VDP_loadTileData((const u32 *)g_pair_tiles, PAIR_TILE_BASE, PAIR_TILE_COUNT, DMA);
}

static void init_hud_tiles(void) {
    VDP_loadTileData((const u32 *)FREEDOOM_HUD_TILES, HUD_TILE_BASE, FREEDOOM_HUD_TILE_COUNT, DMA);
}

static void init_wall_sampling_table(void) {
    for (u16 height = 1; height <= VIEW_PIXEL_H; height++) {
        for (u16 rel_y = 0; rel_y < VIEW_PIXEL_H; rel_y++) {
            g_wall_tex_y_by_height[height][rel_y] = (u8)((rel_y * 16) / height);
        }
    }
}

static void init_view_tilemap(void) {
    for (u16 y = 0; y < VIEW_TILE_H; y++) {
        for (u16 x = 0; x < VIEW_TILE_W; x++) {
            const u16 index = (u16)((y * VIEW_TILE_W) + x);
            g_view_tilemap[index] = TILE_ATTR_FULL(PAL0, FALSE, FALSE, FALSE, VIEW_TILE_BASE + index);
        }
    }
}

void set_view_pair_tile(u16 x, u16 y, u8 left_color, u8 right_color) {
    const u32 row = make_pair_tile_row(left_color, right_color);
    const u16 map_index = (u16)((y * VIEW_TILE_W) + x);

    for (u16 row_index = 0; row_index < 8; row_index++) {
        g_view_tiles[map_index][row_index] = row;
    }
}

void set_view_column_color(u16 column, u16 y, u8 color) {
    const u16 tile_x = (u16)(column / 2);
    const u16 tile_y = (u16)(y / 8);
    const u16 row_y = (u16)(y & 7);
    const u16 map_index = (u16)(tile_y * VIEW_TILE_W + tile_x);
    const u32 row = g_view_tiles[map_index][row_y];
    u8 left_color = (u8)((row >> 28) & 0x0F);
    u8 right_color = (u8)((row >> 12) & 0x0F);

    if ((column & 1) == 0) {
        left_color = color;
    } else {
        right_color = color;
    }

    g_view_tiles[map_index][row_y] = make_pair_tile_row(left_color, right_color);
}

void renderer_set_bg_pair_tile(u16 x, u16 y, u8 left_color, u8 right_color) {
    const u16 pair_index = (u16)(((left_color & 0x0F) << 4) | (right_color & 0x0F));

    VDP_setTileMapXY(BG_B, TILE_ATTR_FULL(PAL0, FALSE, FALSE, FALSE, PAIR_TILE_BASE + pair_index), x, y);
}

void renderer_init(void) {
    init_video();
    init_pair_tiles();
    init_hud_tiles();
    init_wall_sampling_table();
    init_view_tilemap();
}
