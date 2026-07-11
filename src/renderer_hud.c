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

// Remembers which portrait frame is currently on the tilemap so the per-frame
// path only rewrites the 12 face tiles when the expression actually changes.
static u16 s_last_face_frame = 0xFFFF;

static void draw_hud_face(u16 frame_index) {
    if (frame_index == s_last_face_frame) {
        return;
    }
    s_last_face_frame = frame_index;

    // Each portrait frame is a consecutive 3x4-tile block inside FREEDOOM_FACE_TILES.
    const u16 base = (u16)(FACE_TILE_BASE + (frame_index * FREEDOOM_FACE_FRAME_TILES));
    for (u16 y = 0; y < FREEDOOM_FACE_TILE_H; y++) {
        for (u16 x = 0; x < FREEDOOM_FACE_TILE_W; x++) {
            const u16 tile_id = (u16)(base + (y * FREEDOOM_FACE_TILE_W) + x);
            VDP_setTileMapXY(BG_B,
                             TILE_ATTR_FULL(PAL2, FALSE, FALSE, FALSE, tile_id),
                             (u16)(HUD_FACE_TILE_X + x),
                             (u16)(HUD_FACE_TILE_Y + y));
        }
    }
}

// Pick the portrait expression: dead -> pain -> HP bracket with an idle glance
// cycle. Bracket 0 = high HP .. 4 = near death, derived from health percent.
static u16 compute_face_frame(const RendererHudState *state) {
    u16 hp = state->health_percent;
    if (hp > 100) {
        hp = 100;
    }
    u16 bracket = (u16)((100 - hp) / 20);
    if (bracket > 4) {
        bracket = 4;
    }

    if (hp == 0) {
        return (u16)FACE_FRAME_DEAD;
    }
    if (state->portrait_state == 1) {
        // Just took damage: grimace for this HP bracket.
        return (u16)FACE_FRAME_OUCH(bracket);
    }

    // Idle: glance around now and then (mostly forward), like the original HUD.
    static const u8 dir_table[4] = {1, 0, 1, 2};
    const u16 dir = dir_table[(state->frame >> 4) & 3];
    return (u16)FACE_FRAME_ST(bracket, dir);
}

// Cached copies of the four HUD numbers last written to the tilemap, so the
// per-frame path only issues VDP_drawText (slow per-tile VRAM writes) for values
// that actually changed. Sentinel 0xFFFF forces a full repaint after a reset.
static u16 s_last_ammo = 0xFFFF;
static u16 s_last_health = 0xFFFF;
static u16 s_last_frags = 0xFFFF;
static u16 s_last_armor = 0xFFFF;

// Screen is 320x224 = 40x28 tiles (VDP_setScreenWidth320 / Height224 in renderer.c).
#define SCREEN_TILE_W 40
#define SCREEN_TILE_H 28

// Viewport window bounds, in tiles, derived from the view tilemap placement.
#define VIEW_TX0 VIEW_TILEMAP_X
#define VIEW_TY0 VIEW_TILEMAP_Y
#define VIEW_TX1 (VIEW_TILEMAP_X + VIEW_TILE_W - 1)
#define VIEW_TY1 (VIEW_TILEMAP_Y + VIEW_TILE_H - 1)

// Bezel colours (PAL0 indices, see renderer.c init_video): a dark panel base with a
// light top/left highlight and dark bottom/right shadow for a recessed-window look.
#define BEZEL_FILL 3
#define BEZEL_LIGHT 6
#define BEZEL_SHADOW 2

static void bezel_solid(u16 x, u16 y, u8 color) {
    renderer_set_bg_pair_tile(x, y, color, color);
}

// Fill the black margins around the 3D window with a framed panel so the small
// viewport reads as intentional instead of floating in empty space. Static: painted
// once per level reset onto BG_B, never touched per frame. Skips the live view
// window (its tilemap was uploaded once at init) and the HUD panel columns
// (draw_hud_backdrop paints those). The compass is re-uploaded on top each frame.
static void renderer_draw_bezel(void) {
    // Panel base for everything above the HUD, minus the view window itself.
    for (u16 y = 0; y < HUD_PANEL_Y; y++) {
        for (u16 x = 0; x < SCREEN_TILE_W; x++) {
            if ((x >= VIEW_TX0) && (x <= VIEW_TX1) && (y >= VIEW_TY0) && (y <= VIEW_TY1)) {
                continue;
            }
            bezel_solid(x, y, BEZEL_FILL);
        }
    }

    // Frame the sides of the HUD (columns outside its 32-wide panel).
    for (u16 y = HUD_PANEL_Y; y < SCREEN_TILE_H; y++) {
        for (u16 x = 0; x < HUD_PANEL_X; x++) {
            bezel_solid(x, y, BEZEL_FILL);
        }
        for (u16 x = (HUD_PANEL_X + HUD_PANEL_W); x < SCREEN_TILE_W; x++) {
            bezel_solid(x, y, BEZEL_FILL);
        }
    }

    // Beveled ring just outside the window: light top/left, dark bottom/right.
    const u16 fx0 = VIEW_TX0 - 1;
    const u16 fx1 = VIEW_TX1 + 1;
    const u16 fy0 = VIEW_TY0 - 1;
    const u16 fy1 = VIEW_TY1 + 1;
    for (u16 x = fx0; x <= fx1; x++) {
        bezel_solid(x, fy0, BEZEL_LIGHT);
        bezel_solid(x, fy1, BEZEL_SHADOW);
    }
    for (u16 y = fy0; y <= fy1; y++) {
        bezel_solid(fx0, y, BEZEL_LIGHT);
        bezel_solid(fx1, y, BEZEL_SHADOW);
    }
}

void renderer_draw_static_screen(void) {
    renderer_draw_bezel();
    draw_hud_backdrop();
    s_last_face_frame = 0xFFFF;
    s_last_ammo = 0xFFFF;
    s_last_health = 0xFFFF;
    s_last_frags = 0xFFFF;
    s_last_armor = 0xFFFF;
    draw_hud_face((u16)FACE_FRAME_ST(0, 1));
}

void renderer_draw_hud(const RendererHudState *state) {
    char text[4];
    const bool numbers_changed = (state->ammo != s_last_ammo) ||
                                 (state->health_percent != s_last_health) ||
                                 (state->enemy_count != s_last_frags) ||
                                 (state->armor != s_last_armor);

    // Doom-red numerals (palette line 1, foreground index 15) so they read
    // clearly against the grey metal bar. Restore the default text palette
    // afterwards so other on-screen text stays as-is. Only touch VRAM for the
    // numbers that changed since the last frame.
    if (numbers_changed) {
        VDP_setTextPalette(PAL1);

        if (state->ammo != s_last_ammo) {
            write_u16_2(state->ammo, text);
            VDP_drawText(text, HUD_AMMO_X, HUD_NUM_ROW);
            s_last_ammo = state->ammo;
        }

        if (state->health_percent != s_last_health) {
            write_u16_3(state->health_percent, text);
            VDP_drawText(text, HUD_HEALTH_X, HUD_NUM_ROW);
            s_last_health = state->health_percent;
        }

        if (state->enemy_count != s_last_frags) {
            write_u16_2(state->enemy_count, text);
            VDP_drawText(text, HUD_FRAGS_X, HUD_NUM_ROW);
            s_last_frags = state->enemy_count;
        }

        if (state->armor != s_last_armor) {
            write_u16_2(state->armor, text);
            VDP_drawText(text, HUD_ARMOR_X, HUD_NUM_ROW);
            s_last_armor = state->armor;
        }

        VDP_setTextPalette(PAL0);
    }

    draw_hud_face(compute_face_frame(state));
}
