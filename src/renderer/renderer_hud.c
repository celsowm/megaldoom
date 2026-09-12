#include "renderer_internal.h"
#include "generated_hud_assets.h"

// Native Doom number anchors are pixel coordinates in the 320px STBAR. The
// right edge belongs to the number; percentage glyphs begin at that same X.
#define HUD_NUMBER_PIXEL_Y ((HUD_PANEL_Y * 8) + 3)
#define HUD_AMMO_RIGHT_X 44
#define HUD_HEALTH_RIGHT_X 90
#define HUD_FRAGS_RIGHT_X 138
#define HUD_ARMOR_RIGHT_X 221

typedef struct {
    u8 tile_x;
    u8 tile_w;
    u16 vram_offset;
    u16 right_x;
    u8 min_digits;
    u8 max_digits;
    bool percent;
} HudNumberField;

static const HudNumberField HUD_AMMO_FIELD = {
    0, HUD_NUMBER_AMMO_TILE_W, 0, HUD_AMMO_RIGHT_X, 2, 3, FALSE
};
static const HudNumberField HUD_HEALTH_FIELD = {
    6, HUD_NUMBER_HEALTH_TILE_W, HUD_NUMBER_AMMO_TILE_COUNT,
    HUD_HEALTH_RIGHT_X, 3, 3, TRUE
};
static const HudNumberField HUD_FRAGS_FIELD = {
    13, HUD_NUMBER_FRAGS_TILE_W,
    HUD_NUMBER_AMMO_TILE_COUNT + HUD_NUMBER_HEALTH_TILE_COUNT,
    HUD_FRAGS_RIGHT_X, 2, 2, FALSE
};
static const HudNumberField HUD_ARMOR_FIELD = {
    22, HUD_NUMBER_ARMOR_TILE_W,
    HUD_NUMBER_AMMO_TILE_COUNT + HUD_NUMBER_HEALTH_TILE_COUNT + HUD_NUMBER_FRAGS_TILE_COUNT,
    HUD_ARMOR_RIGHT_X, 2, 3, TRUE
};

// The number canvas is composed on the STACK, not in .bss. It is live only
// inside draw_hud_number_ex(), and 768 bytes of permanently resident static data
// is 768 bytes the SGDK heap does not have: the frontend unpacks its boot cards
// through MEM_alloc, and starving that heap is a boot-time crash, not a slow
// frame. The larger viewport sizes spend most of the work-RAM headroom, so this
// buffer pays its way back by being transient.
//
// DMA'ing from the stack is safe here because the transfer method is DMA (not
// DMA_QUEUE): DMA_transfer runs synchronously inside the call, while a queued
// transfer would read the buffer after the frame has returned.
typedef u32 HudNumberScratch[HUD_NUMBER_MAX_FIELD_TILES][8];

static void clear_number_scratch(HudNumberScratch scratch, u16 tile_count) {
    for (u16 tile = 0; tile < tile_count; tile++) {
        for (u16 row = 0; row < 8; row++) {
            scratch[tile][row] = 0;
        }
    }
}

static void set_number_pixel(HudNumberScratch scratch, const HudNumberField *field,
                             s16 x, s16 y, u8 color) {
    if (x < 0 || y < 0 || x >= (s16)(field->tile_w * 8) ||
        y >= (s16)(HUD_NUMBER_TILE_H * 8) || color == 0) {
        return;
    }
    const u16 tile_x = (u16)x >> 3;
    const u16 tile_y = (u16)y >> 3;
    const u16 tile = (u16)(tile_y * field->tile_w + tile_x);
    const u16 shift = (u16)((7 - (x & 7)) * 4);
    const u32 mask = (u32)0x0Fu << shift;
    scratch[tile][y & 7] =
        (scratch[tile][y & 7] & ~mask) | ((u32)color << shift);
}

static void draw_number_glyph(HudNumberScratch scratch, const HudNumberField *field,
                              u8 glyph, s16 x, s16 y) {
    for (u16 py = 0; py < FREEDOOM_HUD_DIGIT_CANVAS_H; py++) {
        for (u16 px = 0; px < FREEDOOM_HUD_DIGIT_WIDTHS[glyph]; px++) {
            set_number_pixel(scratch, field, (s16)(x + px), (s16)(y + py),
                             FREEDOOM_HUD_DIGITS[glyph][py][px]);
        }
    }
}

static u8 format_number(u16 value, u8 min_digits, u8 max_digits, u8 *digits) {
    const u16 limit = (max_digits == 3) ? 999 : 99;
    if (value > limit) value = limit;
    u8 count = min_digits;
    if (max_digits == 3 && value >= 100) count = 3;
    for (u8 i = count; i > 0; i--) {
        digits[i - 1] = (u8)(value % 10);
        value = (u16)(value / 10);
    }
    return count;
}

// `blank` composes an empty field (the transparent scratch alone), used for the
// melee weapons' ammo slot: Doom shows nothing there rather than a zero.
static void draw_hud_number_ex(const HudNumberField *field, u16 value, bool blank) {
    const u16 tile_count = (u16)(field->tile_w * HUD_NUMBER_TILE_H);
    const s16 field_pixel_x = (s16)(field->tile_x * 8);
    const s16 local_y = (s16)(HUD_NUMBER_PIXEL_Y - (HUD_PANEL_Y * 8));
    s16 right = (s16)(field->right_x - field_pixel_x);
    u8 digits[3];
    HudNumberScratch scratch;
    const u8 count = blank ? 0
        : format_number(value, field->min_digits, field->max_digits, digits);

    clear_number_scratch(scratch, tile_count);
    if (field->percent && !blank) {
        draw_number_glyph(scratch, field, FREEDOOM_HUD_DIGIT_PERCENT, right, local_y);
    }
    for (u8 i = count; i > 0; i--) {
        const u8 glyph = digits[i - 1];
        right = (s16)(right - FREEDOOM_HUD_DIGIT_WIDTHS[glyph]);
        draw_number_glyph(scratch, field, glyph, right, local_y);
    }
    VDP_loadTileData((const u32 *)scratch,
                     (u16)(HUD_NUMBER_TILE_BASE + field->vram_offset),
                     tile_count, DMA);
}

static void draw_hud_number(const HudNumberField *field, u16 value) {
    draw_hud_number_ex(field, value, FALSE);
}

// The number fields sit on the WINDOW plane, not BG_A: BG_A is whole-plane
// scrolled for weapon bob (renderer_frame_overlay.c) and the status bar must not
// ride along. The window covers the bottom HUD_PANEL_H rows; its other cells are
// transparent tile 0 so the BG_B backdrop and face show through exactly as when
// this lived on BG_A.
static void draw_hud_number_tilemap(void) {
    const HudNumberField *fields[4] = {
        &HUD_AMMO_FIELD, &HUD_HEALTH_FIELD, &HUD_FRAGS_FIELD, &HUD_ARMOR_FIELD
    };
    for (u16 field_index = 0; field_index < 4; field_index++) {
        const HudNumberField *field = fields[field_index];
        for (u16 y = 0; y < HUD_NUMBER_TILE_H; y++) {
            for (u16 x = 0; x < field->tile_w; x++) {
                const u16 tile_id = (u16)(HUD_NUMBER_TILE_BASE + field->vram_offset +
                                          y * field->tile_w + x);
                VDP_setTileMapXY(WINDOW, TILE_ATTR_FULL(PAL1, FALSE, FALSE, FALSE, tile_id),
                                 (u16)(field->tile_x + x), (u16)(HUD_PANEL_Y + y));
            }
        }
    }
}

// Doom's key-card box, right of the armor field. Doom draws the icons at x=239,
// but the armor field owns window tiles 22..29 and its percent sign reaches
// x=234, so the icons take tile column 30 alone, one pixel right (still inside
// the box interior, x 236..246). Rows are Doom's ST_KEY0Y..2Y minus the bar top.
#define HUD_KEY_TILE_X 30
#define HUD_KEY_PIXEL_X 240
static const u8 HUD_KEY_PIXEL_Y[FREEDOOM_HUD_KEY_COUNT] = { 3, 13, 23 };
static const u8 HUD_KEY_MASKS[FREEDOOM_HUD_KEY_COUNT] = {
    BSP_KEY_BLUE, BSP_KEY_YELLOW, BSP_KEY_RED
};

// Recomposes the whole column from transparent each time, so a level reset that
// clears the keys also clears the icons. The 128-byte canvas is on the stack
// for the same heap reason as the number scratch above.
static void draw_hud_keys(u8 key_mask) {
    u32 scratch[HUD_KEY_TILE_COUNT][8];
    for (u16 tile = 0; tile < HUD_KEY_TILE_COUNT; tile++) {
        for (u16 row = 0; row < 8; row++) {
            scratch[tile][row] = 0;
        }
    }
    const u16 local_x = (u16)(HUD_KEY_PIXEL_X - (HUD_KEY_TILE_X * 8));
    for (u16 key = 0; key < FREEDOOM_HUD_KEY_COUNT; key++) {
        if (!(key_mask & HUD_KEY_MASKS[key])) {
            continue;
        }
        for (u16 py = 0; py < FREEDOOM_HUD_KEY_H; py++) {
            const u16 y = (u16)(HUD_KEY_PIXEL_Y[key] + py);
            for (u16 px = 0; px < FREEDOOM_HUD_KEY_W; px++) {
                const u8 color = FREEDOOM_HUD_KEYS[key][py][px];
                if (color == 0) {
                    continue;
                }
                const u16 shift = (u16)((7 - (local_x + px)) * 4);
                scratch[y >> 3][y & 7] |= (u32)color << shift;
            }
        }
    }
    VDP_loadTileData((const u32 *)scratch, HUD_KEY_TILE_BASE, HUD_KEY_TILE_COUNT, DMA);
}

static void draw_hud_key_tilemap(void) {
    for (u16 y = 0; y < HUD_KEY_TILE_COUNT; y++) {
        VDP_setTileMapXY(WINDOW,
                         TILE_ATTR_FULL(PAL0, FALSE, FALSE, FALSE,
                                        (u16)(HUD_KEY_TILE_BASE + y)),
                         HUD_KEY_TILE_X, (u16)(HUD_PANEL_Y + y));
    }
}

void renderer_hud_window_setup(void) {
    // The window spans the black gutter under the centred view AND the status
    // bar (VIEW_WINDOW_TILE_H rows), not just the status bar. Plane A is
    // suppressed over that whole region, which is what clips the weapon's
    // downward bob dip at the view's bottom edge -- the job the view being
    // parked flush on the status bar used to do. The gutter cells stay
    // transparent tile 0 so BG_B's black letterbox shows through unchanged.
    VDP_fillTileMapRect(WINDOW, 0, 0, VIEW_WINDOW_TOP_Y, SCREEN_TILE_W,
                        VIEW_WINDOW_TILE_H);
    VDP_setWindowOnBottom(VIEW_WINDOW_TILE_H);
    draw_hud_number_tilemap();
    draw_hud_key_tilemap();
}

void renderer_hud_window_suspend(void) {
    // The pause/menu panels draw a full-screen BG_A image; the window plane
    // would otherwise keep the stale status numbers painted over their bottom.
    VDP_setWindowOff();
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
// path only rewrites the 16 face tiles when the expression actually changes.
static u16 s_last_face_frame = 0xFFFF;

// The portrait is a STREAMING WINDOW of FACE_VRAM_TILE_COUNT tiles, not a
// resident 258-tile atlas -- see the FACE_TILE_BASE note in
// renderer_internal.h. The tilemap under the face never changes; switching
// expression DMAs the new frame's 16 tiles over the window instead.
//
// The baked set deduplicates tiles across expressions, so a frame's 16 entries
// are arbitrary indices into FREEDOOM_FACE_TILES rather than a contiguous run.
// That is why this is 16 one-tile transfers and not a single block copy. It
// costs 512 bytes of VRAM traffic and runs only when the expression actually
// changes (an HP bracket crossing or the idle glance cycle, a few times a
// second at most), which is the same budget the weapon window already spends
// on a weapon switch.
static void upload_face_frame(u16 frame_index) {
    for (u16 tile = 0; tile < FREEDOOM_FACE_FRAME_TILES; tile++) {
        const u16 src = FREEDOOM_FACE_FRAME_TILE_IDS[frame_index][tile];
        VDP_loadTileData(FREEDOOM_FACE_TILES[src],
                         (u16)(FACE_TILE_BASE + tile), 1, DMA);
    }
}

// The face cell's tilemap: a fixed 4x4 block pointing straight at the window.
// Written once per static-screen build; the per-frame path only re-uploads
// pixels underneath it.
static void draw_hud_face_tilemap(void) {
    for (u16 y = 0; y < FREEDOOM_FACE_TILE_H; y++) {
        for (u16 x = 0; x < FREEDOOM_FACE_TILE_W; x++) {
            const u16 frame_tile = (u16)(y * FREEDOOM_FACE_TILE_W + x);
            VDP_setTileMapXY(BG_B,
                             TILE_ATTR_FULL(PAL2, FALSE, FALSE, FALSE,
                                            (u16)(FACE_TILE_BASE + frame_tile)),
                             (u16)(HUD_FACE_TILE_X + x),
                             (u16)(HUD_FACE_TILE_Y + y));
        }
    }
}

static void draw_hud_face(u16 frame_index) {
    if (frame_index == s_last_face_frame) {
        return;
    }
    s_last_face_frame = frame_index;
    upload_face_frame(frame_index);
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

// Cached copies of the four HUD numbers last composed into BG_A, so unchanged
// values issue no tile DMA. Sentinel 0xFFFF forces a repaint after reset.
static u16 s_last_ammo = 0xFFFF;
static u16 s_last_health = 0xFFFF;
static u16 s_last_frags = 0xFFFF;
static u16 s_last_armor = 0xFFFF;
// Key bits last composed; 0xFF is never a real mask (BSP_KEY_ALL is 0x07).
static u8 s_last_keys = 0xFF;

void renderer_draw_static_screen(void) {
    draw_hud_backdrop();
    renderer_hud_window_setup();
    draw_hud_face_tilemap();
    s_last_face_frame = 0xFFFF;
    s_last_ammo = 0xFFFF;
    s_last_health = 0xFFFF;
    s_last_frags = 0xFFFF;
    s_last_armor = 0xFFFF;
    s_last_keys = 0xFF;
    draw_hud_face((u16)FACE_FRAME_ST(0, 1));
}

void renderer_draw_hud(const RendererHudState *state) {
    // 0xFFFE is the "field is blank" cache key. It cannot collide with a real
    // count: format_number clamps this field at 999, and 0xFFFF is already the
    // force-repaint sentinel.
    const u16 ammo_key = state->ammo_visible ? state->ammo : 0xFFFE;
    if (ammo_key != s_last_ammo) {
        draw_hud_number_ex(&HUD_AMMO_FIELD, state->ammo, !state->ammo_visible);
        s_last_ammo = ammo_key;
    }
    if (state->health_percent != s_last_health) {
        draw_hud_number(&HUD_HEALTH_FIELD, state->health_percent);
        s_last_health = state->health_percent;
    }
    if (state->enemy_count != s_last_frags) {
        draw_hud_number(&HUD_FRAGS_FIELD, state->enemy_count);
        s_last_frags = state->enemy_count;
    }
    if (state->armor != s_last_armor) {
        draw_hud_number(&HUD_ARMOR_FIELD, state->armor);
        s_last_armor = state->armor;
    }
    if (state->key_mask != s_last_keys) {
        draw_hud_keys(state->key_mask);
        s_last_keys = state->key_mask;
    }

    draw_hud_face(compute_face_frame(state));
}
