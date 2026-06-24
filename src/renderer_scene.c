#include "renderer_internal.h"
#include "generated_assets.h"
#include "generated_billboard_assets.h"

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

static const u8 LOCKED_DOOR_TEXTURE[8][8] = {
    {13, 12, 13, 12, 13, 12, 13, 12},
    {13, 2, 13, 12, 13, 2, 13, 12},
    {13, 12, 13, 11, 13, 12, 13, 11},
    {12, 13, 12, 2, 12, 13, 12, 2},
    {13, 12, 13, 11, 13, 12, 13, 11},
    {13, 2, 13, 12, 13, 2, 13, 12},
    {13, 12, 13, 12, 13, 12, 13, 12},
    {12, 11, 12, 13, 12, 11, 12, 13},
};

static const u8 EXIT_SWITCH_TEXTURE[8][8] = {
    {11, 11, 12, 12, 11, 11, 12, 12},
    {11, 2, 12, 2, 11, 2, 12, 2},
    {12, 12, 11, 11, 12, 12, 11, 11},
    {12, 2, 11, 2, 12, 2, 11, 2},
    {11, 11, 12, 12, 11, 11, 12, 12},
    {11, 2, 12, 2, 11, 2, 12, 2},
    {12, 12, 11, 11, 12, 12, 11, 11},
    {12, 2, 11, 2, 12, 2, 11, 2},
};

static const u8 (*get_billboard_texture(u8 visual_id))[8] {
    switch (visual_id) {
        case BILLBOARD_VISUAL_DUMMY_DAMAGED:
        case BILLBOARD_VISUAL_DUMMY:
        case BILLBOARD_VISUAL_DECOR_DAMAGED:
        case BILLBOARD_VISUAL_DECOR:
            return FREEDOOM_BILLBOARD_DECOR_TEXTURE;
        case BILLBOARD_VISUAL_KEY:
            return FREEDOOM_BILLBOARD_KEY_TEXTURE;
        case BILLBOARD_VISUAL_BONUS:
        default:
            return FREEDOOM_BILLBOARD_BONUS_TEXTURE;
    }
}

static u8 remap_damaged_billboard_texel(u8 texel) {
    if (texel == 0) {
        return 0;
    }

    return (texel & 1) ? 12 : 13;
}

static u8 remap_dummy_billboard_texel(u8 texel, bool damaged) {
    if (texel == 0) {
        return 0;
    }

    if (damaged) {
        return (texel & 1) ? 12 : 2;
    }

    return (texel & 1) ? 10 : 11;
}

static u8 sample_column_color(const RayColumn *column, u16 y) {
    const u8 bg = (y < (VIEW_TILE_H / 2)) ? 9 : 14;
    const u16 wall_h = column->height;
    const u16 top = (u16)((VIEW_TILE_H - wall_h) / 2);
    const u16 bottom = (u16)(top + wall_h);

    if ((y < top) || (y >= bottom)) {
        return bg;
    }

    const u8 (*texture)[8];
    const u8 *tex_y_table = g_wall_tex_y_by_height[wall_h];
    const u16 rel_y = (u16)(y - top);

    if (column->color == 12) {
        texture = DOOR_TEXTURE;
    } else if (column->color == 13) {
        texture = LOCKED_DOOR_TEXTURE;
    } else if (column->color == 11) {
        texture = EXIT_SWITCH_TEXTURE;
    } else {
        texture = FREEDOOM_WALL_TEXTURE;
    }

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
                u8 texel = texture[tex_y][span->tex_x & 7];

                if (span->visual_id == BILLBOARD_VISUAL_DECOR_DAMAGED) {
                    texel = remap_damaged_billboard_texel(texel);
                } else if (span->visual_id == BILLBOARD_VISUAL_DUMMY) {
                    texel = remap_dummy_billboard_texel(texel, FALSE);
                } else if (span->visual_id == BILLBOARD_VISUAL_DUMMY_DAMAGED) {
                    texel = remap_dummy_billboard_texel(texel, TRUE);
                }

                if (texel == 0) {
                    continue;
                }

                set_view_column_color((u16)span->column, (u16)y, texel);
            }
        }
    }
}

static void draw_weapon_overlay(bool flash) {
    for (u16 y = 8; y < VIEW_TILE_H; y++) {
        for (u16 x = 13; x <= 18; x++) {
            set_view_column_color(x, y, 2);
        }
    }

    for (u16 y = 9; y < VIEW_TILE_H; y++) {
        for (u16 x = 14; x <= 17; x++) {
            set_view_column_color(x, y, 4);
        }
    }

    for (u16 y = 8; y <= 10; y++) {
        set_view_column_color(15, y, flash ? 11 : 7);
        set_view_column_color(16, y, flash ? 11 : 7);
    }

    if (flash) {
        set_view_column_color(14, 7, 11);
        set_view_column_color(15, 7, 11);
        set_view_column_color(16, 7, 11);
        set_view_column_color(17, 7, 11);
    }
}

static void draw_damage_overlay(void) {
    for (u16 x = 0; x < RAY_VIEW_COLS; x++) {
        set_view_column_color(x, 0, 2);
        set_view_column_color(x, (VIEW_TILE_H - 1), 2);
    }

    for (u16 y = 1; y < (VIEW_TILE_H - 1); y++) {
        set_view_column_color(0, y, 2);
        set_view_column_color(1, y, 2);
        set_view_column_color((RAY_VIEW_COLS - 2), y, 2);
        set_view_column_color((RAY_VIEW_COLS - 1), y, 2);
    }
}

static void draw_low_health_overlay(void) {
    for (u16 x = 4; x <= 8; x++) {
        set_view_column_color(x, 1, 11);
        set_view_column_color(x, 2, 11);
        set_view_column_color(x, (VIEW_TILE_H - 3), 11);
        set_view_column_color(x, (VIEW_TILE_H - 2), 11);
    }

    for (u16 x = (RAY_VIEW_COLS - 9); x <= (RAY_VIEW_COLS - 5); x++) {
        set_view_column_color(x, 1, 11);
        set_view_column_color(x, 2, 11);
        set_view_column_color(x, (VIEW_TILE_H - 3), 11);
        set_view_column_color(x, (VIEW_TILE_H - 2), 11);
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

void renderer_render_scene(const RayColumn *columns,
                           const PlayerState *player,
                           bool weapon_flash,
                           bool damage_flash,
                           bool low_health_warning) {
    BillboardSpan spans[BILLBOARD_MAX_SPANS];
    const u16 span_count = billboard_project_scene(player, spans, BILLBOARD_MAX_SPANS);

    build_raycast_tilemap(columns);
    draw_billboard_spans(columns, spans, span_count);
    draw_weapon_overlay(weapon_flash);
    if (damage_flash) {
        draw_damage_overlay();
    } else if (low_health_warning) {
        draw_low_health_overlay();
    }
    upload_view_tilemap();
    build_compass_tilemap(player->angle);
    upload_compass_tilemap();
}
