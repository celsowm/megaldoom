#include "renderer_pack_internal.h"
#include "generated_assets.h"
#include "generated_hud_assets.h"
#include "generated_renderer_assets.h"

// The weapon VRAM window holds ONE weapon at a time and is sized to the largest
// of them (renderer_set_weapon streams the rest in on a switch). If a resprite
// pushes that past the font region the fix is a smaller FREEDOOM_WEAPON_RECT_*
// in tools/convert-freedoom-assets.ps1, not a higher limit here.
#if (WEAPON_TILE_BASE + MEGALDOOM_WEAPON_MAX_TILE_COUNT) > HUD_VRAM_SAFE_TILE_LIMIT
#error "Weapon tiles overlap the SGDK font VRAM region"
#endif
#if RENDERER_SPARSE_FB && ((WEAPON_TILE_BASE + MEGALDOOM_WEAPON_MAX_TILE_COUNT) > STATIC_CEILING_ATLAS_BASE)
#error "Weapon tiles overlap the static ceiling/floor atlas"
#endif

u32 g_view_tiles[VIEW_TILE_ALLOC][8];
u32 g_view_bank_dirty_bits[VIEW_BANK_COUNT][VIEW_DIRTY_WORD_COUNT];
u16 g_view_bank_dirty_count[VIEW_BANK_COUNT];
u16 g_view_vram_bank;
u16 g_view_dirty_bank_mask;
// Per-sector ceiling atlas tile selection (Phase 2, Task 2). Indexed by sector;
// holds MEGALDOOM_SECTOR_CEILING_TILE_INDEX[sector]. Only consumed when
// RENDERER_SPARSE_FB == 1; dead code while the flag is 0.
u8 g_sector_ceiling_tile[FREEDOOM_SECTOR_VISUAL_COUNT];

#if DEBUG_PERF
// Distinct-tile-modified accounting for the perf overlay. Deduplicated per
// frame via g_frame_modified_bits; renderer_mark_tile_dirty sets a bit the
// first time a tile is dirtied this frame.
static u32 g_frame_modified_bits[VIEW_DIRTY_WORD_COUNT];
static u16 g_frame_modified_count;
#endif


_Static_assert((VIEW_TILE_BASE + (VIEW_BANK_COUNT * VIEW_TILE_ALLOC)) <= 2048,
               "view bank base must stay inside the VDP tile-index field");

static void load_game_palettes(void) {
    PAL_setColor(0, RGB24_TO_VDPCOLOR(0x000000));
    PAL_setColor(1, RGB24_TO_VDPCOLOR(0xD8D8D8));
    PAL_setColor(2, RGB24_TO_VDPCOLOR(0x181410));
    PAL_setColor(3, RGB24_TO_VDPCOLOR(0x383030));
    PAL_setColor(4, RGB24_TO_VDPCOLOR(0x585048));
    PAL_setColor(5, RGB24_TO_VDPCOLOR(0x888078));
    PAL_setColor(6, RGB24_TO_VDPCOLOR(0xB4ACA0));
    PAL_setColor(7, RGB24_TO_VDPCOLOR(0x404020));
    PAL_setColor(8, RGB24_TO_VDPCOLOR(0x301E10));
    // Index 9 is reserved for the death-screen "PRESS FIRE" prompt (frontend.c
    // frontend_load_death_prompt / frontend_death_prompt asset). The HUD tiles
    // only use indices 2-8, so this is otherwise unclaimed in PAL0.
    PAL_setColor(9, RGB24_TO_VDPCOLOR(0xD80000));

    // The SGDK stock font paints colour index 15, and nothing else in gameplay
    // writes PAL0[10..15] -- so text drawn with it inherits whatever the last
    // frontend image left there (0x000000 in every frontend PNG), i.e. black
    // glyphs on a black backdrop. No shipped tile uses PAL0 index 15 (the
    // status bar stops at 8, the view is PAL3, the face PAL2, the status
    // numbers PAL1), so this is invisible to everything but stock-font debug
    // text. Originally gated to DEBUG_PERF (its perf overlay was the only user
    // and the guard kept the release palette upload byte-identical), but
    // debug_light.c reuses the same font/colour convention and ships in
    // release ROMs -- reached only through the title's OPTIONS menu, so a
    // route that starts gameplay directly never exercised this. Unconditional
    // now; still a no-op for every other tile.
    PAL_setColor(15, RGB24_TO_VDPCOLOR(0xFFFFFF));

    // Palette line 1: native Doom STTNUM/STTPRCNT shading for the transparent
    // BG_A status-number compositor.
    for (u16 i = 0; i < 16; i++) {
        PAL_setColor((u16)(16 + i), RGB24_TO_VDPCOLOR(FREEDOOM_HUD_DIGIT_PALETTE[i]));
    }

    // Palette line 2: dedicated skin/brown/red ramp for the Doom-guy portrait so
    // the face keeps proper flesh tones instead of going gold under PAL0.
    for (u16 i = 0; i < 16; i++) {
        PAL_setColor((u16)(32 + i), RGB24_TO_VDPCOLOR(FREEDOOM_FACE_PALETTE[i]));
    }

    // Palette line 3 is dedicated to the dynamic 3D view. Keeping it separate
    // from the status bar gives E1M1's walls, weapon and actors all 16 entries.
    for (u16 i = 0; i < 16; i++) {
        PAL_setColor((u16)(48 + i), RGB24_TO_VDPCOLOR(FREEDOOM_WORLD_PALETTE[i]));
    }
}

void renderer_load_world_palette(void) {
    for (u16 i = 0; i < 16; i++) {
        PAL_setColor((u16)(48 + i), RGB24_TO_VDPCOLOR(FREEDOOM_WORLD_PALETTE[i]));
    }
}

static void init_video(void) {
    VDP_setScreenWidth320();
    VDP_setScreenHeight224();
    VDP_setHInterrupt(FALSE);
    VDP_setHilightShadow(FALSE);
    VDP_setTextPlane(BG_A);
    // Weapon bob scrolls BG_A as a whole plane; keep both axes in plain-scroll
    // mode (the frontend leaves it here, but do not depend on that).
    VDP_setScrollingMode(HSCROLL_PLANE, VSCROLL_PLANE);
    VDP_setHorizontalScroll(BG_A, 0);
    VDP_setVerticalScroll(BG_A, 0);

    load_game_palettes();

    VDP_clearPlane(BG_A, TRUE);
    VDP_clearPlane(BG_B, TRUE);
    VDP_clearPlane(WINDOW, TRUE);
    VDP_setBackgroundColor(0);
}

static u32 make_pair_tile_row(u8 left_color, u8 right_color) {
    u32 row = 0;
    left_color &= 0x0F;
    right_color &= 0x0F;

    for (u16 x = 0; x < 4; x++) {
        row = (row << 4) | left_color;
    }
    for (u16 x = 0; x < 4; x++) {
        row = (row << 4) | right_color;
    }
    return row;
}

static void init_hud_tiles(void) {
    VDP_loadTileData((const u32 *)FREEDOOM_HUD_TILES, HUD_TILE_BASE, FREEDOOM_HUD_TILE_COUNT, DMA);
    // No face upload here any more: the portrait streams one 16-tile frame into
    // FACE_TILE_BASE from draw_hud_face(). Blitting all FREEDOOM_FACE_TILE_COUNT
    // (258) tiles would now run straight over the HUD number and weapon windows.
    // Restores whichever weapon is currently selected into the shared window
    // (the pistol at boot). This runs from renderer_restore_after_menu too, so
    // it must NOT reset the selection -- a pause must not disarm the player.
    reload_weapon_tiles();
}

void renderer_set_view_vram_bank(u16 bank) {
    g_view_vram_bank = (u16)(bank & 1);

    // No RAM-side tilemap: the mapping is generated straight into the plane.
    //
    // g_view_tiles is COLUMN-major (view_tile_index = x*VIEW_TILE_STRIDE + y),
    // so one screen column's tiles are a CONTIGUOUS ASCENDING run of VRAM
    // indices -- which is exactly what VDP_fillTileMapRectInc writes into a
    // 1-tile-wide, VIEW_TILE_H-tall rect. One call per column replaces a
    // VIEW_TILE_ALLOC-entry screen-order array that had to be materialized in
    // work RAM and pushed through the slower ...RectEx path. Work RAM is the
    // budget the viewport option spends, and this gives 704 bytes of it back for
    // the same number of VRAM writes: each call sets the auto-increment once and
    // streams its column.
    const u16 bank_base = (u16)(VIEW_TILE_BASE + (g_view_vram_bank * VIEW_TILE_ALLOC));
    for (u16 x = 0; x < VIEW_TILE_W; x++) {
        VDP_fillTileMapRectInc(BG_B,
                               TILE_ATTR_FULL(PAL3, FALSE, FALSE, FALSE,
                                              (u16)(bank_base + view_tile_index(x, 0))),
                               (u16)(VIEW_TILEMAP_X + x),
                               VIEW_TILEMAP_Y,
                               1,
                               VIEW_TILE_H);
    }
}

static void init_view_tilemap(void) {
    // The view tilemap points at one of two dynamic tile banks. Turn/base redraws
    // upload into the inactive bank, then swap this map only after the upload is
    // complete so a half-updated view is never displayed.
    renderer_set_view_vram_bank(0);
}

void renderer_mark_tile_dirty(u16 tile_index) {
    const u16 word = (u16)(tile_index >> 5);
    const u32 mask = (u32)1u << (tile_index & 31);

    // Overlay-only redraws mutate the displayed bank in place. Base redraws
    // prepare an unconditional full upload to the inactive bank up front and
    // set this mask to zero, avoiding 300 redundant per-tile bitmap updates.
    for (u16 bank = 0; bank < VIEW_BANK_COUNT; bank++) {
        if ((g_view_dirty_bank_mask & (1u << bank)) == 0) continue;
        if ((g_view_bank_dirty_bits[bank][word] & mask) == 0) {
            g_view_bank_dirty_bits[bank][word] |= mask;
            g_view_bank_dirty_count[bank]++;
        }
    }
#if DEBUG_PERF
    // Count each distinct tile once per frame regardless of how many banks are
    // marked stale — this is the CPU-side "tiles modified" figure, separate
    // from the per-bank "tiles uploaded" figure tracked in the uploader.
    if ((g_frame_modified_bits[word] & mask) == 0) {
        g_frame_modified_bits[word] |= mask;
        g_frame_modified_count++;
    }
#endif
}

void renderer_prepare_full_base_upload(void) {
    const u16 target_bank = (u16)(g_view_vram_bank ^ 1u);

    for (u16 bank = 0; bank < VIEW_BANK_COUNT; bank++) {
        for (u16 word = 0; word < VIEW_DIRTY_WORD_COUNT; word++) {
            g_view_bank_dirty_bits[bank][word] = 0;
        }
        g_view_bank_dirty_count[bank] = 0;
    }
    for (u16 tile = 0; tile < VIEW_TILE_COUNT; tile++) {
        if (!view_tile_is_live(tile)) continue;
        const u16 word = (u16)(tile >> 5);
        g_view_bank_dirty_bits[target_bank][word] |= (u32)1u << (tile & 31);
    }
    g_view_bank_dirty_count[target_bank] = VIEW_TILE_LIVE_COUNT;
    g_view_dirty_bank_mask = 0;
#if DEBUG_PERF
    for (u16 word = 0; word < VIEW_DIRTY_WORD_COUNT; word++) {
        g_frame_modified_bits[word] = 0xFFFFFFFFu;
    }
    g_frame_modified_count = VIEW_TILE_LIVE_COUNT;
#endif
}

void set_view_pair_tile(u16 x, u16 y, u8 left_color, u8 right_color) {
    const u32 row = make_pair_tile_row(left_color, right_color);
    const u16 map_index = view_tile_index(x, y);

    for (u16 row_index = 0; row_index < 8; row_index++) {
        g_view_tiles[map_index][row_index] = row;
    }
    renderer_mark_tile_dirty(map_index);
}

void set_view_column_color(u16 column, u16 y, u8 color) {
    const u16 tile_x = (u16)(column >> 3);
    const u16 tile_y = (u16)(y / 8);
    const u16 row_y = (u16)(y & 7);
    const u16 map_index = view_tile_index(tile_x, tile_y);
    const u16 shift = (u16)((7 - (column & 7)) * 4);
    u32 row = g_view_tiles[map_index][row_y];

    row &= ~((u32)0x0F << shift);
    row |= ((u32)(color & 0x0F)) << shift;

    renderer_mark_overlay_tile(map_index);
    g_view_tiles[map_index][row_y] = row;
}

#if RENDERER_SPARSE_FB
// Forward declaration so the call inside renderer_init (below) resolves to this
// static definition without an implicit non-static prototype (which would clash
// with the `static` at the definition).
static void init_static_atlas(void);
#endif

// renderer_init() runs after the main menu (and again at each level
// transition), so it adopts whatever viewport size is currently selected --
// including one the player just chose in the OPTIONS menu. The boot default is
// applied once in main(), not here, or returning to the title would silently
// reset the player's choice.
void renderer_init(void) {
    init_video();
    init_hud_tiles();
    init_view_tilemap();
    renderer_hud_window_setup();
    g_view_dirty_bank_mask = 0;
#if RENDERER_SPARSE_FB
    init_static_atlas();
#endif
    renderer_scene_init();
}

// Phase 2, Task 2: upload the static ceiling/floor atlas ONCE. The floor is the
// ROM-constant MEGALDOOM_WORLD_COLOR_FLOOR; the ceilings are one deduplicated
// 8x8 tile per distinct RayFlatColor key (MEGALDOOM_CEILING_TILES). Neither is
// ever rebaked, so crossing into a different-ceiling sector costs ZERO DMA.
// Guarded by RENDERER_SPARSE_FB: with the flag at 0 this is dead code that is
// never called and does not touch the legacy upload path.
#if RENDERER_SPARSE_FB
static void init_static_atlas(void) {
    // Floor: one constant tile, always tile 0 of the atlas region.
    const u32 floor_rows[8] = { 0 };
    for (u16 y = 0; y < 8; y++) {
        ((u32 *)floor_rows)[y] = (u32)MEGALDOOM_WORLD_COLOR_FLOOR * 0x11111111u;
    }
    VDP_loadTileData(floor_rows, STATIC_FLOOR_TILE_BASE, 1, DMA);
    // Ceiling atlas: one tile per distinct key.
    VDP_loadTileData((const u32 *)MEGALDOOM_CEILING_TILES,
                     STATIC_CEILING_ATLAS_BASE, MEGALDOOM_CEILING_TILE_COUNT, DMA);
    // Populate the per-sector -> atlas-tile selection table.
    for (u16 s = 0; s < FREEDOOM_SECTOR_VISUAL_COUNT; s++) {
        g_sector_ceiling_tile[s] = MEGALDOOM_SECTOR_CEILING_TILE_INDEX[s];
    }
}
#endif

u16 renderer_get_menu_tile_base(void) {
    return PAIR_TILE_BASE;
}

void renderer_restore_after_menu(void) {
    // The pause frontend deliberately borrows the reloadable pair/HUD region
    // while leaving both dynamic view banks untouched. Restore everything it
    // can have overwritten, then rebuild BG_A and force a fresh scene cast.
    //
    // This is also where a VIEW SIZE change made in the pause OPTIONS menu
    // lands. The frontend only records the choice (raycast_set_view_size); the
    // geometry is adopted here and in renderer_init(), which between them cover
    // both ways into the OPTIONS menu. Rebuilding unconditionally keeps that
    // free of a "did the size change?" flag that could go stale.
    //
    // Clearing the play area BEFORE the new tilemap goes down is load-bearing:
    // a viewport that shrank leaves the outer ring of the previous one on BG_B,
    // and nothing would ever write those cells again. Clearing the whole area
    // above the status bar covers any previous size without remembering it.
    VDP_waitVSync();
    load_game_palettes();
    init_hud_tiles();
    VDP_clearTileMapRect(BG_B, 0, 0, SCREEN_TILE_W, HUD_PANEL_Y);
    VDP_clearPlane(BG_A, TRUE);
    init_view_tilemap();
    renderer_invalidate_scene();
    renderer_draw_static_screen();
}

#if DEBUG_PERF
void renderer_reset_frame_modified(void) {
    for (u16 i = 0; i < VIEW_DIRTY_WORD_COUNT; i++) {
        g_frame_modified_bits[i] = 0;
    }
    g_frame_modified_count = 0;
}

u16 renderer_get_frame_modified_count(void) {
    return g_frame_modified_count;
}

#endif
