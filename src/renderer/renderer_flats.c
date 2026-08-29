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

// The ceiling table is PACK_CEILING_ROW_COUNT rows indexed by absolute screen
// row, so a sky sector can give every row its own colour. An ordinary ceiling
// has a 4-row period and simply repeats it across the table: the asm ceiling
// post then runs the identical instruction sequence for both cases, and the
// indoor frame cost is unchanged.
//
// A whole level's ceiling is ONE colour today (GLOBAL_CEILING_INDEX), so the
// repeated case collapses to the same u32 128 times; the loop is kept general
// because the RayFlatColor Bayer pair is still the renderer's contract.
// Both candidates live in ROM in the same row-indexed shape, so choosing a
// ceiling is a pointer assignment: no per-frame table build, and no work RAM.
// See build_flat_ceiling_rows in tools/world_assets.py for the invariant that
// makes the indoor table expressible as ROM (one ceiling triple, no dither).
static void select_ceiling_rows(PackedFlatRows *rows,
                                const RaySceneColors *scene_colors) {
    rows->ceiling = scene_colors->sky ? MEGALDOOM_SKY_CEILING_ROWS
                                      : MEGALDOOM_FLAT_CEILING_ROWS;
}

// Returned by pointer into a static, and rebuilt only when the scene's flats
// actually change. The ceiling table is PACK_CEILING_ROW_COUNT entries; filling
// and then returning that by value would cost ~130 stores plus a ~530-byte
// copy on EVERY frame, in the stage that already co-dominates the frame
// budget. Crossing a sky threshold is rare, so the steady-state cost here is
// one comparison.
static PackedFlatRows s_flat_rows;
static RaySceneColors s_flat_rows_key;
static bool s_flat_rows_valid = FALSE;

void flat_rows_invalidate(void) {
    s_flat_rows_valid = FALSE;
}

const PackedFlatRows *build_flat_rows(const RaySceneColors *scene_colors) {
    if (s_flat_rows_valid &&
        scene_flats_equal(scene_colors, &s_flat_rows_key)) {
        return &s_flat_rows;
    }
    select_ceiling_rows(&s_flat_rows, scene_colors);
    for (u16 y = 0; y < 4; y++) {
#if RAY_COL_STRIDE == 4
        const u16 floor = pack_flat_quad(&scene_colors->floor, 0, y);
        s_flat_rows.floor[y] = ((u32)floor << 16) | floor;
#else
        s_flat_rows.floor[y] = pack_flat_row(&scene_colors->floor, 0, y);
#endif
    }
    s_flat_rows_key = *scene_colors;
    s_flat_rows_valid = TRUE;
    return &s_flat_rows;
}
