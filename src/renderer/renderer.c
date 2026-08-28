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

u32 g_view_tiles[VIEW_TILE_COUNT][8];
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


static u16 s_view_bank_tilemaps[VIEW_BANK_COUNT][VIEW_TILE_COUNT];

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
    VDP_loadTileData((const u32 *)FREEDOOM_FACE_TILES, FACE_TILE_BASE, FREEDOOM_FACE_TILE_COUNT, DMA);
    // Restores whichever weapon is currently selected into the shared window
    // (the pistol at boot). This runs from renderer_restore_after_menu too, so
    // it must NOT reset the selection -- a pause must not disarm the player.
    reload_weapon_tiles();
}

static void build_view_bank_tilemaps(void) {
    for (u16 bank = 0; bank < VIEW_BANK_COUNT; bank++) {
        for (u16 y = 0; y < VIEW_TILE_H; y++) {
            for (u16 x = 0; x < VIEW_TILE_W; x++) {
                // The tilemap array stays in screen order (row-major: the VDP
                // scans screen rows), but each entry points at the COLUMN-MAJOR
                // VRAM slot where the packer wrote this tile's pixels
                // (view_tile_index = x*VIEW_TILE_H + y).
                const u16 screen_index = (u16)((y * VIEW_TILE_W) + x);
                const u16 vram_tile = view_tile_index(x, y);
                s_view_bank_tilemaps[bank][screen_index] = TILE_ATTR_FULL(
                    PAL3, FALSE, FALSE, FALSE,
                    VIEW_TILE_BASE + (bank * VIEW_TILE_COUNT) + vram_tile);
            }
        }
    }
}

void renderer_set_view_vram_bank(u16 bank) {
    g_view_vram_bank = (u16)(bank & 1);

    VDP_setTileMapDataRect(BG_B,
                           s_view_bank_tilemaps[g_view_vram_bank],
                           VIEW_TILEMAP_X,
                           VIEW_TILEMAP_Y,
                           VIEW_TILE_W,
                           VIEW_TILE_H,
                           VIEW_TILE_W,
                           CPU);
}

static void init_view_tilemap(void) {
    // The view tilemap points at one of two dynamic tile banks. Turn/base redraws
    // upload into the inactive bank, then swap this map only after the upload is
    // complete so a half-updated view is never displayed.
    build_view_bank_tilemaps();
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
        const u16 word = (u16)(tile >> 5);
        g_view_bank_dirty_bits[target_bank][word] |= (u32)1u << (tile & 31);
    }
    g_view_bank_dirty_count[target_bank] = VIEW_TILE_COUNT;
    g_view_dirty_bank_mask = 0;
#if DEBUG_PERF
    for (u16 word = 0; word < VIEW_DIRTY_WORD_COUNT; word++) {
        g_frame_modified_bits[word] = 0xFFFFFFFFu;
    }
    g_frame_modified_count = VIEW_TILE_COUNT;
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
    VDP_waitVSync();
    load_game_palettes();
    init_hud_tiles();
    VDP_clearPlane(BG_A, TRUE);
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
