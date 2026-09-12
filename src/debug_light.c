#include "debug_light.h"
#include "bsp_map.h"

// Two rows at the very top of BG_B. The top letterbox is VIEW_TILEMAP_Y rows
// tall, which is 4 at the tallest viewport preset (16 rows of 24), so both rows
// clear the view at every size. BG_B is never scrolled (BG_A carries the weapon
// bob), so the text holds still.
#define DEBUG_LIGHT_W 40
#define DEBUG_LIGHT_ROWS 2
// Held vblanks of A+B+C before a rescue fires, so a normal fire+use+run chord
// cannot trigger it by accident.
#define DEBUG_LIGHT_RESCUE_VBLANKS 60
#define DEBUG_LIGHT_RESCUE_BUTTONS (BUTTON_A | BUTTON_B | BUTTON_C)

static bool s_enabled = DEBUG_LIGHT_BOOT;
// Parity of detected crossings: an odd count means the player is on the far
// side of a wall from where the level put them. Only the inside pose is kept.
static bool s_outside;
static u16 s_crossings;
static s32 s_safe_x;
static s32 s_safe_y;
static s16 s_knock_x;
static s16 s_knock_y;
static u16 s_rescue_hold;
static bool s_dirty = TRUE;

static char s_text[DEBUG_LIGHT_W];
static u16 s_cursor;
static u16 s_tiles[DEBUG_LIGHT_W * DEBUG_LIGHT_ROWS];

// What the rows last showed, so a still player costs no tilemap writes.
static s32 s_last_x;
static s32 s_last_y;
static u16 s_last_angle;
static u16 s_last_phase;
static u16 s_last_crossings;
static s16 s_last_knock_x;
static s16 s_last_knock_y;

bool debug_light_enabled(void) { return s_enabled; }

void debug_light_set_enabled(bool enabled) {
    s_enabled = enabled;
    s_dirty = TRUE;
}

void debug_light_level_start(const PlayerState *player) {
    s_outside = FALSE;
    s_crossings = 0;
    s_safe_x = player->x;
    s_safe_y = player->y;
    s_knock_x = 0;
    s_knock_y = 0;
    s_rescue_hold = 0;
    s_dirty = TRUE;
}

void debug_light_invalidate(void) { s_dirty = TRUE; }

void debug_light_note_hop(s32 from_x, s32 from_y, s32 to_x, s32 to_y) {
    if (!s_enabled) return;
    // Open doors and triggers do not count: bsp_segment_crosses_wall skips
    // exactly the segs collision skips.
    if (bsp_segment_crosses_wall(from_x, from_y, to_x, to_y)) {
        if (s_crossings != 0xFFFF) s_crossings++;
        s_outside = !s_outside;
        return;
    }
    if (!s_outside) {
        s_safe_x = to_x;
        s_safe_y = to_y;
    }
}

void debug_light_note_knockback(s32 dx, s32 dy) {
    s_knock_x = (s16)dx;
    s_knock_y = (s16)dy;
}

bool debug_light_update_rescue(PlayerState *player, u16 held_joy, u16 elapsed_vblanks) {
    if (!s_enabled ||
        (held_joy & DEBUG_LIGHT_RESCUE_BUTTONS) != DEBUG_LIGHT_RESCUE_BUTTONS) {
        s_rescue_hold = 0;
        return FALSE;
    }
    if (s_rescue_hold < DEBUG_LIGHT_RESCUE_VBLANKS) {
        s_rescue_hold = (u16)(s_rescue_hold + elapsed_vblanks);
        if (s_rescue_hold < DEBUG_LIGHT_RESCUE_VBLANKS) return FALSE;
        // Fires once per hold; releasing re-arms it.
        if (!s_outside) return FALSE;
        player->x = s_safe_x;
        player->y = s_safe_y;
        s_outside = FALSE;
        s_dirty = TRUE;
        return TRUE;
    }
    return FALSE;
}

static void text_begin(void) {
    for (u16 i = 0; i < DEBUG_LIGHT_W; i++) s_text[i] = ' ';
    s_cursor = 0;
}

static void put_char(char value) {
    if (s_cursor < DEBUG_LIGHT_W) s_text[s_cursor++] = value;
}

static void put_text(const char *value) {
    while (*value != 0) put_char(*value++);
}

// Sign, then `digits` digits of |value| (saturated), zero-padded. Map
// coordinates fit s16, so repeated subtraction stays in 16 bits and pulls in no
// divide helper.
static void put_signed(s32 value, u16 digits, bool plus) {
    static const u16 powers[5] = {10000, 1000, 100, 10, 1};
    u16 magnitude;
    if (value < 0) {
        put_char('-');
        value = -value;
    } else {
        put_char(plus ? '+' : ' ');
    }
    magnitude = (value > 65535) ? 65535u : (u16)value;
    for (u16 i = (u16)(5 - digits); i < 5; i++) {
        char digit = '0';
        while (magnitude >= powers[i]) {
            magnitude = (u16)(magnitude - powers[i]);
            digit++;
        }
        put_char((digit > '9') ? '9' : digit);
    }
}

static void put_unsigned(u16 value, u16 digits) {
    static const u16 powers[5] = {10000, 1000, 100, 10, 1};
    for (u16 i = (u16)(5 - digits); i < 5; i++) {
        char digit = '0';
        while (value >= powers[i]) {
            value = (u16)(value - powers[i]);
            digit++;
        }
        put_char((digit > '9') ? '9' : digit);
    }
}

static void text_commit(u16 row) {
    u16 *target = &s_tiles[row * DEBUG_LIGHT_W];
    for (u16 i = 0; i < DEBUG_LIGHT_W; i++) {
        target[i] = TILE_ATTR_FULL(PAL0, FALSE, FALSE, FALSE,
                                   TILE_FONT_INDEX + ((u8)s_text[i] - 32));
    }
}

void debug_light_draw(const PlayerState *player, u16 phase_index) {
    if (!s_enabled) return;
    if (!s_dirty && player->x == s_last_x && player->y == s_last_y &&
        player->angle == s_last_angle && phase_index == s_last_phase &&
        s_crossings == s_last_crossings &&
        s_knock_x == s_last_knock_x && s_knock_y == s_last_knock_y) {
        return;
    }
    s_dirty = FALSE;
    s_last_x = player->x;
    s_last_y = player->y;
    s_last_angle = player->angle;
    s_last_phase = phase_index;
    s_last_crossings = s_crossings;
    s_last_knock_x = s_knock_x;
    s_last_knock_y = s_knock_y;

    const u16 subsector = bsp_find_subsector(player->x, player->y);
    const u16 sector = (subsector < bsp_subsector_count) ?
        bsp_subsector_sector[subsector] : 0xFFFF;

    // Whole-word labels: in SGDK's 8x8 font a lone "S" before digits reads as
    // a 5 once a tester has photographed the screen.
    // "E1M3 X-1824 Y 2256 ANG128 SEC057"
    text_begin();
    put_text("E1M");
    put_unsigned((u16)((phase_index % 9) + 1), 1);
    put_text(" X");
    put_signed(player->x, 4, FALSE);
    put_text(" Y");
    put_signed(player->y, 4, FALSE);
    put_text(" ANG");
    put_unsigned(player->angle, 3);
    put_text(" SEC");
    put_unsigned(sector, 3);
    text_commit(0);

    // "DEBUG ON  WALL CROSS 00  KNOCK +64-64"
    text_begin();
    put_text(s_outside ? "OUTSIDE! HOLD A+B+C" : "DEBUG ON");
    put_text("  WALL CROSS ");
    put_unsigned(s_crossings, 2);
    if (!s_outside) {
        put_text("  KNOCK");
        put_signed(s_knock_x, 2, TRUE);
        put_signed(s_knock_y, 2, TRUE);
    }
    text_commit(1);

    VDP_setTileMapDataRect(BG_B, s_tiles, 0, 0, DEBUG_LIGHT_W, DEBUG_LIGHT_ROWS,
                           DEBUG_LIGHT_W, CPU);
}
