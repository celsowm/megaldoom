#include "renderer_internal.h"
#include "generated_assets.h"
#include "generated_billboard_assets.h"
#include "generated_hud_assets.h"

static const u8 (*get_billboard_texture(u8 visual_id))[16] {
    switch (visual_id) {
        case BILLBOARD_VISUAL_DUMMY_DAMAGED:
        case BILLBOARD_VISUAL_DUMMY:
            return FREEDOOM_BILLBOARD_ENEMY_TEXTURE;
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

static u8 shade_texel(u8 texel) {
    static const u8 SHADED_COLOR_MAP[16] = {0, 6, 2, 2, 3, 4, 5, 6, 8, 8, 9, 10, 12, 12, 14, 14};

    return SHADED_COLOR_MAP[texel & 15];
}

static u8 sample_wall_texture(const RayColumn *column, u8 tex_y) {
    u8 texel;

    if (column->texture_id == 1) {
        texel = FREEDOOM_DOOR_TEXTURE[tex_y][column->tex_x & 15];
    } else if (column->texture_id == 2) {
        texel = FREEDOOM_LOCKED_DOOR_TEXTURE[tex_y][column->tex_x & 15];
    } else if (column->texture_id == 3) {
        texel = FREEDOOM_SWITCH_TEXTURE[tex_y][column->tex_x & 15];
    } else if (column->texture_id == 4) {
        texel = FREEDOOM_WALL_BROWN_TEXTURE[tex_y][column->tex_x & 15];
    } else if (column->texture_id == 5) {
        texel = FREEDOOM_WALL_GRAY_TEXTURE[tex_y][column->tex_x & 15];
    } else if (column->texture_id == 6) {
        texel = FREEDOOM_WALL_METAL_TEXTURE[tex_y][column->tex_x & 15];
    } else if (column->texture_id == 7) {
        texel = FREEDOOM_WALL_BRICK_TEXTURE[tex_y][column->tex_x & 15];
    } else if (column->texture_id == 8) {
        texel = FREEDOOM_WALL_TECH_TEXTURE[tex_y][column->tex_x & 15];
    } else {
        texel = FREEDOOM_WALL_TEXTURE[tex_y][column->tex_x & 15];
    }

    if (column->shade && (texel != 0)) {
        texel = shade_texel(texel);
    }

    return texel;
}

static u8 sample_column_color(const RayColumn *column, u16 y) {
    const u8 bg = (y < (VIEW_PIXEL_H / 2)) ? 9 : 14;
    const u16 wall_h = column->height;
    const u16 top = (u16)((VIEW_PIXEL_H - wall_h) / 2);
    const u16 bottom = (u16)(top + wall_h);

    if ((y < top) || (y >= bottom)) {
        return bg;
    }

    const u8 *tex_y_table = g_wall_tex_y_by_height[wall_h];
    const u16 rel_y = (u16)(y - top);
    const u8 tex_y = tex_y_table[rel_y];
    return sample_wall_texture(column, tex_y);
}

static void build_raycast_tilemap(const RayColumn *columns) {
    for (u16 tile_y = 0; tile_y < VIEW_TILE_H; tile_y++) {
        for (u16 tile_x = 0; tile_x < VIEW_TILE_W; tile_x++) {
            const u16 tile_index = (u16)((tile_y * VIEW_TILE_W) + tile_x);
            const u16 base_col = (u16)(tile_x * 8);

            for (u16 row = 0; row < 8; row++) {
                const u16 pixel_y = (u16)((tile_y * 8) + row);
                u32 packed = 0;

                // Sample one color per RAY_COL_STRIDE pixels and replicate it, matching
                // the columns produced by raycast_cast_frame.
                for (u16 px = 0; px < 8; px += RAY_COL_STRIDE) {
                    const u8 color = (u8)(sample_column_color(&columns[base_col + px], pixel_y) & 0x0F);

                    for (u16 k = 0; k < RAY_COL_STRIDE; k++) {
                        packed = (packed << 4) | color;
                    }
                }

                g_view_tiles[tile_index][row] = packed;
            }
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
        const u8 (*texture)[16] = get_billboard_texture(span->visual_id);

        for (s16 y = span->top; y <= span->bottom; y++) {
            if ((y >= 0) && (y < VIEW_PIXEL_H)) {
                const u8 tex_y = (u8)(((y - span->top) * 16) / height);
                u8 texel = texture[tex_y][span->tex_x & 15];

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
    const u8 (*weapon)[FREEDOOM_WEAPON_W] = flash ? FREEDOOM_WEAPON_FIRE : FREEDOOM_WEAPON_IDLE;
    const u16 bottom_y = FREEDOOM_WEAPON_DRAW_Y + FREEDOOM_WEAPON_DRAW_H;
    const u16 right_x = FREEDOOM_WEAPON_DRAW_X + FREEDOOM_WEAPON_DRAW_W;

    for (u16 y = FREEDOOM_WEAPON_DRAW_Y; y < bottom_y; y++) {
        for (u16 x = FREEDOOM_WEAPON_DRAW_X; x < right_x; x++) {
            const u8 texel = weapon[y][x];

            if (texel != 0) {
                set_view_column_color(x, y, texel);
            }
        }
    }
}

#define DAMAGE_BORDER_PX 8

static void draw_damage_overlay(void) {
    for (u16 x = 0; x < RAY_VIEW_COLS; x++) {
        for (u16 t = 0; t < 3; t++) {
            set_view_column_color(x, t, 2);
            set_view_column_color(x, (VIEW_PIXEL_H - 1 - t), 2);
        }
    }

    for (u16 y = 0; y < VIEW_PIXEL_H; y++) {
        for (u16 t = 0; t < DAMAGE_BORDER_PX; t++) {
            set_view_column_color(t, y, 2);
            set_view_column_color((RAY_VIEW_COLS - 1 - t), y, 2);
        }
    }
}

static void draw_low_health_overlay(void) {
    for (u16 x = 16; x <= 32; x++) {
        set_view_column_color(x, 1, 11);
        set_view_column_color(x, 2, 11);
        set_view_column_color(x, (VIEW_PIXEL_H - 3), 11);
        set_view_column_color(x, (VIEW_PIXEL_H - 2), 11);
    }

    for (u16 x = (RAY_VIEW_COLS - 33); x <= (RAY_VIEW_COLS - 17); x++) {
        set_view_column_color(x, 1, 11);
        set_view_column_color(x, 2, 11);
        set_view_column_color(x, (VIEW_PIXEL_H - 3), 11);
        set_view_column_color(x, (VIEW_PIXEL_H - 2), 11);
    }
}

static void upload_view_tilemap(void) {
    VDP_loadTileData((const u32 *)g_view_tiles, VIEW_TILE_BASE, VIEW_TILE_COUNT, DMA);
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
        g_compass_tilemap[i] = TILE_ATTR_FULL(PAL0, FALSE, FALSE, FALSE, PAIR_TILE_BASE);
    }
}

static void set_compass_tile(s16 x, s16 y, u8 color) {
    if ((x < 0) || (y < 0) || (x >= COMPASS_W) || (y >= COMPASS_H)) {
        return;
    }

    g_compass_tilemap[(y * COMPASS_W) + x] =
        TILE_ATTR_FULL(PAL0, FALSE, FALSE, FALSE, PAIR_TILE_BASE + ((color & 0x0F) << 4) + (color & 0x0F));
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
