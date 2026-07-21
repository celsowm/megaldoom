#include "renderer_pack_internal.h"
#include "generated_renderer_assets.h"

static s16 g_last_weapon_variant = -1;

// The weapon is a transparent BG_A tile layer. Camera movement therefore never
// recomposes it into the 300 dynamic view tiles; only idle/fire transitions
// rewrite this small tilemap rectangle.
void draw_weapon_overlay(bool flash) {
    const s16 variant = flash ? 1 : 0;
    u16 tilemap[MEGALDOOM_WEAPON_TILE_W * MEGALDOOM_WEAPON_TILE_H];
    if (variant == g_last_weapon_variant) return;

    for (u16 i = 0; i < (MEGALDOOM_WEAPON_TILE_W * MEGALDOOM_WEAPON_TILE_H); i++) {
        const u16 tile = MEGALDOOM_WEAPON_TILEMAP[variant][i];
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
