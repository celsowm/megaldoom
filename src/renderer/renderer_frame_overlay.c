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
    u16 tilemap[MEGALDOOM_WEAPON_TILE_W * MEGALDOOM_WEAPON_TILE_H];
    if (variant == g_last_weapon_variant) return;

    for (u16 i = 0; i < (MEGALDOOM_WEAPON_TILE_W * MEGALDOOM_WEAPON_TILE_H); i++) {
        const u16 tile = MEGALDOOM_WEAPON_TILEMAP[g_weapon_id][variant][i];
        tilemap[i] = TILE_ATTR_FULL(PAL3, FALSE, FALSE, FALSE,
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
}
