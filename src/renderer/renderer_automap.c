#include "renderer_pack_internal.h"
#include "automap.h"

#define AM_COLOR_GRID 1
#define AM_COLOR_SOLID 4
#define AM_COLOR_FLOOR 5
#define AM_COLOR_CEILING 6
#define AM_COLOR_SPECIAL 7
#define AM_COLOR_PLAYER 15

#define AM_OUT_LEFT   0x01
#define AM_OUT_RIGHT  0x02
#define AM_OUT_TOP    0x04
#define AM_OUT_BOTTOM 0x08

static const u32 s_automap_palette[16] = {
    0x000000, 0x242424, 0x500000, 0x800000,
    0xE02020, 0xA06430, 0xE8D020, 0x3078E8,
    0x183818, 0x205820, 0x289028, 0x40C040,
    0x808080, 0xA0A0A0, 0xD0D0D0, 0xFFFFFF
};

static u8 automap_outcode(s32 x, s32 y) {
    u8 code = 0;
    if (x < 0) code |= AM_OUT_LEFT;
    else if (x >= RAY_VIEW_COLS) code |= AM_OUT_RIGHT;
    if (y < 0) code |= AM_OUT_TOP;
    else if (y >= RAY_VIEW_ROWS) code |= AM_OUT_BOTTOM;
    return code;
}

static bool automap_clip_line(s32 *x0, s32 *y0, s32 *x1, s32 *y1) {
    u8 code0 = automap_outcode(*x0, *y0);
    u8 code1 = automap_outcode(*x1, *y1);
    while (TRUE) {
        if ((code0 | code1) == 0) return TRUE;
        if (code0 & code1) return FALSE;
        const u8 code = code0 ? code0 : code1;
        s32 x = 0;
        s32 y = 0;
        if (code & AM_OUT_TOP) {
            if (*y1 == *y0) return FALSE;
            y = 0;
            x = *x0 + ((*x1 - *x0) * (y - *y0)) / (*y1 - *y0);
        } else if (code & AM_OUT_BOTTOM) {
            if (*y1 == *y0) return FALSE;
            y = RAY_VIEW_ROWS - 1;
            x = *x0 + ((*x1 - *x0) * (y - *y0)) / (*y1 - *y0);
        } else if (code & AM_OUT_RIGHT) {
            if (*x1 == *x0) return FALSE;
            x = RAY_VIEW_COLS - 1;
            y = *y0 + ((*y1 - *y0) * (x - *x0)) / (*x1 - *x0);
        } else {
            if (*x1 == *x0) return FALSE;
            x = 0;
            y = *y0 + ((*y1 - *y0) * (x - *x0)) / (*x1 - *x0);
        }
        if (code == code0) {
            *x0 = x; *y0 = y; code0 = automap_outcode(x, y);
        } else {
            *x1 = x; *y1 = y; code1 = automap_outcode(x, y);
        }
    }
}

static void automap_put_pixel(s16 x, s16 y, u8 color) {
    if ((u16)x >= RAY_VIEW_COLS || (u16)y >= RAY_VIEW_ROWS) return;
    const u16 tile = view_tile_index((u16)x >> 3, (u16)y >> 3);
    const u16 shift = (u16)((7 - ((u16)x & 7)) << 2);
    u32 *row = &g_view_tiles[tile][(u16)y & 7];
    *row = (*row & ~((u32)0x0Fu << shift)) |
           ((u32)(color & 0x0F) << shift);
}

static void automap_draw_line(s32 x0, s32 y0, s32 x1, s32 y1, u8 color) {
    if (!automap_clip_line(&x0, &y0, &x1, &y1)) return;
    s16 x = (s16)x0;
    s16 y = (s16)y0;
    const s16 end_x = (s16)x1;
    const s16 end_y = (s16)y1;
    const s16 dx = (s16)((end_x > x) ? (end_x - x) : (x - end_x));
    const s16 sx = (x < end_x) ? 1 : -1;
    const s16 dy = (s16)-((end_y > y) ? (end_y - y) : (y - end_y));
    const s16 sy = (y < end_y) ? 1 : -1;
    s16 error = (s16)(dx + dy);
    for (;;) {
        automap_put_pixel(x, y, color);
        if (x == end_x && y == end_y) break;
        const s16 twice = (s16)(error << 1);
        if (twice >= dy) { error = (s16)(error + dy); x = (s16)(x + sx); }
        if (twice <= dx) { error = (s16)(error + dx); y = (s16)(y + sy); }
    }
}

static s32 automap_screen_x(s32 world_x, const AutomapState *state) {
    return RAY_VIEW_CENTER_X + ((world_x - state->center_x) >> state->scale_shift);
}

static s32 automap_screen_y(s32 world_y, const AutomapState *state) {
    return (RAY_VIEW_ROWS / 2) + ((world_y - state->center_y) >> state->scale_shift);
}

static u8 automap_line_color(u8 kind) {
    if (kind == BSP_AUTOMAP_LINE_FLOOR) return AM_COLOR_FLOOR;
    if (kind == BSP_AUTOMAP_LINE_CEILING) return AM_COLOR_CEILING;
    if (kind == BSP_AUTOMAP_LINE_SPECIAL) return AM_COLOR_SPECIAL;
    return AM_COLOR_SOLID;
}

static s32 automap_grid_start(s32 low) {
    s32 value = low & ~127L;
    if (value < low) value += 128;
    return value;
}

static void automap_draw_grid(const AutomapState *state) {
    const s32 half_w = (s32)(RAY_VIEW_COLS / 2) << state->scale_shift;
    const s32 half_h = (s32)(RAY_VIEW_ROWS / 2) << state->scale_shift;
    const s32 min_x = state->center_x - half_w;
    const s32 max_x = state->center_x + half_w;
    const s32 min_y = state->center_y - half_h;
    const s32 max_y = state->center_y + half_h;
    for (s32 x = automap_grid_start(min_x); x <= max_x; x += 128) {
        const s32 sx = automap_screen_x(x, state);
        automap_draw_line(sx, 0, sx, RAY_VIEW_ROWS - 1, AM_COLOR_GRID);
    }
    for (s32 y = automap_grid_start(min_y); y <= max_y; y += 128) {
        const s32 sy = automap_screen_y(y, state);
        automap_draw_line(0, sy, RAY_VIEW_COLS - 1, sy, AM_COLOR_GRID);
    }
}

static void automap_draw_player(const PlayerState *player,
                                const AutomapState *state) {
    const s32 px = automap_screen_x(player->x, state);
    const s32 py = automap_screen_y(player->y, state);
    const s16 fx = (s16)((fx_cos(player->angle) * 8) >> FX_SHIFT);
    const s16 fy = (s16)((fx_sin(player->angle) * 8) >> FX_SHIFT);
    const s16 sx = (s16)-fy;
    const s16 sy = fx;
    const s32 tail_x = px - (fx >> 1);
    const s32 tail_y = py - (fy >> 1);
    automap_draw_line(tail_x, tail_y, px + fx, py + fy, AM_COLOR_PLAYER);
    automap_draw_line(tail_x, tail_y,
                      tail_x + (sx >> 1) - (fx >> 2),
                      tail_y + (sy >> 1) - (fy >> 2), AM_COLOR_PLAYER);
    automap_draw_line(tail_x, tail_y,
                      tail_x - (sx >> 1) - (fx >> 2),
                      tail_y - (sy >> 1) - (fy >> 2), AM_COLOR_PLAYER);
}

void renderer_set_automap_active(bool active) {
    renderer_apply_weapon_bob(0, 0);
    if (active) {
        for (u16 i = 0; i < 16; i++) {
            PAL_setColor((u16)(48 + i), RGB24_TO_VDPCOLOR(s_automap_palette[i]));
        }
    } else {
        renderer_load_world_palette();
    }
    renderer_automap_weapon_visibility(active);
}

void renderer_render_automap(const PlayerState *player,
                             const AutomapState *automap) {
    for (u16 tile = 0; tile < VIEW_TILE_COUNT; tile++) {
        for (u16 row = 0; row < 8; row++) g_view_tiles[tile][row] = 0;
    }
    if (automap->grid) automap_draw_grid(automap);
    for (u16 i = 0; i < bsp_automap_line_count; i++) {
        if (!bsp_automap_line_visible(i)) continue;
        const BspAutomapLine *line = &bsp_automap_lines[i];
        const BspVertex *a = &bsp_vertices[line->v1];
        const BspVertex *b = &bsp_vertices[line->v2];
        automap_draw_line(automap_screen_x(a->x, automap),
                          automap_screen_y(a->y, automap),
                          automap_screen_x(b->x, automap),
                          automap_screen_y(b->y, automap),
                          automap_line_color(line->kind));
    }
    automap_draw_player(player, automap);
    renderer_prepare_full_base_upload();
}
