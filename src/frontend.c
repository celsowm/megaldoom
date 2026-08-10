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
/* The skull art is 24px tall; its visible head must sit beside the selected
 * label, rather than one tile above it. */
#define MAIN_CURSOR_Y 12
#define MAIN_CURSOR_STEP 4

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
        game_audio_play_music(intro_music);
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
// VIEW_TILEMAP_X+19), well above the weapon overlay (which starts at view
// row 10, i.e. screen tile row 15) so the prompt never fights the weapon
// sprite for the same tiles.
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
