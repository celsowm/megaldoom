#include "renderer_pack_internal.h"
#include "generated_renderer_assets.h"

static const u8 FLAT_BAYER_4X4[4][4] = {
    {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5},
};

static u8 sample_flat_color(const RayFlatColor *material, u16 x, u16 y) {
    return (FLAT_BAYER_4X4[y & 3][x & 3] < material->secondary_coverage) ?
        material->secondary : material->primary;
}

#if RAY_COL_STRIDE == 4
const u32 REP4[16] = {
    0x0000, 0x1111, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666, 0x7777,
    0x8888, 0x9999, 0xAAAA, 0xBBBB, 0xCCCC, 0xDDDD, 0xEEEE, 0xFFFF,
};

static u16 pack_flat_quad(const RayFlatColor *material, u16 x, u16 y) {
    return (u16)REP4[sample_flat_color(material, x, y) & 0x0F];
}
#else /* RAY_COL_STRIDE == 2 */
const u32 REP2[16] = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
};

static u8 pack_flat_pair(const RayFlatColor *material, u16 x, u16 y) {
    return (u8)((sample_flat_color(material, x, y) << 4) |
                sample_flat_color(material, (u16)(x + 1), y));
}

static u32 pack_flat_row(const RayFlatColor *material, u16 x, u16 y) {
    return ((u32)pack_flat_pair(material, x, y) << 24) |
           ((u32)pack_flat_pair(material, (u16)(x + 2), y) << 16) |
           ((u32)pack_flat_pair(material, (u16)(x + 4), y) << 8) |
           pack_flat_pair(material, (u16)(x + 6), y);
}
#endif

PackedFlatRows build_flat_rows(const RaySceneColors *scene_colors) {
    PackedFlatRows rows;
    for (u16 y = 0; y < 4; y++) {
#if RAY_COL_STRIDE == 4
        const u16 ceiling = pack_flat_quad(&scene_colors->ceiling, 0, y);
        rows.ceiling[y] = ((u32)ceiling << 16) | ceiling;
        const u16 floor = pack_flat_quad(&scene_colors->floor, 0, y);
        rows.floor[y] = ((u32)floor << 16) | floor;
#else
        rows.ceiling[y] = pack_flat_row(&scene_colors->ceiling, 0, y);
        // Per-sector floor, mirroring the ceiling above: tools/wad-map-extract.py
        // now bakes FREEDOOM_SECTOR_VISUALS[sector][3..5] from that sector's own
        // floor flat (lit, chroma-clamped, best_mix-dithered) instead of forcing
        // every sector to one ROM-constant grey. See tools/test-wall-quality.py
        // for the guardrail that floor and ceiling never share an index.
        rows.floor[y] = pack_flat_row(&scene_colors->floor, 0, y);
#endif
    }
    return rows;
}
