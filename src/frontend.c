#include "frontend.h"
#include "debug_checkpoint.h"
#include "game_audio.h"
#include "resources.h"

#define MENU_ACCEPT (BUTTON_START | BUTTON_A | BUTTON_C)
#define MENU_BACK BUTTON_B
#define MENU_INPUT (BUTTON_UP | BUTTON_DOWN | MENU_ACCEPT | MENU_BACK)
#define PANEL_X 8
#define PANEL_Y 7
#define MAIN_CURSOR_X 9
/* Menu labels are 24px apart, so the 24px skull stays beside every row. */
#define MAIN_CURSOR_Y 12
#define MAIN_CURSOR_STEP 3
#define BOOT_CARD_FRAMES 180
#define BOOT_SEGA_CARD_FRAMES 564
#define BOOT_FADE_FRAMES 12
#define BOOT_CARD_VISIBLE_FRAMES (BOOT_CARD_FRAMES - (BOOT_FADE_FRAMES * 2))
#define BOOT_SEGA_VISIBLE_FRAMES (BOOT_SEGA_CARD_FRAMES - (BOOT_FADE_FRAMES * 2))
#define BOOT_SEGA_LOGO_Y 128
#define BOOT_CACODEMON_X 136
#define BOOT_CACODEMON_Y 60
#define BOOT_CACODEMON_ENTRY_X 320
#define BOOT_CACODEMON_ENTRY_Y 8
#define BOOT_CACODEMON_ENTRY_END 90
#define BOOT_CACODEMON_ATTACK_START 180
#define BOOT_CACODEMON_ATTACK_END 228
#define BOOT_PROJECTILE_START 228
#define BOOT_PROJECTILE_IMPACT 270
#define BOOT_PROJECTILE_EXPLOSION_END 318
#define BOOT_SEGA_LETTERS_FLIGHT_END 438
#define BOOT_CACODEMON_LAUGH_END 540
#define BOOT_SEGA_SPRITE_VRAM_TILES 192
#define ENDING_PROMPT_BLINK_MASK 0x3F
#define ENDING_PROMPT_ON_FRAMES 48
// Generated at y=176 and centred at x=114.  This is the exact 12-tile span
// covering PRESS START in frontend_ending_thanks, so blinking never disturbs
// the closing copy above it.
#define ENDING_PROMPT_X 14
#define ENDING_PROMPT_Y 22
#define ENDING_PROMPT_W 12
#define INTERMISSION_INPUT (BUTTON_A | BUTTON_B | BUTTON_C | BUTTON_START)
#define INTERMISSION_MAP_FRAMES 180

/* A return from gameplay is a return to the title, not a fresh console boot. */
static bool s_boot_sequence_played = FALSE;

static void clear_plane_cpu(VDPPlane plane) {
    /* CPU writes avoid competing with XGM2 for DMA/Z80 arbitration. */
    VDP_fillTileMapRect(plane, 0, 0, 0, 64, 32);
}

static void frontend_video_init(void) {
    VDP_setScreenWidth320();
    VDP_setScreenHeight224();
    VDP_setPlaneSize(64, 32, TRUE);
    VDP_setScrollingMode(HSCROLL_PLANE, VSCROLL_PLANE);
    VDP_setHorizontalScroll(BG_A, 0);
    VDP_setHorizontalScroll(BG_B, 0);
    VDP_setVerticalScroll(BG_A, 0);
    VDP_setVerticalScroll(BG_B, 0);
    // Gameplay parks the status bar on the window plane (BG_A is scrolled for
    // weapon bob); the menus/cards own the whole of BG_A, so drop the window.
    VDP_setWindowOff();
    VDP_setHInterrupt(FALSE);
    VDP_setHilightShadow(FALSE);
    clear_plane_cpu(BG_A);
    clear_plane_cpu(BG_B);
    VDP_setBackgroundColor(0);
}

static void wait_for_release(u16 mask) {
    do {
        JOY_update();
        VDP_waitVSync();
    } while ((JOY_readJoypad(JOY_1) & mask) != 0);
}

static u16 read_pressed(u16 *previous) {
    /* SGDK only refreshes the controller state when JOY_update runs. Without
     * this, the title screen keeps reading the state captured before its loop
     * and Start (BlastEm Enter) is never observed as a new press. */
    JOY_update();
    const u16 current = JOY_readJoypad(JOY_1);
    const u16 pressed = (u16)(current & ~(*previous));
    *previous = current;
    return pressed;
}

static void draw_panel(const Image *image, u16 tile_base) {
    game_audio_suspend_for_video();
    VDP_waitVSync();
    VDP_setEnable(FALSE);
    VDP_drawImageEx(BG_A, image,
                    TILE_ATTR_FULL(PAL0, TRUE, FALSE, FALSE, tile_base),
                    0, 0, TRUE, TRUE);
    VDP_setEnable(TRUE);
    game_audio_resume_after_video();
}

/* The boot cards deliberately avoid draw_panel(): suspending XGM2 to redraw a
 * card would restart the intro track between cards, while these cards are
 * intended to play as one continuous opening sequence. */
static void draw_boot_card(const Image *image) {
    clear_plane_cpu(BG_A);
    clear_plane_cpu(BG_B);
    VDP_waitVSync();
    VDP_drawImageEx(BG_B, image,
                    TILE_ATTR_FULL(PAL0, FALSE, FALSE, FALSE, TILE_USER_INDEX),
                    0, 0, TRUE, TRUE);
}

static const s8 s_boot_cacodemon_bob[16] = {
    0, -1, -2, -2, -2, -1, 0, 1,
    2, 2, 2, 1, 0, -1, -2, -1,
};

/* Entries 12..15 are intentionally reserved by make_boot_sega(). Cycling
 * only those shades gives the blue wordmark a metallic sheen without ever
 * touching PAL1, which belongs to the Cacodemon. */
static const u16 s_sega_shimmer[4][4] = {
    { RGB24_TO_VDPCOLOR(0x00287D), RGB24_TO_VDPCOLOR(0x004AB4),
      RGB24_TO_VDPCOLOR(0x58A8F0), RGB24_TO_VDPCOLOR(0xFFFFFF) },
    { RGB24_TO_VDPCOLOR(0x003995), RGB24_TO_VDPCOLOR(0x005CC8),
      RGB24_TO_VDPCOLOR(0x80C4F8), RGB24_TO_VDPCOLOR(0xFFFFFF) },
    { RGB24_TO_VDPCOLOR(0x00287D), RGB24_TO_VDPCOLOR(0x005CC8),
      RGB24_TO_VDPCOLOR(0xFFFFFF), RGB24_TO_VDPCOLOR(0x80C4F8) },
    { RGB24_TO_VDPCOLOR(0x001248), RGB24_TO_VDPCOLOR(0x003995),
      RGB24_TO_VDPCOLOR(0x005CC8), RGB24_TO_VDPCOLOR(0x58A8F0) },
};

static const u16 s_sega_attack_shimmer[4] = {
    RGB24_TO_VDPCOLOR(0x004AB4), RGB24_TO_VDPCOLOR(0x0078D8),
    RGB24_TO_VDPCOLOR(0x80C4F8), RGB24_TO_VDPCOLOR(0xFFFFFF),
};

/* Palette indices 2..8 are the solid blue/cyan face. Reinstalling them after
 * each white hit-flash makes the explosion read on the letters themselves,
 * rather than only in the projectile sprite. */
static const u16 s_sega_face_colours[7] = {
    RGB24_TO_VDPCOLOR(0x001248), RGB24_TO_VDPCOLOR(0x00287D),
    RGB24_TO_VDPCOLOR(0x0041AA), RGB24_TO_VDPCOLOR(0x0055CD),
    RGB24_TO_VDPCOLOR(0x1470E1), RGB24_TO_VDPCOLOR(0x4898EE),
    RGB24_TO_VDPCOLOR(0x80C4F8),
};

static const u16 s_sega_impact_face[7] = {
    RGB24_TO_VDPCOLOR(0xFFFFFF), RGB24_TO_VDPCOLOR(0xFFFFFF),
    RGB24_TO_VDPCOLOR(0xB6FFFF), RGB24_TO_VDPCOLOR(0xFFFFFF),
    RGB24_TO_VDPCOLOR(0xB6FFFF), RGB24_TO_VDPCOLOR(0xFFFFFF),
    RGB24_TO_VDPCOLOR(0xB6FFFF),
};

static const SpriteDefinition *const s_sega_letter_defs[4] = {
    &frontend_sega_s, &frontend_sega_e, &frontend_sega_g, &frontend_sega_a,
};

/* The classic startup mark is about 70px wide. Sprite rectangles overlap,
 * but each independently rendered glyph has a transparent gutter. */
static const s16 s_sega_letter_start_x[4] = { 109, 130, 152, 174 };

static void animate_sega_shimmer(u16 frame, bool attack, bool impact) {
    const u16 *colours;
    if (impact && ((frame >> 2) & 1) == 0) {
        PAL_setColors(2, s_sega_impact_face, 7, CPU);
        PAL_setColors(12, s_sega_attack_shimmer, 4, CPU);
        return;
    }
    PAL_setColors(2, s_sega_face_colours, 7, CPU);
    colours = attack ? s_sega_attack_shimmer : s_sega_shimmer[(frame >> 3) & 3];
    PAL_setColors(12, colours, 4, CPU);
}

static void fade_sega_card_in(const Image *image) {
    /* PAL0 is the logo, PAL1 the Cacodemon and PAL2 the fireball. */
    u16 palette[48];
    for (u16 index = 0; index < 16; index++) {
        palette[index] = image->palette->data[index];
        palette[16 + index] = frontend_cacodemon.palette->data[index];
        palette[32 + index] = frontend_cacodemon_projectile.palette->data[index];
    }
    PAL_fadeIn(0, 47, palette, BOOT_FADE_FRAMES, FALSE);
}

static u16 sega_cacodemon_frame(u16 frame) {
    if (frame >= BOOT_CACODEMON_ATTACK_START && frame < BOOT_CACODEMON_ATTACK_END) {
        return (u16)(2 + (((frame - BOOT_CACODEMON_ATTACK_START) >> 3) & 3));
    }
    /* The open/closed attack faces, paired with a wider bob, read as a cruel
     * laugh once the logo has vanished. */
    if (frame >= BOOT_SEGA_LETTERS_FLIGHT_END && frame < BOOT_CACODEMON_LAUGH_END) {
        return (u16)(2 + ((frame >> 2) & 3));
    }
    return (frame >> 3) & 1;
}

static void set_sega_letter_visibility(Sprite *const letters[4], bool visible) {
    for (u16 index = 0; index < 4; index++) {
        if (letters[index] != NULL) {
            SPR_setVisibility(letters[index], visible ? VISIBLE : HIDDEN);
        }
    }
}

static void animate_sega_letters(Sprite *const letters[4], u16 frame) {
    static const s16 s_velocity_x[4] = { -3, -2, 2, 3 };
    static const s16 s_velocity_y[4] = { 0, -3, 2, 0 };

    if (frame < BOOT_PROJECTILE_EXPLOSION_END) {
        for (u16 index = 0; index < 4; index++) {
            if (letters[index] != NULL) {
                SPR_setPosition(letters[index], s_sega_letter_start_x[index],
                                BOOT_SEGA_LOGO_Y);
                SPR_setVisibility(letters[index], VISIBLE);
            }
        }
        return;
    }
    if (frame >= BOOT_SEGA_LETTERS_FLIGHT_END) {
        set_sega_letter_visibility(letters, FALSE);
        return;
    }

    const s16 travel = (s16)(frame - BOOT_PROJECTILE_EXPLOSION_END);
    const s16 gravity = (s16)(((s32)travel * travel) / 160);
    for (u16 index = 0; index < 4; index++) {
        const s16 x = (s16)(s_sega_letter_start_x[index] +
                            s_velocity_x[index] * travel);
        const s16 y = (s16)(BOOT_SEGA_LOGO_Y + s_velocity_y[index] * travel + gravity);
        if (letters[index] == NULL) continue;
        SPR_setPosition(letters[index], x, y);
        /* The Genesis X coordinate only represents a bounded signed range;
         * hide a completed flight instead of letting it wrap back on screen. */
        SPR_setVisibility(letters[index],
                          x > -32 && x < 320 && y > -48 && y < 224 ? VISIBLE : HIDDEN);
    }
}

static void run_boot_card(const Image *image, bool show_cacodemon) {
    Sprite *cacodemon = NULL;
    Sprite *projectile = NULL;
    Sprite *sega_letters[4] = { NULL, NULL, NULL, NULL };
    u16 previous;
    u16 cacodemon_frame = 0;
    const u16 visible_frames = show_cacodemon ? BOOT_SEGA_VISIBLE_FRAMES
                                               : BOOT_CARD_VISIBLE_FRAMES;

    draw_boot_card(image);
    if (show_cacodemon) {
        /* Four wordmark pieces, the smaller Cacodemon and its 56x48 fireball
         * all fit below the Genesis per-line sprite limit at their overlaps. */
        /* 42 Cacodemon tiles + 42 projectile tiles + four 24-tile letters
         * require 180 cached tiles. Keep a small margin so every semantic
         * letter is allocated before the first visible frame. */
        SPR_initEx(BOOT_SEGA_SPRITE_VRAM_TILES);
        cacodemon = SPR_addSprite(&frontend_cacodemon, BOOT_CACODEMON_ENTRY_X,
                                  BOOT_CACODEMON_ENTRY_Y,
                                  TILE_ATTR(PAL1, TRUE, FALSE, FALSE));
        projectile = SPR_addSprite(&frontend_cacodemon_projectile, 132, 72,
                                   TILE_ATTR(PAL2, TRUE, FALSE, FALSE));
        for (u16 index = 0; index < 4; index++) {
            sega_letters[index] = SPR_addSprite(s_sega_letter_defs[index],
                                                 s_sega_letter_start_x[index],
                                                 BOOT_SEGA_LOGO_Y,
                                                 TILE_ATTR(PAL0, TRUE, FALSE, FALSE));
            /* Each letter is now its own one-frame SpriteDefinition.  Using
             * the letter index here selected a non-existent animation frame
             * for E/G/A, making the logo look sliced before impact. */
            if (sega_letters[index] != NULL) SPR_setFrame(sega_letters[index], 0);
        }
        if (projectile != NULL) SPR_setVisibility(projectile, HIDDEN);
        SPR_update();
    }

    if (show_cacodemon) {
        fade_sega_card_in(image);
    } else {
        PAL_fadeIn(0, 63, image->palette->data, BOOT_FADE_FRAMES, FALSE);
    }
    wait_for_release(BUTTON_START);
    previous = JOY_readJoypad(JOY_1);
    for (u16 frame = 0; frame < visible_frames; frame++) {
        const u16 pressed = read_pressed(&previous);
        if ((pressed & BUTTON_START) != 0) break;
        if (show_cacodemon) {
            u16 caco_x = BOOT_CACODEMON_X;
            u16 caco_y = BOOT_CACODEMON_Y;
            const u16 next_frame = sega_cacodemon_frame(frame);
            const bool attack = frame >= BOOT_CACODEMON_ATTACK_START &&
                                frame < BOOT_PROJECTILE_IMPACT;
            const bool impact = frame >= BOOT_PROJECTILE_IMPACT &&
                                frame < BOOT_PROJECTILE_EXPLOSION_END;
            if (frame < BOOT_CACODEMON_ENTRY_END) {
                caco_x = (u16)(BOOT_CACODEMON_ENTRY_X -
                               ((u32)frame * (BOOT_CACODEMON_ENTRY_X - BOOT_CACODEMON_X)) /
                                (BOOT_CACODEMON_ENTRY_END - 1));
                caco_y = (u16)(BOOT_CACODEMON_ENTRY_Y +
                               ((u32)frame * (BOOT_CACODEMON_Y - BOOT_CACODEMON_ENTRY_Y)) /
                                (BOOT_CACODEMON_ENTRY_END - 1));
            }
            animate_sega_shimmer(frame, attack, impact);
            animate_sega_letters(sega_letters, frame);
            if (cacodemon != NULL) {
                s16 bob = s_boot_cacodemon_bob[(frame >> 1) & 15];
                if (frame >= BOOT_SEGA_LETTERS_FLIGHT_END) bob *= 2;
                if (next_frame != cacodemon_frame) {
                    SPR_setFrame(cacodemon, next_frame);
                    cacodemon_frame = next_frame;
                }
                SPR_setPosition(cacodemon, (s16)caco_x, (s16)(caco_y + bob));
            }
            if (projectile != NULL) {
                if (frame >= BOOT_PROJECTILE_START && frame < BOOT_PROJECTILE_IMPACT) {
                    const u16 travel = (u16)(frame - BOOT_PROJECTILE_START);
                    const s16 x = (s16)(132 + (((travel >> 1) & 3) - 1) * 2);
                    const s16 y = (s16)(80 + ((u32)travel * 43u) /
                                         (BOOT_PROJECTILE_IMPACT - BOOT_PROJECTILE_START - 1));
                    SPR_setFrame(projectile, (s16)((travel >> 3) & 1));
                    SPR_setPosition(projectile, x, y);
                    SPR_setVisibility(projectile, VISIBLE);
                } else if (impact) {
                    SPR_setFrame(projectile, (s16)(2 + ((frame - BOOT_PROJECTILE_IMPACT) >> 4)));
                    SPR_setPosition(projectile, 132, 123);
                    SPR_setVisibility(projectile, VISIBLE);
                } else {
                    SPR_setVisibility(projectile, HIDDEN);
                }
                if (frame == BOOT_PROJECTILE_START) {
                    game_audio_play_sfx(sfx_cacodemon_fire,
                                        sizeof(sfx_cacodemon_fire), SOUND_PCM_CH2);
                } else if (frame == BOOT_PROJECTILE_IMPACT) {
                    game_audio_play_sfx(sfx_cacodemon_impact,
                                        sizeof(sfx_cacodemon_impact), SOUND_PCM_CH3);
                } else if (frame == BOOT_SEGA_LETTERS_FLIGHT_END + 18 ||
                           frame == BOOT_SEGA_LETTERS_FLIGHT_END + 72) {
                    game_audio_play_sfx(sfx_cacodemon_laugh,
                                        sizeof(sfx_cacodemon_laugh), SOUND_PCM_CH2);
                }
            }
            SPR_update();
        }
        /* SPR_update and the palette sheen are queued work; VDP_waitVSync
         * merely waits for blanking, while this commits the sprite table and
         * palette on every boot-card frame. */
        SYS_doVBlankProcess();
    }

    if (show_cacodemon) {
        PAL_fadeOut(0, 47, BOOT_FADE_FRAMES, FALSE);
        if (cacodemon != NULL) SPR_releaseSprite(cacodemon);
        if (projectile != NULL) SPR_releaseSprite(projectile);
        for (u16 index = 0; index < 4; index++) {
            if (sega_letters[index] != NULL) SPR_releaseSprite(sega_letters[index]);
        }
        SPR_end();
        // SPR_end queues the cleared SAT, but the title/menu loops only wait
        // for VBlank and do not run SGDK's queued-work pump. Commit it now or
        // the boot Cacodemon/letter fragments remain visible over the menu.
        SYS_doVBlankProcess();
    } else {
        PAL_fadeOut(0, 63, BOOT_FADE_FRAMES, FALSE);
    }
    clear_plane_cpu(BG_A);
    clear_plane_cpu(BG_B);
    /* A single held Start only skips the current card. */
    wait_for_release(BUTTON_START);
}

static void run_boot_sequence(void) {
    frontend_video_init();
    game_audio_play_music(intro_music);
    run_boot_card(&frontend_boot_disclaimer, FALSE);
    run_boot_card(&frontend_boot_sega, TRUE);
    run_boot_card(&frontend_boot_social, FALSE);
}

static void set_ending_prompt(bool visible) {
    if (visible) {
        VDP_setTileMapEx(BG_B, frontend_ending_thanks.tilemap,
                         TILE_ATTR_FULL(PAL0, FALSE, FALSE, FALSE, TILE_USER_INDEX),
                         ENDING_PROMPT_X, ENDING_PROMPT_Y,
                         ENDING_PROMPT_X, ENDING_PROMPT_Y,
                         ENDING_PROMPT_W, 1, CPU);
    } else {
        VDP_clearTileMapRect(BG_B, ENDING_PROMPT_X, ENDING_PROMPT_Y,
                             ENDING_PROMPT_W, 1);
    }
}

static void run_demo_thanks(void) {
    u16 previous;
    u16 ticks = 0;

    draw_boot_card(&frontend_ending_thanks);
    PAL_fadeIn(0, 63, frontend_ending_thanks.palette->data, BOOT_FADE_FRAMES, FALSE);
    wait_for_release(BUTTON_START);
    previous = JOY_readJoypad(JOY_1);
    while (TRUE) {
        const u16 pressed = read_pressed(&previous);
        set_ending_prompt((bool)((ticks & ENDING_PROMPT_BLINK_MASK) < ENDING_PROMPT_ON_FRAMES));
        if ((pressed & BUTTON_START) != 0) break;
        ticks++;
        VDP_waitVSync();
    }
    PAL_fadeOut(0, 63, BOOT_FADE_FRAMES, FALSE);
    clear_plane_cpu(BG_A);
    clear_plane_cpu(BG_B);
    wait_for_release(BUTTON_START);
}

typedef struct {
    u16 stats;
    u16 digits;
} IntermissionStatsTiles;

typedef struct {
    u16 entering;
    u16 splat;
    u16 pointer0;
    u16 pointer1;
    u16 end;
} IntermissionMapTiles;

static IntermissionStatsTiles load_intermission_stats_tiles(u16 completed_level) {
    IntermissionStatsTiles tiles;
    u16 next = (u16)(TILE_USER_INDEX + frontend_ending_mars.tileset->numTile);
    const Image *stats = completed_level == 0
        ? &frontend_intermission_stats : &frontend_intermission_stats_e1m2;
    tiles.stats = next;
    VDP_loadTileSet(stats->tileset, next, DMA);
    next = (u16)(next + stats->tileset->numTile);
    tiles.digits = next;
    VDP_loadTileSet(frontend_intermission_digits.tileset, next, DMA);
    return tiles;
}

static IntermissionMapTiles load_intermission_map_tiles(u16 completed_level) {
    IntermissionMapTiles tiles;
    u16 next = (u16)(TILE_USER_INDEX + frontend_ending_mars.tileset->numTile);
    tiles.entering = next;
    if (completed_level == 0) {
        VDP_loadTileSet(frontend_intermission_entering_e1m2.tileset, next, DMA);
        next = (u16)(next + frontend_intermission_entering_e1m2.tileset->numTile);
    }
    tiles.splat = next;
    VDP_loadTileSet(frontend_intermission_splat.tileset, next, DMA);
    next = (u16)(next + frontend_intermission_splat.tileset->numTile);
    tiles.pointer0 = next;
    VDP_loadTileSet(frontend_intermission_pointer0.tileset, next, DMA);
    next = (u16)(next + frontend_intermission_pointer0.tileset->numTile);
    tiles.pointer1 = next;
    VDP_loadTileSet(frontend_intermission_pointer1.tileset, next, DMA);
    tiles.end = (u16)(next + frontend_intermission_pointer1.tileset->numTile);
    return tiles;
}

static void draw_intermission_image(const Image *image, u16 tile_base) {
    VDP_setTileMapEx(BG_A, image->tilemap,
                     TILE_ATTR_FULL(PAL0, TRUE, FALSE, FALSE, tile_base),
                     0, 0, 0, 0, image->tilemap->w, image->tilemap->h, CPU);
}

static void draw_intermission_glyph(u16 tile_base, u16 glyph, u16 x, u16 y) {
    VDP_setTileMapEx(BG_A, frontend_intermission_digits.tilemap,
                     TILE_ATTR_FULL(PAL0, TRUE, FALSE, FALSE, tile_base),
                     x, y, (u16)(glyph * 2), 0, 2, 2, CPU);
}

static void draw_intermission_number(u16 tile_base, u16 value, bool percent,
                                     u16 right, u16 y) {
    u8 digits[5];
    u16 count = 0;
    u16 cursor;
    if (value == 0) digits[count++] = 0;
    while (value > 0 && count < 5) {
        digits[count++] = (u8)(value % 10);
        value = (u16)(value / 10);
    }
    cursor = (u16)(right - (count + (percent ? 1 : 0)) * 2);
    for (s16 i = (s16)count - 1; i >= 0; i--) {
        draw_intermission_glyph(tile_base, digits[i], cursor, y);
        cursor = (u16)(cursor + 2);
    }
    if (percent) draw_intermission_glyph(tile_base, 10, cursor, y);
}

static void draw_intermission_time(u16 tile_base, u16 seconds, u16 right, u16 y) {
    const u16 minutes = (u16)(seconds / 60);
    const u16 remainder = (u16)(seconds % 60);
    u16 cursor = (u16)(right - 10);
    draw_intermission_glyph(tile_base, (u16)((minutes / 10) % 10), cursor, y);
    cursor += 2;
    draw_intermission_glyph(tile_base, (u16)(minutes % 10), cursor, y);
    cursor += 2;
    draw_intermission_glyph(tile_base, 11, cursor, y);
    cursor += 2;
    draw_intermission_glyph(tile_base, (u16)(remainder / 10), cursor, y);
    cursor += 2;
    draw_intermission_glyph(tile_base, (u16)(remainder % 10), cursor, y);
}

static u16 intermission_percent(u16 value, u16 total) {
    if (total == 0) return 0;
    return (u16)(((u32)value * 100u) / total);
}

static void draw_intermission_values(u16 tile_base,
                                     u16 kills, u16 items, u16 secrets,
                                     u16 time_seconds, u16 par_seconds) {
    VDP_clearTileMapRect(BG_A, 24, 6, 15, 11);
    VDP_clearTileMapRect(BG_A, 8, 21, 12, 2);
    VDP_clearTileMapRect(BG_A, 30, 21, 10, 2);
    draw_intermission_number(tile_base, kills, TRUE, 38, 6);
    draw_intermission_number(tile_base, items, TRUE, 38, 10);
    draw_intermission_number(tile_base, secrets, TRUE, 38, 14);
    draw_intermission_time(tile_base, time_seconds, 20, 21);
    draw_intermission_time(tile_base, par_seconds, 40, 21);
}

static void run_intermission_stats(const FrontendIntermissionStats *stats,
                                   const IntermissionStatsTiles *tiles) {
    const u16 target_kills = intermission_percent(stats->kills, stats->kill_total);
    const u16 target_items = intermission_percent(stats->items, stats->item_total);
    const u16 target_secrets = intermission_percent(stats->secrets, stats->secret_total);
    const u16 target_time = (u16)(stats->time_vblanks / 60u);
    u16 kills = 0, items = 0, secrets = 0, time_seconds = 0, par_seconds = 0;
    u16 stage = 0;
    u16 previous;

    draw_intermission_image(stats->completed_level == 0
        ? &frontend_intermission_stats : &frontend_intermission_stats_e1m2,
        tiles->stats);
    draw_intermission_values(tiles->digits, 0, 0, 0, 0, 0);
    wait_for_release(INTERMISSION_INPUT);
    previous = JOY_readJoypad(JOY_1);
    while (TRUE) {
        const u16 pressed = read_pressed(&previous);
        const bool finished = stage >= 4;
        if ((pressed & INTERMISSION_INPUT) != 0) {
            if (!finished) {
                kills = target_kills;
                items = target_items;
                secrets = target_secrets;
                time_seconds = target_time;
                par_seconds = stats->par_seconds;
                stage = 4;
                game_audio_play_sfx(sfx_door, sizeof(sfx_door), SOUND_PCM_CH2);
            } else {
                break;
            }
        } else if (stage == 0) {
            kills = (kills + 2 >= target_kills) ? target_kills : (u16)(kills + 2);
            if (kills == target_kills) stage++;
        } else if (stage == 1) {
            items = (items + 2 >= target_items) ? target_items : (u16)(items + 2);
            if (items == target_items) stage++;
        } else if (stage == 2) {
            secrets = (secrets + 2 >= target_secrets) ? target_secrets : (u16)(secrets + 2);
            if (secrets == target_secrets) stage++;
        } else if (stage == 3) {
            time_seconds = (time_seconds + 3 >= target_time) ? target_time :
                           (u16)(time_seconds + 3);
            par_seconds = (par_seconds + 3 >= stats->par_seconds) ? stats->par_seconds :
                          (u16)(par_seconds + 3);
            if (time_seconds == target_time && par_seconds == stats->par_seconds) stage++;
        }
        draw_intermission_values(tiles->digits, kills, items, secrets,
                                 time_seconds, par_seconds);
        if ((stage < 4) && ((vtimer & 3) == 0)) {
            game_audio_play_sfx(sfx_pickup, sizeof(sfx_pickup), SOUND_PCM_CH2);
        }
        SYS_doVBlankProcess();
    }
    wait_for_release(INTERMISSION_INPUT);
}

static void draw_marker(const Image *image, u16 tile_base, u16 x, u16 y) {
    VDP_setTileMapEx(BG_A, image->tilemap,
                     TILE_ATTR_FULL(PAL0, TRUE, FALSE, FALSE, tile_base),
                     x, y, 0, 0, image->tilemap->w, image->tilemap->h, CPU);
}

static void run_intermission_map(u16 completed_level,
                                 const IntermissionMapTiles *tiles) {
    u16 previous;
    clear_plane_cpu(BG_A);
    // WIMAP0 node coordinates, converted to the tile-aligned 224-line frame.
    if (completed_level == 0) {
        draw_intermission_image(&frontend_intermission_entering_e1m2,
                                tiles->entering);
    }
    draw_marker(&frontend_intermission_splat, tiles->splat, 21, 20);
    if (completed_level > 0) {
        draw_marker(&frontend_intermission_splat, tiles->splat, 16, 18);
    }
    wait_for_release(INTERMISSION_INPUT);
    previous = JOY_readJoypad(JOY_1);
    for (u16 frame = 0; frame < INTERMISSION_MAP_FRAMES; frame++) {
        if ((read_pressed(&previous) & INTERMISSION_INPUT) != 0) break;
        if (completed_level == 0) {
            VDP_clearTileMapRect(BG_A, 10, 18, 8, 2);
            if ((frame & 31) < 24) {
                const bool alternate = (bool)((frame & 8) != 0);
                draw_marker(alternate ? &frontend_intermission_pointer1
                                      : &frontend_intermission_pointer0,
                            alternate ? tiles->pointer1 : tiles->pointer0,
                            10, 18);
            }
        }
        SYS_doVBlankProcess();
    }
    wait_for_release(INTERMISSION_INPUT);
}

FrontendIntermissionAction frontend_run_intermission(
    const FrontendIntermissionStats *stats) {
    PAL_fadeOut(0, 63, BOOT_FADE_FRAMES, FALSE);
    frontend_video_init();
    draw_boot_card(&frontend_ending_mars);
    const IntermissionStatsTiles stats_tiles =
        load_intermission_stats_tiles(stats->completed_level);
    game_audio_play_music(intermission_music);
    PAL_fadeIn(0, 63, frontend_ending_mars.palette->data, BOOT_FADE_FRAMES, FALSE);
    run_intermission_stats(stats, &stats_tiles);
    const IntermissionMapTiles map_tiles =
        load_intermission_map_tiles(stats->completed_level);
    run_intermission_map(stats->completed_level, &map_tiles);
    PAL_fadeOut(0, 63, BOOT_FADE_FRAMES, FALSE);
    if (stats->completed_level == 0) {
        game_audio_stop_music();
        clear_plane_cpu(BG_A);
        clear_plane_cpu(BG_B);
        return FRONTEND_INTERMISSION_CONTINUE;
    }
    run_demo_thanks();
    game_audio_stop_music();
    return FRONTEND_INTERMISSION_TITLE;
}

static void draw_main_cursor(u16 tile_base, u16 selected, bool alternate) {
    const Image *image = alternate ? &frontend_skull2 : &frontend_skull1;
    if (alternate) tile_base = (u16)(tile_base + frontend_skull1.tileset->numTile);
    VDP_setTileMapEx(BG_A, image->tilemap,
                     TILE_ATTR_FULL(PAL0, TRUE, FALSE, FALSE, tile_base),
                     MAIN_CURSOR_X, (u16)(MAIN_CURSOR_Y + selected * MAIN_CURSOR_STEP),
                     0, 0, image->tilemap->w, image->tilemap->h, CPU);
}

static void load_main_cursor_tiles(u16 tile_base) {
    VDP_loadTileSet(frontend_skull1.tileset, tile_base, DMA);
    VDP_loadTileSet(frontend_skull2.tileset,
                    (u16)(tile_base + frontend_skull1.tileset->numTile), DMA);
}

static const Image *options_panel(u16 selected) {
    const bool music = game_audio_music_enabled();
    const bool sfx = game_audio_sfx_enabled();
    if (!music && !sfx) {
        if (selected == 0) return &frontend_options_0_0_0;
        if (selected == 1) return &frontend_options_0_0_1;
        return &frontend_options_0_0_2;
    }
    if (!music && sfx) {
        if (selected == 0) return &frontend_options_0_1_0;
        if (selected == 1) return &frontend_options_0_1_1;
        return &frontend_options_0_1_2;
    }
    if (music && !sfx) {
        if (selected == 0) return &frontend_options_1_0_0;
        if (selected == 1) return &frontend_options_1_0_1;
        return &frontend_options_1_0_2;
    }
    if (selected == 0) return &frontend_options_1_1_0;
    if (selected == 1) return &frontend_options_1_1_1;
    return &frontend_options_1_1_2;
}

static const Image *skill_panel(u16 selected) {
    if (selected == DOOM_SKILL_IM_TOO_YOUNG_TO_DIE) return &frontend_skill_0;
    if (selected == DOOM_SKILL_HEY_NOT_TOO_ROUGH) return &frontend_skill_1;
    if (selected == DOOM_SKILL_HURT_ME_PLENTY) return &frontend_skill_2;
    if (selected == DOOM_SKILL_ULTRA_VIOLENCE) return &frontend_skill_3;
    return &frontend_skill_4;
}

static const Image *pause_panel(u16 selected) {
    if (selected == 0) return &frontend_pause_0;
    if (selected == 1) return &frontend_pause_1;
    return &frontend_pause_2;
}

static const Image *confirm_panel(u16 selected) {
    return selected == 0 ? &frontend_confirm_0 : &frontend_confirm_1;
}

static void run_options(u16 tile_base) {
    u16 selected = 0;
    u16 previous;

    draw_panel(options_panel(selected), tile_base);
    wait_for_release(MENU_INPUT);
    previous = JOY_readJoypad(JOY_1);
    while (TRUE) {
        const u16 pressed = read_pressed(&previous);
        bool redraw = FALSE;
        if ((pressed & BUTTON_UP) != 0) {
            selected = (u16)((selected + 2) % 3);
            redraw = TRUE;
        }
        if ((pressed & BUTTON_DOWN) != 0) {
            selected = (u16)((selected + 1) % 3);
            redraw = TRUE;
        }
        if ((pressed & MENU_BACK) != 0) break;
        if ((pressed & MENU_ACCEPT) != 0) {
            if (selected == 0) game_audio_toggle_music();
            else if (selected == 1) game_audio_toggle_sfx();
            else break;
            redraw = TRUE;
        }
        if (redraw) draw_panel(options_panel(selected), tile_base);
        VDP_waitVSync();
    }
    wait_for_release(MENU_INPUT);
}

static bool run_skill_menu(u16 tile_base, DoomSkill *skill) {
    u16 selected = DOOM_SKILL_HURT_ME_PLENTY;
    u16 previous;

    draw_panel(skill_panel(selected), tile_base);
    wait_for_release(MENU_INPUT);
    previous = JOY_readJoypad(JOY_1);
    while (TRUE) {
        const u16 pressed = read_pressed(&previous);
        bool redraw = FALSE;
        if ((pressed & BUTTON_UP) != 0) {
            selected = (u16)((selected + 4) % 5);
            redraw = TRUE;
        }
        if ((pressed & BUTTON_DOWN) != 0) {
            selected = (u16)((selected + 1) % 5);
            redraw = TRUE;
        }
        if ((pressed & MENU_BACK) != 0) return FALSE;
        if ((pressed & MENU_ACCEPT) != 0) {
            *skill = (DoomSkill)selected;
            wait_for_release(MENU_INPUT);
            return TRUE;
        }
        if (redraw) draw_panel(skill_panel(selected), tile_base);
        VDP_waitVSync();
    }
}

static bool run_quit_confirmation(u16 tile_base) {
    u16 selected = 1;
    u16 previous;

    draw_panel(confirm_panel(selected), tile_base);
    wait_for_release(MENU_INPUT);
    previous = JOY_readJoypad(JOY_1);
    while (TRUE) {
        const u16 pressed = read_pressed(&previous);
        if ((pressed & (BUTTON_UP | BUTTON_DOWN)) != 0) {
            selected ^= 1;
            draw_panel(confirm_panel(selected), tile_base);
        }
        if ((pressed & MENU_BACK) != 0) return FALSE;
        if ((pressed & MENU_ACCEPT) != 0) return selected == 0;
        VDP_waitVSync();
    }
}

static bool run_main_menu(u16 menu_base, u16 overlay_base, DoomSkill *skill) {
    u16 selected = 0;
    u16 previous;
    u32 ticks = 0;
    bool last_frame = FALSE;

    debug_checkpoint_mark(DEBUG_CHECKPOINT_MENU);
    clear_plane_cpu(BG_A);
    clear_plane_cpu(BG_B);
    game_audio_suspend_for_video();
    VDP_waitVSync();
    VDP_setEnable(FALSE);
    VDP_drawImageEx(BG_A, &frontend_main_menu,
                    TILE_ATTR_FULL(PAL0, FALSE, FALSE, FALSE, menu_base),
                    0, 0, TRUE, TRUE);
    load_main_cursor_tiles(overlay_base);
    draw_main_cursor(overlay_base, selected, FALSE);
    VDP_setEnable(TRUE);
    game_audio_resume_after_video();
    wait_for_release(MENU_INPUT);
    previous = JOY_readJoypad(JOY_1);
    while (TRUE) {
        const u16 pressed = read_pressed(&previous);
        bool redraw = FALSE;
        if ((pressed & BUTTON_UP) != 0) {
            VDP_clearTileMapRect(BG_A, MAIN_CURSOR_X,
                                 (u16)(MAIN_CURSOR_Y + selected * MAIN_CURSOR_STEP), 3, 3);
            selected = (u16)((selected + 2) % 3);
            redraw = TRUE;
        }
        if ((pressed & BUTTON_DOWN) != 0) {
            VDP_clearTileMapRect(BG_A, MAIN_CURSOR_X,
                                 (u16)(MAIN_CURSOR_Y + selected * MAIN_CURSOR_STEP), 3, 3);
            selected = (u16)((selected + 1) % 3);
            redraw = TRUE;
        }
        if ((pressed & MENU_BACK) != 0) return FALSE;
        if ((pressed & MENU_ACCEPT) != 0) {
            if (selected == 0) {
                if (run_skill_menu((u16)(overlay_base + frontend_skull1.tileset->numTile), skill)) {
                    return TRUE;
                }
                clear_plane_cpu(BG_A);
                clear_plane_cpu(BG_B);
                game_audio_suspend_for_video();
                VDP_waitVSync();
                VDP_setEnable(FALSE);
                VDP_drawImageEx(BG_A, &frontend_main_menu,
                                TILE_ATTR_FULL(PAL0, FALSE, FALSE, FALSE, menu_base),
                                0, 0, TRUE, TRUE);
                load_main_cursor_tiles(overlay_base);
                draw_main_cursor(overlay_base, selected, FALSE);
                VDP_setEnable(TRUE);
                game_audio_resume_after_video();
                wait_for_release(MENU_INPUT);
                previous = JOY_readJoypad(JOY_1);
            }
            else if (selected == 1) {
                clear_plane_cpu(BG_A);
                clear_plane_cpu(BG_B);
                run_options((u16)(overlay_base + frontend_skull1.tileset->numTile));
                clear_plane_cpu(BG_A);
                game_audio_suspend_for_video();
                VDP_waitVSync();
                VDP_setEnable(FALSE);
                VDP_drawImageEx(BG_A, &frontend_main_menu,
                                TILE_ATTR_FULL(PAL0, FALSE, FALSE, FALSE, menu_base),
                                0, 0, TRUE, TRUE);
                load_main_cursor_tiles(overlay_base);
                draw_main_cursor(overlay_base, selected, FALSE);
                VDP_setEnable(TRUE);
                game_audio_resume_after_video();
                wait_for_release(MENU_INPUT);
                previous = JOY_readJoypad(JOY_1);
            } else return FALSE;
        }
        ticks++;
        if ((ticks & 15) == 0) {
            last_frame = !last_frame;
            redraw = TRUE;
        }
        if (redraw) draw_main_cursor(overlay_base, selected, last_frame);
        VDP_waitVSync();
    }
}

DoomSkill frontend_run(void) {
    bool title_music_running = FALSE;

    if (!s_boot_sequence_played) {
        run_boot_sequence();
        s_boot_sequence_played = TRUE;
        title_music_running = TRUE;
    }
    while (TRUE) {
        u16 previous;
        u32 ticks = 0;
        const u16 title_base = TILE_USER_INDEX;
        const u16 prompt_base = (u16)(title_base + frontend_title.tileset->numTile);
        const u16 menu_base = TILE_USER_INDEX;
        const u16 overlay_base = (u16)(menu_base + frontend_main_menu.tileset->numTile);

        debug_checkpoint_reset();
        debug_checkpoint_mark(DEBUG_CHECKPOINT_TITLE);
        frontend_video_init();
        VDP_drawImageEx(BG_B, &frontend_title,
                        TILE_ATTR_FULL(PAL0, FALSE, FALSE, FALSE, title_base),
                        0, 0, TRUE, TRUE);
        VDP_loadTileSet(frontend_prompt.tileset, prompt_base, DMA);
        VDP_setEnable(TRUE);
        VDP_waitVSync();
        if (!title_music_running) {
            game_audio_play_music(intro_music);
            title_music_running = TRUE;
        }
        wait_for_release(BUTTON_START);
        previous = JOY_readJoypad(JOY_1);
        while (TRUE) {
            const u16 pressed = read_pressed(&previous);
            if ((ticks & 63) == 0) {
                VDP_setTileMapEx(BG_A, frontend_prompt.tilemap,
                                 TILE_ATTR_FULL(PAL0, TRUE, FALSE, FALSE, prompt_base),
                                 14, 27, 0, 0,
                                 frontend_prompt.tilemap->w, frontend_prompt.tilemap->h, CPU);
            } else if ((ticks & 63) == 48) {
                VDP_clearTileMapRect(BG_A, 14, 27, 12, 1);
            }
            if ((pressed & BUTTON_START) != 0) break;
            ticks++;
            VDP_waitVSync();
        }
        clear_plane_cpu(BG_A);
        clear_plane_cpu(BG_B);
        {
            DoomSkill skill = DOOM_SKILL_HURT_ME_PLENTY;
            if (run_main_menu(menu_base, overlay_base, &skill)) return skill;
        }
    }
}

// Centred in the 20-tile-wide (160px) view region (VIEW_TILEMAP_X 10 ..
// VIEW_TILEMAP_X+19), near the top of the view (screen tile row 8 =
// VIEW_TILEMAP_Y 5 + 3) so the prompt never fights the weapon overlay lower
// down for the same tiles. Moves with the view.
#define DEATH_PROMPT_X 14
#define DEATH_PROMPT_Y 8

void frontend_load_death_prompt(u16 tile_base) {
    VDP_loadTileSet(frontend_death_prompt.tileset, tile_base, DMA);
}

void frontend_set_death_prompt(u16 tile_base, bool visible) {
    if (visible) {
        VDP_setTileMapEx(BG_A, frontend_death_prompt.tilemap,
                         TILE_ATTR_FULL(PAL0, TRUE, FALSE, FALSE, tile_base),
                         DEATH_PROMPT_X, DEATH_PROMPT_Y, 0, 0,
                         frontend_death_prompt.tilemap->w, frontend_death_prompt.tilemap->h, CPU);
    } else {
        VDP_clearTileMapRect(BG_A, DEATH_PROMPT_X, DEATH_PROMPT_Y,
                             frontend_death_prompt.tilemap->w, frontend_death_prompt.tilemap->h);
    }
}

FrontendPauseAction frontend_run_pause(u16 tile_base) {
    u16 selected = 0;
    u16 previous;

    clear_plane_cpu(BG_A);
    draw_panel(pause_panel(selected), tile_base);
    wait_for_release(MENU_INPUT);
    previous = JOY_readJoypad(JOY_1);
    while (TRUE) {
        const u16 pressed = read_pressed(&previous);
        bool redraw = FALSE;
        if ((pressed & BUTTON_UP) != 0) {
            selected = (u16)((selected + 2) % 3);
            redraw = TRUE;
        }
        if ((pressed & BUTTON_DOWN) != 0) {
            selected = (u16)((selected + 1) % 3);
            redraw = TRUE;
        }
        if ((pressed & MENU_BACK) != 0) {
            wait_for_release(MENU_INPUT);
            return FRONTEND_PAUSE_RESUME;
        }
        if ((pressed & MENU_ACCEPT) != 0) {
            if (selected == 0) {
                wait_for_release(MENU_INPUT);
                return FRONTEND_PAUSE_RESUME;
            }
            if (selected == 1) {
                run_options(tile_base);
                draw_panel(pause_panel(selected), tile_base);
                wait_for_release(MENU_INPUT);
                previous = JOY_readJoypad(JOY_1);
            } else if (run_quit_confirmation(tile_base)) {
                wait_for_release(MENU_INPUT);
                return FRONTEND_PAUSE_QUIT_TO_TITLE;
            } else {
                draw_panel(pause_panel(selected), tile_base);
                wait_for_release(MENU_INPUT);
                previous = JOY_readJoypad(JOY_1);
            }
        }
        if (redraw) draw_panel(pause_panel(selected), tile_base);
        VDP_waitVSync();
    }
}
