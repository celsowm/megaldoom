#include "renderer_internal.h"

static const char HEX_DIGITS[] = "0123456789ABCDEF";

static void write_hex_u32(u32 value, char *out) {
    for (u16 i = 0; i < 8; i++) {
        const u16 shift = (u16)((7 - i) * 4);
        out[i] = HEX_DIGITS[(value >> shift) & 0x0F];
    }
    out[8] = '\0';
}

void renderer_draw_static_screen(void) {
    VDP_drawText(RENDERER_VERSION_TEXT, 7, 2);
    VDP_drawText("LOW HP WARNING", 14, 4);
    VDP_drawText("START USE  B FIRE", 8, 22);
    VDP_drawText("PH 00", 17, 16);
    VDP_drawText("ENMY 00", 14, 15);
    VDP_drawText("HP 00", 23, 15);
    VDP_drawText("SHOT ----", 14, 19);
    VDP_drawText("COOL 00", 15, 18);
    VDP_drawText("TGT 00 HP 00", 12, 17);
    VDP_drawText("USE ----", 14, 21);
    VDP_drawText("GOAL SEEK ", 14, 20);
    VDP_drawText("BONUS 00 KEY 00", 11, 24);
    VDP_drawText("LAST NONE", 13, 23);
    VDP_drawText("FRAME", 13, 25);
}

void renderer_draw_frame_counter(u32 frame) {
    char frame_text[9];

    write_hex_u32(frame, frame_text);
    VDP_drawText(frame_text, 19, 25);
}

void renderer_draw_pickup_counter(BillboardPickupCounts counts) {
    char bonus_text[3];
    char key_text[3];

    if (counts.bonus > 99) {
        counts.bonus = 99;
    }
    if (counts.key > 99) {
        counts.key = 99;
    }

    bonus_text[0] = (char)('0' + (counts.bonus / 10));
    bonus_text[1] = (char)('0' + (counts.bonus % 10));
    bonus_text[2] = '\0';
    key_text[0] = (char)('0' + (counts.key / 10));
    key_text[1] = (char)('0' + (counts.key % 10));
    key_text[2] = '\0';

    VDP_drawText(bonus_text, 17, 24);
    VDP_drawText(key_text, 24, 24);
}

void renderer_draw_last_pickup(BillboardPickupKind kind) {
    const char *text = "NONE";

    if (kind == BILLBOARD_PICKUP_BONUS) {
        text = "BONUS";
    } else if (kind == BILLBOARD_PICKUP_KEY) {
        text = "KEY ";
    }

    VDP_drawText(text, 18, 23);
}

void renderer_draw_action_status(DoorActionResult action) {
    const char *text = "----";

    if (action == DOOR_ACTION_TOGGLED) {
        text = "DOOR";
    } else if (action == DOOR_ACTION_LOCKED) {
        text = "LOCK";
    } else if (action == DOOR_ACTION_UNLOCKED) {
        text = "KEY!";
    } else if (action == DOOR_ACTION_EXIT) {
        text = "EXIT";
    } else if (action == DOOR_ACTION_EXIT_LOCKED) {
        text = "HUNT";
    }

    VDP_drawText(text, 18, 21);
}

void renderer_draw_goal_status(bool level_cleared) {
    VDP_drawText(level_cleared ? "CLEAR" : "SEEK ", 19, 20);
}

void renderer_draw_shot_status(BillboardShotResult shot) {
    const char *text = "MISS";

    if (shot == BILLBOARD_SHOT_DAMAGE) {
        text = "DMG ";
    } else if (shot == BILLBOARD_SHOT_KILL) {
        text = "KILL";
    }

    VDP_drawText(text, 19, 19);
}

void renderer_draw_shot_cooldown(u16 frames_left) {
    char text[3];

    if (frames_left > 99) {
        frames_left = 99;
    }

    text[0] = (char)('0' + (frames_left / 10));
    text[1] = (char)('0' + (frames_left % 10));
    text[2] = '\0';
    VDP_drawText(text, 20, 18);
}

void renderer_draw_enemy_count(u16 count) {
    char text[3];

    if (count > 99) {
        count = 99;
    }

    text[0] = (char)('0' + (count / 10));
    text[1] = (char)('0' + (count % 10));
    text[2] = '\0';
    VDP_drawText(text, 19, 15);
}

void renderer_draw_player_health(u16 hp) {
    char text[3];

    if (hp > 99) {
        hp = 99;
    }

    text[0] = (char)('0' + (hp / 10));
    text[1] = (char)('0' + (hp % 10));
    text[2] = '\0';
    VDP_drawText(text, 26, 15);
}

void renderer_draw_phase(u16 phase_index) {
    char text[3];
    const u16 phase = (u16)((phase_index % 99) + 1);

    text[0] = (char)('0' + (phase / 10));
    text[1] = (char)('0' + (phase % 10));
    text[2] = '\0';
    VDP_drawText(text, 20, 16);
}

void renderer_draw_target_count(u16 count) {
    char text[3];

    if (count > 99) {
        count = 99;
    }

    text[0] = (char)('0' + (count / 10));
    text[1] = (char)('0' + (count % 10));
    text[2] = '\0';
    VDP_drawText(text, 16, 17);
}

void renderer_draw_target_health(u16 hp) {
    char text[3];

    if (hp > 99) {
        hp = 99;
    }

    text[0] = (char)('0' + (hp / 10));
    text[1] = (char)('0' + (hp % 10));
    text[2] = '\0';
    VDP_drawText(text, 22, 17);
}
