#include <genesis.h>
#include <bmp.h>

#include "game.h"
#include "map.h"
#include "draw.h"
#include "trig1024_q8.h"

static Player player = {
    TO_FP(3) + (FP_ONE / 2),
    TO_FP(3) + (FP_ONE / 2),
    0
};

static Actor imp = {
    TO_FP(9) + (FP_ONE / 2),
    TO_FP(7) + (FP_ONE / 2),
    TRUE
};

static s16 g_depth[VIEW_COLS];
static s16 g_ray_rel[VIEW_COLS];
static s16 g_ray_corr[VIEW_COLS];
static s32 g_inv_abs_cos[ANGLE_FULL];
static s32 g_inv_abs_sin[ANGLE_FULL];

static u16 g_prev_joy = 0;
static u16 g_frame = 0;
static u8 g_weapon_recoil = 0;
static u8 g_last_action_was_door = FALSE;

static inline s16 sin_q8(u16 a) {
    return SIN1024_Q8[a & ANGLE_MASK];
}

static inline s16 cos_q8(u16 a) {
    return SIN1024_Q8[(a + ANGLE_90) & ANGLE_MASK];
}

static inline s16 abs_s16(s16 v) {
    return (v < 0) ? -v : v;
}

static inline s32 abs_s32(s32 v) {
    return (v < 0) ? -v : v;
}

static inline s32 safe_div_s32(s32 n, s32 d) {
    if (d == 0) return 0x3fffffff;
    return n / d;
}

static void init_perf_tables(void) {
    for (u16 col = 0; col < VIEW_COLS; col++) {
        g_ray_rel[col] = ((s16)col * FOV_ANGLE) / VIEW_COLS - (FOV_ANGLE / 2);
        g_ray_corr[col] = cos_q8((u16)g_ray_rel[col]);
    }

    for (u16 a = 0; a < ANGLE_FULL; a++) {
        s16 ca = cos_q8(a);
        s16 sa = sin_q8(a);
        s32 abs_ca = abs_s16(ca);
        s32 abs_sa = abs_s16(sa);

        g_inv_abs_cos[a] = safe_div_s32(FP_ONE * FP_ONE, abs_ca);
        g_inv_abs_sin[a] = safe_div_s32(FP_ONE * FP_ONE, abs_sa);
    }
}

static u8 distance_shade(s32 corrected_dist) {
    if (corrected_dist < TO_FP(2)) return 2;
    if (corrected_dist < TO_FP(5)) return 1;
    return 0;
}

static u8 shade_color(u8 dark, u8 mid, u8 light, u8 shade, u8 side) {
    if (side && shade > 0) shade--;
    if (shade == 2) return light;
    if (shade == 1) return mid;
    return dark;
}

static u8 wall_texel_color(u8 tile, u8 side, u8 tex_x, u8 tex_y, s32 corrected_dist) {
    u8 shade = distance_shade(corrected_dist);

    if (tile == TILE_DOOR) {
        // Fake vertical metal door: red slabs + bright center stripe.
        if ((tex_x > 29 && tex_x < 34) || (tex_x & 15) == 0) {
            return C_DOOR_LIGHT;
        }
        return shade_color(C_DOOR_DARK, C_DOOR_DARK, C_DOOR_LIGHT, shade, side);
    }

    if (tile == TILE_TECH) {
        // Tech wall with cyan seams.
        if ((tex_y & 15) == 0 || (tex_x & 31) == 0) {
            return C_TECH_LIGHT;
        }
        return shade_color(C_TECH_DARK, C_TECH_DARK, C_TECH_LIGHT, shade, side);
    }

    // Brick-ish stone. Mortar lines darken the surface.
    if ((tex_y & 15) == 0 || ((tex_x + ((tex_y & 16) ? 16 : 0)) & 31) == 0) {
        return C_WALL_DARK;
    }

    return shade_color(C_WALL_DARK, C_WALL_MID, C_WALL_LIGHT, shade, side);
}

static void setup_palette(void) {
    PAL_setColor(0,  RGB24_TO_VDPCOLOR(0x000000));
    PAL_setColor(1,  RGB24_TO_VDPCOLOR(0x101020));
    PAL_setColor(2,  RGB24_TO_VDPCOLOR(0x242448));
    PAL_setColor(3,  RGB24_TO_VDPCOLOR(0x20180f));
    PAL_setColor(4,  RGB24_TO_VDPCOLOR(0x3a2c18));
    PAL_setColor(5,  RGB24_TO_VDPCOLOR(0x303040));
    PAL_setColor(6,  RGB24_TO_VDPCOLOR(0x606078));
    PAL_setColor(7,  RGB24_TO_VDPCOLOR(0xa0a0b8));
    PAL_setColor(8,  RGB24_TO_VDPCOLOR(0x401820));
    PAL_setColor(9,  RGB24_TO_VDPCOLOR(0xd85040));
    PAL_setColor(10, RGB24_TO_VDPCOLOR(0x103040));
    PAL_setColor(11, RGB24_TO_VDPCOLOR(0x00c8d8));
    PAL_setColor(12, RGB24_TO_VDPCOLOR(0x101018));
    PAL_setColor(13, RGB24_TO_VDPCOLOR(0x686878));
    PAL_setColor(14, RGB24_TO_VDPCOLOR(0x00f0f0));
    PAL_setColor(15, RGB24_TO_VDPCOLOR(0xf0d890));
}

static void try_move(s32 dx, s32 dy) {
    s32 nx = player.x + dx;
    s32 ny = player.y + dy;

    // Slide on walls: X and Y are tested separately.
    if (!is_blocked_fixed(nx, player.y)) player.x = nx;
    if (!is_blocked_fixed(player.x, ny)) player.y = ny;
}

static void update_input(void) {
    u16 joy = JOY_readJoypad(JOY_1);
    u16 pressed = joy & ~g_prev_joy;

    if (joy & BUTTON_LEFT) {
        player.angle = (player.angle - ROT_SPEED) & ANGLE_MASK;
    }

    if (joy & BUTTON_RIGHT) {
        player.angle = (player.angle + ROT_SPEED) & ANGLE_MASK;
    }

    s16 ca = cos_q8(player.angle);
    s16 sa = sin_q8(player.angle);

    if (joy & BUTTON_UP) {
        try_move(((s32)ca * PLAYER_SPEED) >> FP_SHIFT,
                 ((s32)sa * PLAYER_SPEED) >> FP_SHIFT);
    }

    if (joy & BUTTON_DOWN) {
        try_move(-(((s32)ca * PLAYER_SPEED) >> FP_SHIFT),
                 -(((s32)sa * PLAYER_SPEED) >> FP_SHIFT));
    }

    if (joy & BUTTON_A) {
        try_move(((s32)sa * STRAFE_SPEED) >> FP_SHIFT,
                 -(((s32)ca * STRAFE_SPEED) >> FP_SHIFT));
    }

    if (joy & BUTTON_C) {
        try_move(-(((s32)sa * STRAFE_SPEED) >> FP_SHIFT),
                 (((s32)ca * STRAFE_SPEED) >> FP_SHIFT));
    }

    if (pressed & BUTTON_B) {
        g_last_action_was_door = map_toggle_door_in_front(&player);
        g_weapon_recoil = 7;
    }

    if (g_weapon_recoil > 0) g_weapon_recoil--;
    g_prev_joy = joy;
}

static RayHit cast_ray(u16 ray_angle) {
    RayHit hit;
    ray_angle &= ANGLE_MASK;

    s16 ray_dx = cos_q8(ray_angle);
    s16 ray_dy = sin_q8(ray_angle);
    s16 map_x = (s16)(player.x >> FP_SHIFT);
    s16 map_y = (s16)(player.y >> FP_SHIFT);

    s32 delta_x = g_inv_abs_cos[ray_angle];
    s32 delta_y = g_inv_abs_sin[ray_angle];
    s32 side_x;
    s32 side_y;
    s16 step_x;
    s16 step_y;
    u8 side = 0;
    s32 dist = RAY_MAX;
    u8 tile = TILE_WALL;

    if (ray_dx == 0) {
        step_x = 0;
        side_x = 0x3fffffff;
    } else if (ray_dx < 0) {
        step_x = -1;
        side_x = ((player.x - ((s32)map_x << FP_SHIFT)) * delta_x) >> FP_SHIFT;
    } else {
        step_x = 1;
        side_x = (((((s32)map_x + 1) << FP_SHIFT) - player.x) * delta_x) >> FP_SHIFT;
    }

    if (ray_dy == 0) {
        step_y = 0;
        side_y = 0x3fffffff;
    } else if (ray_dy < 0) {
        step_y = -1;
        side_y = ((player.y - ((s32)map_y << FP_SHIFT)) * delta_y) >> FP_SHIFT;
    } else {
        step_y = 1;
        side_y = (((((s32)map_y + 1) << FP_SHIFT) - player.y) * delta_y) >> FP_SHIFT;
    }

    for (u8 i = 0; i < DDA_MAX_STEPS; i++) {
        if (side_x < side_y) {
            map_x += step_x;
            dist = side_x;
            side_x += delta_x;
            side = 0;
        } else {
            map_y += step_y;
            dist = side_y;
            side_y += delta_y;
            side = 1;
        }

        tile = map_cell(map_x, map_y);
        if (tile != TILE_EMPTY) break;
    }

    if (tile == TILE_EMPTY) {
        tile = TILE_WALL;
        dist = RAY_MAX;
    }

    s32 hx = player.x + (((s32)ray_dx * dist) >> FP_SHIFT);
    s32 hy = player.y + (((s32)ray_dy * dist) >> FP_SHIFT);
    u8 tex_x = (u8)(((side == 0 ? hy : hx) & (FP_ONE - 1)) >> 2);

    if ((side == 0 && ray_dx > 0) || (side == 1 && ray_dy < 0)) {
        tex_x = 63 - tex_x;
    }

    hit.tile = tile;
    hit.side = side;
    hit.tex_x = tex_x;
    hit.map_x = map_x;
    hit.map_y = map_y;
    hit.dist = dist;
    return hit;
}

static void render_background(void) {
    const s16 horizon = VIEW_Y + (VIEW_H / 2);

    // This covers the whole 3D area, so render_frame no longer needs BMP_clear().
    draw_rect(VIEW_X, VIEW_Y, VIEW_W, VIEW_H / 4, C_CEIL_DARK);
    draw_rect(VIEW_X, VIEW_Y + VIEW_H / 4, VIEW_W, VIEW_H / 4, C_CEIL_LIGHT);
    draw_rect(VIEW_X, horizon, VIEW_W, VIEW_H / 4, C_FLOOR_LIGHT);
    draw_rect(VIEW_X, horizon + VIEW_H / 4, VIEW_W, VIEW_H / 4, C_FLOOR_DARK);
}

static inline void draw_column4_unclipped(s16 sx, s16 y, u8 color) {
    BMP_setPixelFast((u16)(sx + 0), (u16)y, color);
    BMP_setPixelFast((u16)(sx + 1), (u16)y, color);
    BMP_setPixelFast((u16)(sx + 2), (u16)y, color);
    BMP_setPixelFast((u16)(sx + 3), (u16)y, color);
}

static void render_wall_column(s16 sx, s16 y0, s16 wall_h, const RayHit *hit, s32 corrected) {
    if (wall_h <= 0) return;

    s16 y1 = y0 + wall_h - 1;
    if (y1 < 0 || y0 >= VIEW_H) return;

    s16 clip_y0 = y0;
    s16 clip_y1 = y1;
    if (clip_y0 < 0) clip_y0 = 0;
    if (clip_y1 >= VIEW_H) clip_y1 = VIEW_H - 1;

    // 0.0.3 performance fix: one division per wall column instead of one division
    // per rendered wall scanline. This is much friendlier to the 68000.
    s32 tex_acc = (((s32)(clip_y0 - y0)) << 14) / wall_h;
    s32 tex_step = (64 << 14) / wall_h;

    for (s16 y = clip_y0; y <= clip_y1; y++) {
        u8 tex_y = (u8)((tex_acc >> 14) & 63);
        u8 color = wall_texel_color(hit->tile, hit->side, hit->tex_x, tex_y, corrected);
        draw_column4_unclipped(sx, y, color);
        tex_acc += tex_step;
    }

    // Very cheap wall edge cue.
    if (hit->side) draw_vline(sx, clip_y0, clip_y1, C_WALL_DARK);
}

static void render_3d(void) {
    const s16 horizon = VIEW_Y + (VIEW_H / 2);

    render_background();

    for (u16 col = 0; col < VIEW_COLS; col++) {
        s16 rel = g_ray_rel[col];
        u16 ray_angle = (player.angle + rel) & ANGLE_MASK;
        RayHit hit = cast_ray(ray_angle);

        // Fish-eye correction, now using a per-column precomputed cosine table.
        s32 corrected = ((s32)hit.dist * g_ray_corr[col]) >> FP_SHIFT;
        if (corrected < 24) corrected = 24;
        if (corrected > RAY_MAX) corrected = RAY_MAX;

        s16 wall_h = (VIEW_H * 224) / (s16)corrected;
        if (wall_h > VIEW_H * 2) wall_h = VIEW_H * 2;
        if (wall_h < 2) wall_h = 2;

        s16 y0 = horizon - (wall_h / 2);
        s16 sx = VIEW_X + (col * COL_W);
        render_wall_column(sx, y0, wall_h, &hit, corrected);

        g_depth[col] = (s16)corrected;
    }

    // Small crosshair. Useful once B becomes fire/action.
    draw_hline((VIEW_W / 2) - 3, (VIEW_W / 2) + 3, horizon, C_WHITE);
    draw_vline(VIEW_W / 2, horizon - 3, horizon + 3, C_WHITE);
}

static void render_actor(const Actor *actor) {
    if (!actor->alive) return;

    s32 dx = actor->x - player.x;
    s32 dy = actor->y - player.y;
    s16 ca = cos_q8(player.angle);
    s16 sa = sin_q8(player.angle);

    // Transform actor into camera space.
    s32 forward = (((s32)ca * dx) + ((s32)sa * dy)) >> FP_SHIFT;
    s32 right = (-((s32)sa * dx) + ((s32)ca * dy)) >> FP_SHIFT;

    if (forward < FP_ONE / 3 || forward > RAY_MAX) return;

    s16 screen_x = (VIEW_W / 2) + (s16)((right * 96) / forward);
    s16 sprite_h = (VIEW_H * 160) / (s16)forward;
    if (sprite_h < 6) sprite_h = 6;
    if (sprite_h > ACTOR_MAX_SCREEN_H) sprite_h = ACTOR_MAX_SCREEN_H;

    s16 sprite_w = sprite_h / 2;
    if (sprite_w < 4) sprite_w = 4;

    s16 x0 = screen_x - sprite_w / 2;
    s16 y0 = (VIEW_H / 2) - sprite_h / 2;
    s16 y1 = y0 + sprite_h - 1;

    for (s16 x = x0; x < x0 + sprite_w; x += ACTOR_X_STEP) {
        if (x < 0 || x >= VIEW_W) continue;

        u16 col = (u16)x / COL_W;
        if (col >= VIEW_COLS) continue;
        if (forward >= g_depth[col]) continue;

        s16 body_top = y0 + sprite_h / 4;
        s16 body_bot = y1;
        s16 head_bot = body_top - 1;

        if (head_bot >= 0 && y0 < VIEW_H) {
            draw_vline(x, y0, head_bot, C_DOOR_LIGHT);
            if (ACTOR_X_STEP == 2 && x + 1 < VIEW_W) draw_vline(x + 1, y0, head_bot, C_DOOR_LIGHT);
        }
        if (body_bot >= 0 && body_top < VIEW_H) {
            u8 c = ((x + g_frame) & 2) ? C_TECH_DARK : C_TECH_LIGHT;
            draw_vline(x, body_top, body_bot, c);
            if (ACTOR_X_STEP == 2 && x + 1 < VIEW_W) draw_vline(x + 1, body_top, body_bot, c);
        }
    }
}

static void render_weapon(void) {
    s16 recoil = (s16)g_weapon_recoil;
    s16 yoff = recoil;

    // Better placeholder pistol: barrel, grip, sight and tiny muzzle flash on B.
    draw_rect(103, 121 + yoff, 50, 24, C_HUD_BG);
    draw_rect_outline(103, 121 + yoff, 50, 24, C_HUD_LINE);
    draw_rect(118, 108 + yoff, 22, 18, C_HUD_BG);
    draw_rect_outline(118, 108 + yoff, 22, 18, C_HUD_LINE);
    draw_rect(123, 92 + yoff, 10, 25, C_WHITE);
    draw_rect(121, 90 + yoff, 14, 4, C_HUD_LINE);
    draw_rect(93, 144 + yoff, 70, 7, C_WHITE);

    if (g_weapon_recoil > 4 && !g_last_action_was_door) {
        draw_rect(119, 82, 18, 8, C_DOOR_LIGHT);
        draw_rect(124, 76, 8, 18, C_WHITE);
    }
}

static void render_minimap(void) {
#if ENABLE_MINIMAP
    const s16 ox = 4;
    const s16 oy = HUD_Y + 5;
    const s16 cell = 3;

    draw_rect_outline(ox - 2, oy - 2, MAP_W * cell + 3, MAP_H * cell + 3, C_HUD_LINE);

    for (s16 y = 0; y < MAP_H; y++) {
        for (s16 x = 0; x < MAP_W; x++) {
            u8 tile = g_world_map[y][x];
            u8 c = C_FLOOR_DARK;
            if (tile == TILE_WALL) c = C_WALL_LIGHT;
            else if (tile == TILE_DOOR) c = C_DOOR_LIGHT;
            else if (tile == TILE_TECH) c = C_TECH_LIGHT;
            draw_rect(ox + x * cell, oy + y * cell, cell - 1, cell - 1, c);
        }
    }

    if (imp.alive) {
        s16 ex = ox + (s16)((imp.x >> FP_SHIFT) * cell);
        s16 ey = oy + (s16)((imp.y >> FP_SHIFT) * cell);
        draw_rect(ex - 1, ey - 1, 3, 3, C_DOOR_LIGHT);
    }

    s16 px = ox + (s16)((player.x >> FP_SHIFT) * cell);
    s16 py = oy + (s16)((player.y >> FP_SHIFT) * cell);
    draw_rect(px - 1, py - 1, 3, 3, C_PLAYER);

    s16 fx = px + (((s32)cos_q8(player.angle) * 6) >> FP_SHIFT);
    s16 fy = py + (((s32)sin_q8(player.angle) * 6) >> FP_SHIFT);
    draw_line(px, py, fx, fy, C_PLAYER);
#endif
}

static void render_hud(void) {
    draw_rect(0, HUD_Y, CANVAS_W, HUD_H, C_HUD_BG);
    draw_hline(0, CANVAS_W - 1, HUD_Y, C_HUD_LINE);
    render_minimap();

    BMP_drawText(VERSION_STRING, 8, 15);
    BMP_drawText("D-PAD MOVE/TURN  A/C STRAFE", 8, 17);
    BMP_drawText("B ACTION/FIRE", 8, 19);

    if (g_last_action_was_door && g_weapon_recoil > 0) {
        BMP_drawText("DOOR", 25, 19);
    }
}

static void render_frame(void) {
    BMP_waitWhileFlipRequestPending();

    // No BMP_clear(): render_3d() and render_hud() fully cover the frame.
    render_3d();
    render_actor(&imp);
    render_hud();
    render_weapon();

    BMP_flip(TRUE);
}

int main(bool hardReset) {
    (void)hardReset;

    VDP_setScreenWidth320();
    VDP_setScreenHeight224();
    SYS_disableInts();

    setup_palette();
    JOY_init();
    map_init();
    init_perf_tables();

    // double buffer = TRUE, plane = BG_B, palette = 0, low priority.
    BMP_init(TRUE, BG_B, 0, FALSE);
    BMP_setBufferCopy(FALSE);

    SYS_enableInts();

    while (TRUE) {
        update_input();
        render_frame();
        g_frame++;
        SYS_doVBlankProcess();
    }

    return 0;
}
