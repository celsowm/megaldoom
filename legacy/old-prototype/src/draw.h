#ifndef MEGALDOOM_DRAW_H
#define MEGALDOOM_DRAW_H

#include <genesis.h>

void draw_rect(s16 x, s16 y, s16 w, s16 h, u8 col);
void draw_vline(s16 x, s16 y0, s16 y1, u8 col);
void draw_hline(s16 x0, s16 x1, s16 y, u8 col);
void draw_line(s16 x0, s16 y0, s16 x1, s16 y1, u8 col);
void draw_rect_outline(s16 x, s16 y, s16 w, s16 h, u8 col);

#endif
