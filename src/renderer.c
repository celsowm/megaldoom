#include "renderer.h"
#include "fixed_math.h"
#include "generated_assets.h"

#define VERSION_TEXT "MEGALDOOM REWRITE GATE 9"
#define VIEW_TILE_W 16
#define VIEW_TILE_H 12
#define VIEW_TILE_COUNT (VIEW_TILE_W * VIEW_TILE_H)
#define VIEW_TILE_BASE TILE_USER_INDEX
#define SOLID_TILE_COUNT 16
#define VIEW_TILEMAP_X 12
#define VIEW_TILEMAP_Y 8
#define COMPASS_X 4
#define COMPASS_Y 8
#define COMPASS_W 5
#define COMPASS_H 5

static const char HEX_DIGITS[] = "0123456789ABCDEF";
static u32 g_solid_tiles[SOLID_TILE_COUNT][8];
static u16 g_view_tilemap[VIEW_TILE_COUNT];
static u16 g_compass_tilemap[COMPASS_W * COMPASS_H];
static u8 g_wall_tex_y_by_height[VIEW_TILE_H + 1][VIEW_TILE_H];

static const u8 DOOR_TEXTURE[8][8] = {
    {12, 13, 12, 13, 12, 13, 12, 13},
    {13, 7, 13, 12, 13, 7, 13, 12},
    {12, 13, 12, 13, 12, 13, 12, 13},
    {13, 12, 13, 7, 13, 12, 13, 7},
    {12, 13, 12, 13, 12, 13, 12, 13},
    {13, 7, 13, 12, 13, 7, 13, 12},
    {12, 13, 12, 13, 12, 13, 12, 13},
    {13, 12, 13, 7, 13, 12, 13, 7},
};

static void write_hex_u32(u32 value, char *out) {
    for (u16 i = 0; i < 8; i++) {
        const u16 shift = (u16)((7 - i) * 4);
        out[i] = HEX_DIGITS[(value >> shift) & 0x0F];
    }
    out[8] = '\0';
}

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

static u32 make_solid_tile_row(u8 color) {
    u32 row = 0;
    color &= 0x0F;

    for (u16 x = 0; x < 8; x++) {
        row = (row << 4) | color;
    }

    return row;
}

static void init_solid_tiles(void) {
    for (u16 color = 0; color < SOLID_TILE_COUNT; color++) {
        const u32 row = make_solid_tile_row((u8)color);

        for (u16 y = 0; y < 8; y++) {
            g_solid_tiles[color][y] = row;
        }
    }

    VDP_loadTileData((const u32 *)g_solid_tiles, VIEW_TILE_BASE, SOLID_TILE_COUNT, DMA);
}

static void init_wall_sampling_table(void) {
    for (u16 height = 1; height <= VIEW_TILE_H; height++) {
        for (u16 rel_y = 0; rel_y < VIEW_TILE_H; rel_y++) {
            g_wall_tex_y_by_height[height][rel_y] = (u8)((rel_y * 8) / height);
        }
    }
}

static void set_view_tile(u16 x, u16 y, u8 color) {
    g_view_tilemap[(y * VIEW_TILE_W) + x] =
        TILE_ATTR_FULL(PAL0, FALSE, FALSE, FALSE, VIEW_TILE_BASE + (color & 0x0F));
}

static void build_raycast_tilemap(const RayColumn *columns) {
    for (u16 y = 0; y < VIEW_TILE_H; y++) {
        const u8 bg = (y < (VIEW_TILE_H / 2)) ? 9 : 14;

        for (u16 x = 0; x < VIEW_TILE_W; x++) {
            set_view_tile(x, y, bg);
        }
    }

    for (u16 col = 0; col < RAY_VIEW_COLS; col++) {
        const u16 wall_h = columns[col].height;
        const u16 top = (u16)((VIEW_TILE_H - wall_h) / 2);
        const u16 bottom = (u16)(top + wall_h);
        const u8 (*texture)[8] = (columns[col].color == 12) ? DOOR_TEXTURE : FREEDOOM_WALL_TEXTURE;
        const u8 *tex_y_table = g_wall_tex_y_by_height[wall_h];

        for (u16 y = top; y < bottom && y < VIEW_TILE_H; y++) {
            const u16 rel_y = (u16)(y - top);
            set_view_tile(col, y, texture[tex_y_table[rel_y]][columns[col].tex_x & 7]);
        }
    }
}

static void upload_view_tilemap(void) {
    VDP_setTileMapDataRect(BG_B,
                           g_view_tilemap,
                           VIEW_TILEMAP_X,
                           VIEW_TILEMAP_Y,
                           VIEW_TILE_W,
                           VIEW_TILE_H,
                           VIEW_TILE_W,
                           CPU);
}

static void clear_compass_tilemap(void) {
    for (u16 i = 0; i < (COMPASS_W * COMPASS_H); i++) {
        g_compass_tilemap[i] = TILE_ATTR_FULL(PAL0, FALSE, FALSE, FALSE, VIEW_TILE_BASE);
    }
}

static void set_compass_tile(s16 x, s16 y, u8 color) {
    if ((x < 0) || (y < 0) || (x >= COMPASS_W) || (y >= COMPASS_H)) {
        return;
    }

    g_compass_tilemap[(y * COMPASS_W) + x] =
        TILE_ATTR_FULL(PAL0, FALSE, FALSE, FALSE, VIEW_TILE_BASE + (color & 0x0F));
}

static void build_compass_tilemap(u16 angle) {
    const s16 vx = fx_cos(angle);
    const s16 vy = fx_sin(angle);
    const s16 dot_x = (s16)(2 + ((vx * 2) >> FX_SHIFT));
    const s16 dot_y = (s16)(2 + ((vy * 2) >> FX_SHIFT));

    clear_compass_tilemap();
    set_compass_tile(2, 0, 7);
    set_compass_tile(0, 2, 7);
    set_compass_tile(2, 2, 11);
    set_compass_tile(4, 2, 7);
    set_compass_tile(2, 4, 7);
    set_compass_tile(dot_x, dot_y, 13);
}

static void upload_compass_tilemap(void) {
    VDP_setTileMapDataRect(BG_B,
                           g_compass_tilemap,
                           COMPASS_X,
                           COMPASS_Y,
                           COMPASS_W,
                           COMPASS_H,
                           COMPASS_W,
                           CPU);
}

void renderer_init(void) {
    init_video();
    init_solid_tiles();
    init_wall_sampling_table();
}

void renderer_draw_static_screen(void) {
    VDP_drawText(VERSION_TEXT, 7, 2);
    VDP_drawText("TEXTURE SAMPLING TABLE", 8, 4);
    VDP_drawText("DPAD MOVE/TURN  A/C STRAFE", 6, 22);
    VDP_drawText("FRAME", 13, 25);
}

void renderer_render_scene(const RayColumn *columns, u16 angle) {
    build_raycast_tilemap(columns);
    upload_view_tilemap();
    build_compass_tilemap(angle);
    upload_compass_tilemap();
}

void renderer_draw_frame_counter(u32 frame) {
    char frame_text[9];

    write_hex_u32(frame, frame_text);
    VDP_drawText(frame_text, 19, 25);
}
