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

// Wall texture lookup indexed by texture_id, replacing the per-pixel 8-way branch.
// Order matches the original sample_wall_texture chain (id 0 == default wall).
static const u8 (*const WALL_TEX[9])[16] = {
    FREEDOOM_WALL_TEXTURE,        // 0
    FREEDOOM_DOOR_TEXTURE,        // 1
    FREEDOOM_LOCKED_DOOR_TEXTURE, // 2
    FREEDOOM_SWITCH_TEXTURE,      // 3
    FREEDOOM_WALL_BROWN_TEXTURE,  // 4
    FREEDOOM_WALL_GRAY_TEXTURE,   // 5
    FREEDOOM_WALL_METAL_TEXTURE,  // 6
    FREEDOOM_WALL_BRICK_TEXTURE,  // 7
    FREEDOOM_WALL_TECH_TEXTURE,   // 8
};

// REP4[c] == c * 0x1111: a 4-bit color replicated across 4 nibbles (4 pixels).
static const u32 REP4[16] = {
    0x0000, 0x1111, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666, 0x7777,
    0x8888, 0x9999, 0xAAAA, 0xBBBB, 0xCCCC, 0xDDDD, 0xEEEE, 0xFFFF,
};

// Build a full vertical color strip for one ray column, resolving the texture
// pointer and shade flag once (instead of per pixel). The wall is centered, so
// everything above it is sky (y < VIEW_PIXEL_H/2) and below it is floor.
static void build_column_strip(const RayColumn *column, u8 *strip) {
    const u16 wall_h = column->height;
    const u16 top = (u16)((VIEW_PIXEL_H - wall_h) / 2);
    const u16 bottom = (u16)(top + wall_h);
    const u8 (*tex)[16] = WALL_TEX[(column->texture_id <= 8) ? column->texture_id : 0];
    const u8 *ty_table = g_wall_tex_y_by_height[wall_h];
    const u8 tex_x = (u8)(column->tex_x & 15);
    u16 y = 0;

    for (; y < top; y++) {
        strip[y] = 9; // sky
    }

    if (column->shade) {
        for (; y < bottom; y++) {
            const u8 texel = tex[ty_table[y - top]][tex_x];
            strip[y] = (texel != 0) ? (u8)(shade_texel(texel) & 0x0F) : 0;
        }
    } else {
        for (; y < bottom; y++) {
            strip[y] = (u8)(tex[ty_table[y - top]][tex_x] & 0x0F);
        }
    }

    for (; y < VIEW_PIXEL_H; y++) {
        strip[y] = 14; // floor
    }
}

#if RAY_COL_STRIDE != 4
#error "build_raycast_tilemap packs 2 strided columns per 8px tile; assumes RAY_COL_STRIDE == 4"
#endif

static u8 remap_billboard_texel(u8 visual_id, u8 texel) {
    if (visual_id == BILLBOARD_VISUAL_DECOR_DAMAGED) {
        return remap_damaged_billboard_texel(texel);
    }
    if (visual_id == BILLBOARD_VISUAL_DUMMY) {
        return remap_dummy_billboard_texel(texel, FALSE);
    }
    if (visual_id == BILLBOARD_VISUAL_DUMMY_DAMAGED) {
        return remap_dummy_billboard_texel(texel, TRUE);
    }
    return texel;
}

static void build_raycast_tilemap(const RayColumn *columns) {
    u8 strip_a[VIEW_PIXEL_H];
    u8 strip_b[VIEW_PIXEL_H];

    // Each 8px-wide tile column maps to two cast columns (px 0 and 4), each
    // replicated 4x. Build both strips once per tile column, then pack the 15
    // vertical tiles from them.
    for (u16 tile_x = 0; tile_x < VIEW_TILE_W; tile_x++) {
        const u16 base_col = (u16)(tile_x * 8);
        build_column_strip(&columns[base_col], strip_a);
        build_column_strip(&columns[base_col + 4], strip_b);

        for (u16 tile_y = 0; tile_y < VIEW_TILE_H; tile_y++) {
            const u16 tile_index = (u16)((tile_y * VIEW_TILE_W) + tile_x);
            u16 pixel_y = (u16)(tile_y * 8);

            for (u16 row = 0; row < 8; row++, pixel_y++) {
                g_view_tiles[tile_index][row] =
                    (REP4[strip_a[pixel_y]] << 16) | REP4[strip_b[pixel_y]];
            }
        }
    }
}

// Draw the one-column billboard spans on top of the packed wall tiles. This is
// O(visible billboard pixels): cheap for scattered sprites (turning) yet bounded
// for a fullscreen enemy. Per span the tile_x and nibble shift are constant, and
// we walk tile rows so the tile-row base address is computed once per 8 rows
// (one 68000 multiply per tile, not per pixel — that per-pixel multiply was what
// made the old set_view_column_color path spike).
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
        const u8 tex_x = (u8)(span->tex_x & 15);
        // Reuse the precomputed (rel_y * 16 / h) table to avoid a per-pixel divide.
        // It only covers h in [1, VIEW_PIXEL_H]; taller spans fall back to a divide.
        const bool use_table = (height >= 1) && (height <= VIEW_PIXEL_H);
        const u8 *ty_table = use_table ? g_wall_tex_y_by_height[height] : NULL;

        const u16 col = (u16)span->column;
        const u16 tile_x = (u16)(col >> 3);
        const u16 shift = (u16)((7 - (col & 7)) * 4);
        const u32 keep_mask = ~((u32)0x0F << shift);

        s16 y0 = span->top;
        s16 y1 = span->bottom;
        if (y0 < 0) {
            y0 = 0;
        }
        if (y1 >= VIEW_PIXEL_H) {
            y1 = VIEW_PIXEL_H - 1;
        }

        u16 y = (u16)y0;
        const u16 y_end = (u16)(y1 + 1);
        while (y < y_end) {
            const u16 tile_y = (u16)(y >> 3);
            u32 *tile = g_view_tiles[(tile_y * VIEW_TILE_W) + tile_x]; // multiply once per tile
            const u16 next_tile_y = (u16)((tile_y + 1) * 8);
            const u16 stop = (next_tile_y < y_end) ? next_tile_y : y_end;

            for (; y < stop; y++) {
                const s16 rel_y = (s16)((s16)y - span->top);
                const u8 tex_y = use_table ? ty_table[rel_y] : (u8)((rel_y * 16) / height);
                const u8 texel = remap_billboard_texel(span->visual_id, texture[tex_y][tex_x]);

                if (texel != 0) {
                    const u16 row_y = (u16)(y & 7);
                    tile[row_y] = (tile[row_y] & keep_mask) | ((u32)(texel & 0x0F) << shift);
                }
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
    // The tilemap itself is static and was uploaded once at init; only the tile
    // pixel data changes per frame, so just DMA the tiles.
    VDP_loadTileData((const u32 *)g_view_tiles, VIEW_TILE_BASE, VIEW_TILE_COUNT, DMA);
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
