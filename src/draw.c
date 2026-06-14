#include <genesis.h>
#include <bmp.h>
#include "game.h"
#include "draw.h"

static inline s16 clamp_s16(s16 v, s16 lo, s16 hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline s16 abs_s16_local(s16 v) {
    return (v < 0) ? -v : v;
}

void draw_rect(s16 x, s16 y, s16 w, s16 h, u8 col) {
    if (w <= 0 || h <= 0) return;

    s16 x0 = clamp_s16(x, 0, CANVAS_W - 1);
    s16 y0 = clamp_s16(y, 0, CANVAS_H - 1);
    s16 x1 = clamp_s16(x + w - 1, 0, CANVAS_W - 1);
    s16 y1 = clamp_s16(y + h - 1, 0, CANVAS_H - 1);

    if (x1 < x0 || y1 < y0) return;

    for (s16 py = y0; py <= y1; py++) {
        for (s16 px = x0; px <= x1; px++) {
            BMP_setPixelFast((u16)px, (u16)py, col);
        }
    }
}

void draw_vline(s16 x, s16 y0, s16 y1, u8 col) {
    if (x < 0 || x >= CANVAS_W) return;

    if (y1 < y0) {
        s16 tmp = y0;
        y0 = y1;
        y1 = tmp;
    }

    y0 = clamp_s16(y0, 0, CANVAS_H - 1);
    y1 = clamp_s16(y1, 0, CANVAS_H - 1);

    for (s16 y = y0; y <= y1; y++) {
        BMP_setPixelFast((u16)x, (u16)y, col);
    }
}

void draw_hline(s16 x0, s16 x1, s16 y, u8 col) {
    if (y < 0 || y >= CANVAS_H) return;

    if (x1 < x0) {
        s16 tmp = x0;
        x0 = x1;
        x1 = tmp;
    }

    x0 = clamp_s16(x0, 0, CANVAS_W - 1);
    x1 = clamp_s16(x1, 0, CANVAS_W - 1);

    for (s16 x = x0; x <= x1; x++) {
        BMP_setPixelFast((u16)x, (u16)y, col);
    }
}

void draw_line(s16 x0, s16 y0, s16 x1, s16 y1, u8 col) {
    s16 dx = abs_s16_local(x1 - x0);
    s16 sx = (x0 < x1) ? 1 : -1;
    s16 dy = -abs_s16_local(y1 - y0);
    s16 sy = (y0 < y1) ? 1 : -1;
    s16 err = dx + dy;

    while (TRUE) {
        if (x0 >= 0 && x0 < CANVAS_W && y0 >= 0 && y0 < CANVAS_H) {
            BMP_setPixelFast((u16)x0, (u16)y0, col);
        }

        if (x0 == x1 && y0 == y1) break;

        s16 e2 = err << 1;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void draw_rect_outline(s16 x, s16 y, s16 w, s16 h, u8 col) {
    if (w <= 0 || h <= 0) return;
    draw_hline(x, x + w - 1, y, col);
    draw_hline(x, x + w - 1, y + h - 1, col);
    draw_vline(x, y, y + h - 1, col);
    draw_vline(x + w - 1, y, y + h - 1, col);
}
