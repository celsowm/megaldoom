#include "renderer.h"
#include "billboard.h"
#include "fixed_math.h"
#include "generated_assets.h"
#include "generated_billboard_assets.h"

#define VERSION_TEXT "MEGALDOOM REWRITE GATE 29"
#define VIEW_TILE_W 16
#define VIEW_TILE_H 12
#define VIEW_TILE_COUNT (VIEW_TILE_W * VIEW_TILE_H)
#define VIEW_TILE_BASE TILE_USER_INDEX
#define PAIR_TILE_COUNT 256
#define VIEW_TILEMAP_X 12
#define VIEW_TILEMAP_Y 8
#define COMPASS_X 4
#define COMPASS_Y 8
#define COMPASS_W 5
#define COMPASS_H 5

static const char HEX_DIGITS[] = "0123456789ABCDEF";
static u32 g_pair_tiles[PAIR_TILE_COUNT][8];
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

static const u8 (*get_billboard_texture(u8 visual_id))[8] {
    switch (visual_id) {
        case BILLBOARD_VISUAL_DECOR:
            return FREEDOOM_BILLBOARD_DECOR_TEXTURE;
        case BILLBOARD_VISUAL_KEY:
            return FREEDOOM_BILLBOARD_KEY_TEXTURE;
        case BILLBOARD_VISUAL_BONUS:
        default:
            return FREEDOOM_BILLBOARD_BONUS_TEXTURE;
    }
}

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

    VDP_loadTileData((const u32 *)g_pair_tiles, VIEW_TILE_BASE, PAIR_TILE_COUNT, DMA);
}

static void init_wall_sampling_table(void) {
    for (u16 height = 1; height <= VIEW_TILE_H; height++) {
        for (u16 rel_y = 0; rel_y < VIEW_TILE_H; rel_y++) {
            g_wall_tex_y_by_height[height][rel_y] = (u8)((rel_y * 8) / height);
        }
    }
}

static void set_view_pair_tile(u16 x, u16 y, u8 left_color, u8 right_color) {
    const u16 pair_index = (u16)(((left_color & 0x0F) << 4) | (right_color & 0x0F));

    g_view_tilemap[(y * VIEW_TILE_W) + x] =
        TILE_ATTR_FULL(PAL0, FALSE, FALSE, FALSE, VIEW_TILE_BASE + pair_index);
}

static void set_view_column_color(u16 column, u16 y, u8 color) {
    const u16 tile_x = (u16)(column / 2);
    const u16 map_index = (u16)(y * VIEW_TILE_W + tile_x);
    const u16 tile_attr = g_view_tilemap[map_index];
    const u16 tile_index = tile_attr & TILE_INDEX_MASK;
    const u16 pair_index = (u16)(tile_index - VIEW_TILE_BASE);
    u8 left_color = (u8)((pair_index >> 4) & 0x0F);
    u8 right_color = (u8)(pair_index & 0x0F);

    if ((column & 1) == 0) {
        left_color = color;
    } else {
        right_color = color;
    }

    g_view_tilemap[map_index] =
        (tile_attr & TILE_ATTR_MASK) |
        (VIEW_TILE_BASE + (((u16)(left_color & 0x0F) << 4) | (right_color & 0x0F)));
}

static u8 sample_column_color(const RayColumn *column, u16 y) {
    const u8 bg = (y < (VIEW_TILE_H / 2)) ? 9 : 14;
    const u16 wall_h = column->height;
    const u16 top = (u16)((VIEW_TILE_H - wall_h) / 2);
    const u16 bottom = (u16)(top + wall_h);

    if ((y < top) || (y >= bottom)) {
        return bg;
    }

    const u8 (*texture)[8] = (column->color == 12) ? DOOR_TEXTURE : FREEDOOM_WALL_TEXTURE;
    const u8 *tex_y_table = g_wall_tex_y_by_height[wall_h];
    const u16 rel_y = (u16)(y - top);

    return texture[tex_y_table[rel_y]][column->tex_x & 7];
}

static void build_raycast_tilemap(const RayColumn *columns) {
    for (u16 y = 0; y < VIEW_TILE_H; y++) {
        for (u16 tile_x = 0; tile_x < VIEW_TILE_W; tile_x++) {
            const u16 left_col = (u16)(tile_x * 2);
            const u16 right_col = (u16)(left_col + 1);
            const u8 left_color = sample_column_color(&columns[left_col], y);
            const u8 right_color = sample_column_color(&columns[right_col], y);

            set_view_pair_tile(tile_x, y, left_color, right_color);
        }
    }
}

static void draw_billboard_spans(const RayColumn *columns, const BillboardSpan *spans, u16 span_count) {
    for (u16 i = 0; i < span_count; i++) {
        const BillboardSpan *span = &spans[i];

        if ((span->column < 0) || (span->column >= RAY_VIEW_COLS)) {
            continue;
        }
        if (span->depth >= columns[span->column].depth) {
            continue;
        }

        const s16 height = (s16)(span->bottom - span->top + 1);
        const u8 (*texture)[8] = get_billboard_texture(span->visual_id);

        for (s16 y = span->top; y <= span->bottom; y++) {
            if ((y >= 0) && (y < VIEW_TILE_H)) {
                const u8 tex_y = (u8)(((y - span->top) * 8) / height);
                const u8 texel = texture[tex_y][span->tex_x & 7];

                if (texel == 0) {
                    continue;
                }

                set_view_column_color((u16)span->column, (u16)y, texel);
            }
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
        TILE_ATTR_FULL(PAL0, FALSE, FALSE, FALSE, VIEW_TILE_BASE + ((color & 0x0F) << 4) + (color & 0x0F));
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
    init_pair_tiles();
    init_wall_sampling_table();
}

void renderer_draw_static_screen(void) {
    VDP_drawText(VERSION_TEXT, 7, 2);
    VDP_drawText("DECOR ASSET", 12, 4);
    VDP_drawText("DPAD MOVE/TURN  A/C STRAFE", 6, 22);
    VDP_drawText("PICKUPS 00", 13, 24);
    VDP_drawText("FRAME", 13, 25);
}

void renderer_render_scene(const RayColumn *columns, const PlayerState *player) {
    BillboardSpan spans[BILLBOARD_MAX_SPANS];
    const u16 span_count = billboard_project_scene(player, spans, BILLBOARD_MAX_SPANS);

    build_raycast_tilemap(columns);
    draw_billboard_spans(columns, spans, span_count);
    upload_view_tilemap();
    build_compass_tilemap(player->angle);
    upload_compass_tilemap();
}

void renderer_draw_frame_counter(u32 frame) {
    char frame_text[9];

    write_hex_u32(frame, frame_text);
    VDP_drawText(frame_text, 19, 25);
}

void renderer_draw_pickup_counter(u16 count) {
    char text[3];

    if (count > 99) {
        count = 99;
    }

    text[0] = (char)('0' + (count / 10));
    text[1] = (char)('0' + (count % 10));
    text[2] = '\0';
    VDP_drawText(text, 21, 24);
}
