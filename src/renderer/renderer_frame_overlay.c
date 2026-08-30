#include "renderer_pack_internal.h"
#include "generated_renderer_assets.h"
#include "weapons.h"

static s16 g_last_weapon_variant = -1;
static u8 g_weapon_id = WEAPON_PISTOL;

// Only one weapon's tiles are resident at a time: five weapons would need ~245
// tiles and the region below the SGDK font holds 72. Switching therefore DMAs
// the new weapon's tileset over the old one, which is cheap (it happens on a
// button press, not per frame) and keeps the VRAM map fixed.
void reload_weapon_tiles(void) {
    VDP_loadTileData((const u32 *)MEGALDOOM_WEAPON_TILES[g_weapon_id], WEAPON_TILE_BASE,
                     MEGALDOOM_WEAPON_TILE_COUNTS[g_weapon_id], DMA);
    // The tilemap cache is keyed on the variant alone, so anything that changes
    // or restores the resident tileset has to invalidate it, or the overlay
    // would keep pointing at the previous weapon's tile indices.
    g_last_weapon_variant = -1;
}

void renderer_set_weapon(u8 weapon_id) {
    if (weapon_id >= MEGALDOOM_WEAPON_COUNT) return;
    g_weapon_id = weapon_id;
    reload_weapon_tiles();
}

// The weapon is a transparent BG_A tile layer. Camera movement therefore never
// recomposes it into the 300 dynamic view tiles; only idle/fire transitions
// rewrite this small tilemap rectangle.
void draw_weapon_overlay(bool flash) {
    const s16 variant = flash ? 1 : 0;
    // FIST is baked against the portrait's skin/metal ramp in PAL2. Every
    // other weapon stays in PAL3, so their current world-compatible bake and
    // muzzle-flash colours remain exactly as before.
    const u16 palette = (g_weapon_id == WEAPON_FIST) ? PAL2 : PAL3;
    u16 tilemap[MEGALDOOM_WEAPON_TILE_W * MEGALDOOM_WEAPON_TILE_H];
    if (variant == g_last_weapon_variant) return;

    for (u16 i = 0; i < (MEGALDOOM_WEAPON_TILE_W * MEGALDOOM_WEAPON_TILE_H); i++) {
        const u16 tile = MEGALDOOM_WEAPON_TILEMAP[g_weapon_id][variant][i];
        tilemap[i] = TILE_ATTR_FULL(palette, FALSE, FALSE, FALSE,
            (tile == 0xFFFF) ? 0 : (WEAPON_TILE_BASE + tile));
    }
    VDP_setTileMapDataRect(BG_A, tilemap,
        VIEW_TILEMAP_X + MEGALDOOM_WEAPON_TILE_X,
        VIEW_TILEMAP_Y + MEGALDOOM_WEAPON_TILE_Y,
        MEGALDOOM_WEAPON_TILE_W, MEGALDOOM_WEAPON_TILE_H,
        MEGALDOOM_WEAPON_TILE_W, CPU);
    g_last_weapon_variant = variant;
}

void renderer_draw_weapon_flash(void) {
    draw_weapon_overlay(TRUE);
}

// BG_A now holds only the weapon tilemap (the status-bar numbers moved to the
// WINDOW plane), so a whole-plane scroll bobs the weapon at pixel precision for
// the cost of one VDP register write and zero tile DMA. The last-applied cache
// keeps a steady walk from re-poking the register.
//
// SGDK plane-scroll convention: +H shifts the plane right (so +bob_x tracks the
// weapon right), +V shifts the plane up. bob_y is Doom's always-positive lobe
// and only ever DIPS the gun down (-V): the window plane is pinned from
// VIEW_WINDOW_TOP_Y, the row immediately below the (centred) 3D view, so the
// gun's bottom rows dip into a region where plane A is suppressed and the
// cut-off edge is never seen -- the view itself no longer has to sit flush on
// the status bar for that to hold, which is what lets it be centred. A
// downward dip also reveals a sliver of scene above the hands, exactly like
// Doom. Lifting the gun (+V) would float its base over the floor, so bob_y is
// never negative.
static s16 s_bob_applied_x = -1;
static s16 s_bob_applied_y = -1;

void renderer_apply_weapon_bob(s16 bob_x, s16 bob_y) {
    if (bob_x == s_bob_applied_x && bob_y == s_bob_applied_y) return;
    s_bob_applied_x = bob_x;
    s_bob_applied_y = bob_y;
    VDP_setHorizontalScroll(BG_A, bob_x);
    VDP_setVerticalScroll(BG_A, (s16)-bob_y);
}

void draw_overlay_ops(const MegalDoomOverlayRowOp *ops, u16 count) {
    u32 *base = &g_view_tiles[0][0];

    for (u16 i = 0; i < count; i++) {
        const MegalDoomOverlayRowOp *op = &ops[i];
        u32 *dst = base + op->dst;
        renderer_mark_overlay_tile((u16)(op->dst / 8));
        *dst = (*dst & ~op->clear_mask) | op->value;
    }
}

void frame_overlay_reset(void) {
    g_last_weapon_variant = -1;
    // Park the weapon at its neutral origin. A scene invalidate / level load /
    // menu return may have left BG_A scrolled or had the frontend move it, so
    // write the registers unconditionally here (and prime the cache to force the
    // next apply to write too).
    s_bob_applied_x = -1;
    s_bob_applied_y = -1;
    VDP_setHorizontalScroll(BG_A, 0);
    VDP_setVerticalScroll(BG_A, 0);
}
