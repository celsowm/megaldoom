#include "renderer_internal.h"
#include "generated_hud_assets.h"

// Column/row anchors for the dynamic status-bar numbers. Each number sits in the
// big-digit area of its recessed box, directly above the baked label. The face
// portrait occupies the centre slot (see HUD_FACE_TILE_X/Y).
#define HUD_NUM_ROW (HUD_PANEL_Y + 3)
#define HUD_AMMO_X (HUD_PANEL_X + 2)
#define HUD_HEALTH_X (HUD_PANEL_X + 7)
#define HUD_FRAGS_X (HUD_PANEL_X + 12)
#define HUD_ARMOR_X (HUD_PANEL_X + 19)

static void write_u16_2(u16 value, char *out) {
    if (value > 99) {
        value = 99;
    }

    out[0] = (char)('0' + (value / 10));
    out[1] = (char)('0' + (value % 10));
    out[2] = '\0';
}

static void write_u16_3(u16 value, char *out) {
    if (value > 999) {
        value = 999;
    }

    out[0] = (char)('0' + (value / 100));
    out[1] = (char)('0' + ((value / 10) % 10));
    out[2] = (char)('0' + (value % 10));
    out[3] = '\0';
}

static void draw_hud_backdrop(void) {
    for (u16 y = 0; y < FREEDOOM_HUD_TILE_H; y++) {
        for (u16 x = 0; x < FREEDOOM_HUD_TILE_W; x++) {
            const u16 tile_id = (u16)(HUD_TILE_BASE + (y * FREEDOOM_HUD_TILE_W) + x);
            VDP_setTileMapXY(BG_B, TILE_ATTR_FULL(PAL0, FALSE, FALSE, FALSE, tile_id), (u16)(HUD_PANEL_X + x), (u16)(HUD_PANEL_Y + y));
        }
    }
}

static void draw_hud_face(void) {
    // Blit the 3x4-tile Doom-guy portrait over the recessed face slot of the bar.
    for (u16 y = 0; y < FREEDOOM_FACE_TILE_H; y++) {
        for (u16 x = 0; x < FREEDOOM_FACE_TILE_W; x++) {
            const u16 tile_id = (u16)(FACE_TILE_BASE + (y * FREEDOOM_FACE_TILE_W) + x);
            VDP_setTileMapXY(BG_B,
                             TILE_ATTR_FULL(PAL0, FALSE, FALSE, FALSE, tile_id),
                             (u16)(HUD_FACE_TILE_X + x),
                             (u16)(HUD_FACE_TILE_Y + y));
        }
    }
}

void renderer_draw_static_screen(void) {
    draw_hud_backdrop();
    draw_hud_face();
    VDP_drawText(RENDERER_VERSION_TEXT, 7, 1);
    VDP_drawText("FREEDOOM VISUAL PASS", 10, 3);
}

void renderer_draw_hud(const RendererHudState *state) {
    char text[4];

    // Doom-red numerals (palette line 1, foreground index 15) so they read
    // clearly against the grey metal bar. Restore the default text palette
    // afterwards so other on-screen text stays as-is.
    VDP_setTextPalette(PAL1);

    write_u16_2(state->shot_cooldown, text);
    VDP_drawText(text, HUD_AMMO_X, HUD_NUM_ROW);

    write_u16_3(state->health_percent, text);
    VDP_drawText(text, HUD_HEALTH_X, HUD_NUM_ROW);

    write_u16_2(state->enemy_count, text);
    VDP_drawText(text, HUD_FRAGS_X, HUD_NUM_ROW);

    write_u16_2((u16)(state->pickups.key + state->pickups.bonus), text);
    VDP_drawText(text, HUD_ARMOR_X, HUD_NUM_ROW);

    VDP_setTextPalette(PAL0);
}
