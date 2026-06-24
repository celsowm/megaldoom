#include "renderer_internal.h"
#include "generated_hud_assets.h"

static const char HEX_DIGITS[] = "0123456789ABCDEF";

static void write_u16_2(u16 value, char *out) {
    if (value > 99) {
        value = 99;
    }

    out[0] = (char)('0' + (value / 10));
    out[1] = (char)('0' + (value % 10));
    out[2] = '\0';
}

static void write_hex_u32(u32 value, char *out) {
    for (u16 i = 0; i < 8; i++) {
        const u16 shift = (u16)((7 - i) * 4);
        out[i] = HEX_DIGITS[(value >> shift) & 0x0F];
    }
    out[8] = '\0';
}

static const char *last_pickup_text(BillboardPickupKind kind) {
    if (kind == BILLBOARD_PICKUP_BONUS) {
        return "BONUS";
    }
    if (kind == BILLBOARD_PICKUP_KEY) {
        return "KEY  ";
    }

    return "NONE ";
}

static const char *action_status_text(DoorActionResult action) {
    if (action == DOOR_ACTION_TOGGLED) {
        return "DOOR";
    }
    if (action == DOOR_ACTION_LOCKED) {
        return "LOCK";
    }
    if (action == DOOR_ACTION_UNLOCKED) {
        return "KEY!";
    }
    if (action == DOOR_ACTION_EXIT) {
        return "EXIT";
    }
    if (action == DOOR_ACTION_EXIT_LOCKED) {
        return "HUNT";
    }

    return "----";
}

static const char *shot_status_text(BillboardShotResult shot) {
    if (shot == BILLBOARD_SHOT_DAMAGE) {
        return "DMG ";
    }
    if (shot == BILLBOARD_SHOT_KILL) {
        return "KILL";
    }

    return "MISS";
}

static const char *portrait_line(u8 state, u16 line) {
    static const char *NORMAL[3] = {" /-\\ ", "|o o|", " \\_/ "};
    static const char *PAIN[3] = {" /-\\ ", "|x x|", " /_\\ "};
    static const char *CRIT[3] = {" /-\\ ", "|o _|", " \\o/ "};
    const char **portrait = NORMAL;

    if (state == 1) {
        portrait = PAIN;
    } else if (state == 2) {
        portrait = CRIT;
    }

    return portrait[line];
}

static void draw_hud_backdrop(void) {
    for (u16 y = 0; y < FREEDOOM_HUD_TILE_H; y++) {
        for (u16 x = 0; x < FREEDOOM_HUD_TILE_W; x++) {
            const u16 tile_id = (u16)(HUD_TILE_BASE + (y * FREEDOOM_HUD_TILE_W) + x);
            VDP_setTileMapXY(BG_B, TILE_ATTR_FULL(PAL0, FALSE, FALSE, FALSE, tile_id), (u16)(HUD_PANEL_X + x), (u16)(HUD_PANEL_Y + y));
        }
    }
}

void renderer_draw_static_screen(void) {
    draw_hud_backdrop();
    VDP_drawText(RENDERER_VERSION_TEXT, 7, 1);
    VDP_drawText("FREEDOOM VISUAL PASS", 10, 3);
    VDP_drawText("HP 00  AM 00  PH 00  EN 00", 5, 22);
    VDP_drawText("TG 00/00        KEY 00  BN 00", 5, 23);
    VDP_drawText("SHOT ----       USE ----", 5, 24);
    VDP_drawText("GOAL ----       LAST -----", 5, 25);
    VDP_drawText("FRAME 00000000", 5, 26);
}

void renderer_draw_hud(const RendererHudState *state) {
    char text[9];

    write_u16_2(state->player_health, text);
    VDP_drawText(text, 8, 22);

    write_u16_2(state->shot_cooldown, text);
    VDP_drawText(text, 15, 22);

    write_u16_2(state->phase, text);
    VDP_drawText(text, 22, 22);

    write_u16_2(state->enemy_count, text);
    VDP_drawText(text, 29, 22);

    write_u16_2(state->target_count, text);
    VDP_drawText(text, 8, 23);

    write_u16_2(state->target_health, text);
    VDP_drawText(text, 11, 23);

    write_u16_2(state->pickups.key, text);
    VDP_drawText(text, 19, 23);

    write_u16_2(state->pickups.bonus, text);
    VDP_drawText(text, 26, 23);

    VDP_drawText(portrait_line(state->portrait_state, 0), 16, 23);
    VDP_drawText(portrait_line(state->portrait_state, 1), 16, 24);
    VDP_drawText(portrait_line(state->portrait_state, 2), 16, 25);

    VDP_drawText(shot_status_text(state->shot_status), 10, 24);
    VDP_drawText(action_status_text(state->action_status), 26, 24);
    VDP_drawText(last_pickup_text(state->last_pickup), 26, 25);
    VDP_drawText(state->level_cleared ? "CLEAR" : "SEEK ", 10, 25);

    write_hex_u32(state->frame, text);
    VDP_drawText(text, 11, 26);
}
