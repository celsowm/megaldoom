#include "renderer_internal.h"
#include "bsp_render.h"
#include "generated_assets.h"
#include "generated_billboard_assets.h"
#include "generated_hud_assets.h"
#include "generated_renderer_assets.h"

// Flat billboard-texture descriptor. Storing the pixels as a plain const u8* (a
// [rows][cols] array decays cleanly) lets one draw loop sample sprites of any size:
// world sprites and enemies are 24x48. Index as pixels[y*w + x].
typedef struct {
    const u8 *pixels;
    u8 w;
    u8 h;
} BillboardTex;

static BillboardTex get_billboard_texture(u8 visual_id, u8 frame) {
    switch (visual_id) {
        case BILLBOARD_VISUAL_DUMMY_DAMAGED:
        case BILLBOARD_VISUAL_DUMMY: {
            const u8 f = (frame < FREEDOOM_BILLBOARD_ENEMY_FRAME_COUNT) ? frame : 0;
            return (BillboardTex){(const u8 *)FREEDOOM_BILLBOARD_ENEMY_FRAMES[f],
                                  FREEDOOM_BILLBOARD_ENEMY_W, FREEDOOM_BILLBOARD_ENEMY_H};
        }
        default:
            if (visual_id < FREEDOOM_BILLBOARD_WORLD_TEXTURE_COUNT) {
                return (BillboardTex){(const u8 *)FREEDOOM_BILLBOARD_WORLD_TEXTURES[visual_id],
                                      FREEDOOM_BILLBOARD_WORLD_W, FREEDOOM_BILLBOARD_WORLD_H};
            }
            return (BillboardTex){(const u8 *)FREEDOOM_BILLBOARD_WORLD_TEXTURES[BILLBOARD_VISUAL_BONUS],
                                  FREEDOOM_BILLBOARD_WORLD_W, FREEDOOM_BILLBOARD_WORLD_H};
    }
}

// Distance fog: walls are darkened in discrete steps the farther they are. Level
// 0 is identity; each higher level applies the luminance-derived mapping emitted
// alongside PAL3, so no hand-authored palette indices can shift a hue to blue.
// N/S ("shade") walls add one extra level.
#define SHADE_LEVELS 4
// depth (world units) >> FOG_SHIFT picks the base fog level. Tuned so mid-room
// walls sit around level 1-2 and distant walls saturate at the darkest level.
#define FOG_SHIFT 9
static u8 g_shade_luts[SHADE_LEVELS][16];
static u32 g_overlay_previous_bits[VIEW_DIRTY_WORD_COUNT];
static u32 g_overlay_current_bits[VIEW_DIRTY_WORD_COUNT];
static u16 g_last_compass_angle = 0xFFFF;
static bool g_base_tiles_valid = FALSE;
static bool g_upload_requires_bank_swap = FALSE;
static bool g_compass_dirty = TRUE;

#if DEBUG_PERF
static u16 s_debug_upload_dirty_tiles;
static u16 s_debug_upload_tiles;
static u16 s_debug_upload_runs;
static u16 s_debug_upload_bank;
static bool s_debug_upload_full;
static bool s_debug_upload_swap;
static u32 s_debug_cast_subticks;
static u32 s_debug_pack_subticks;
static u32 s_debug_billboard_subticks;
static u32 s_debug_weapon_overlay_subticks;
static u32 s_debug_overlay_subticks;
// Upload cost decomposition: prepare = CPU time issuing DMA (run-walk +
// VDP_loadTileData); dma_wait = time blocked inside VDP_waitDMACompletion();
// awaited_vblanks = VDP_waitVSync() calls inside the uploader. The total
// upload "time" is never collapsed into one metric so CPU vs DMA vs
// deliberately-awaited VBlanks stay separable.
static u32 s_debug_upload_prepare_subticks;
static u32 s_debug_dma_wait_subticks;
static u16 s_debug_awaited_vblanks_in_upload;
static u16 s_debug_total_vblanks;
#endif

#if DEBUG_PERF
void renderer_debug_set_cast_subticks(u32 subticks) {
    s_debug_cast_subticks = subticks;
}

void renderer_debug_set_total_vblanks(u16 vblanks) {
    s_debug_total_vblanks = vblanks;
}
#endif

void renderer_mark_overlay_tile(u16 tile_index) {
    const u16 word = (u16)(tile_index >> 5);
    const u32 mask = (u32)1u << (tile_index & 31);

    if ((g_overlay_current_bits[word] & mask) == 0) {
        for (u16 row = 0; row < 8; row++) {
            g_base_view_tiles[tile_index][row] = g_view_tiles[tile_index][row];
        }
        g_overlay_current_bits[word] |= mask;
        renderer_mark_tile_dirty(tile_index);
    }
}

static void build_shade_luts(void) {
    for (u16 c = 0; c < 16; c++) {
        g_shade_luts[0][c] = (u8)c; // level 0 == identity (WALL_IDENT_MAP)
    }
    for (u16 level = 1; level < SHADE_LEVELS; level++) {
        for (u16 c = 0; c < 16; c++) {
            g_shade_luts[level][c] =
                FREEDOOM_WORLD_SHADE_MAP[g_shade_luts[level - 1][c] & 0x0F];
        }
    }
}

static void mark_all_view_banks_dirty(void) {
    for (u16 bank = 0; bank < VIEW_BANK_COUNT; bank++) {
        for (u16 word = 0; word < VIEW_DIRTY_WORD_COUNT; word++) {
            g_view_bank_dirty_bits[bank][word] = 0;
        }
        for (u16 tile = 0; tile < VIEW_TILE_COUNT; tile++) {
            const u16 word = (u16)(tile >> 5);
            g_view_bank_dirty_bits[bank][word] |= (u32)1u << (tile & 31);
        }
        g_view_bank_dirty_count[bank] = VIEW_TILE_COUNT;
    }
}

// Pixel-replication table for the active stride (guarded so the unused one isn't
// compiled): REP4[c] == c*0x1111 spreads a colour across 4px (stride 4); REP2[c] ==
// c*0x11 spreads it across 2px (stride 2, four cast columns per 8px tile).
#if !BSP_SECTOR_RENDERER
#if RAY_COL_STRIDE == 4
static const u32 REP4[16] = {
    0x0000, 0x1111, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666, 0x7777,
    0x8888, 0x9999, 0xAAAA, 0xBBBB, 0xCCCC, 0xDDDD, 0xEEEE, 0xFFFF,
};
#else /* RAY_COL_STRIDE == 2 */
static const u32 REP2[16] = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
};

static u32 pack_flat_row(u8 color) {
    const u32 replicated = REP2[color & 0x0F];
    return (replicated << 24) | (replicated << 16) | (replicated << 8) | replicated;
}
#endif
#endif

// Precomposed weapon overlay: the weapon bitmap is static per variant (idle /
// fire), so instead of re-testing ~72x54 pixels every redraw we bake, once at
// init, one (dst, clear_mask, value) op per affected tile-row. Drawing then
// becomes a handful of hundred u32 read-modify-writes: dst = (dst & ~clear) | val.
// Result is byte-identical to the old per-pixel loop (each nibble is written by
// exactly one weapon pixel; transparent pixels leave clear/val bits at 0).
// Stored as a struct-of-arrays to keep .bss tiny (MD has only 64KB RAM shared by
// heap + stack): a u16 flat index into g_view_tiles[][] plus the packed weapon
// nibbles. clear_mask is NOT stored - it is derived at draw time (see below),
// which is exact because weapon texels are 4bpp palette indices 1..15, so every
// opaque pixel leaves a nonzero nibble in value.
//
// Theoretical max = affected tile span (10 x 8 tiles) * 8 rows; assets currently
// use 331 (idle) / 277 (fire). The builder guards against overflow.
void renderer_scene_init(void) {
    build_shade_luts();
    g_last_compass_angle = 0xFFFF;
    g_base_tiles_valid = FALSE;
    g_upload_requires_bank_swap = FALSE;
    g_compass_dirty = TRUE;
    mark_all_view_banks_dirty();
}

void renderer_invalidate_scene(void) {
    g_last_compass_angle = 0xFFFF;
    g_base_tiles_valid = FALSE;
    g_upload_requires_bank_swap = FALSE;
    g_compass_dirty = TRUE;
    for (u16 i = 0; i < VIEW_DIRTY_WORD_COUNT; i++) {
        g_overlay_previous_bits[i] = 0;
        g_overlay_current_bits[i] = 0;
    }
    mark_all_view_banks_dirty();
}

// The raycast tile packer is not linked by the sector renderer.
#if !BSP_SECTOR_RENDERER
typedef struct {
    u16 top;
    u16 bottom;
    const u8 *texture;
    const u8 *shade_map;
    const u8 *vertical_samples;
    u8 tex_x;
    u8 tex_y;
} WallColumnDescriptor;

static WallColumnDescriptor describe_wall_column(const RayColumn *column) {
    const u16 wall_h = column->height;
    const u16 top = (u16)((VIEW_PIXEL_H - wall_h) / 2);
    const u16 bottom = (u16)(top + wall_h);
    const u8 tid = (u8)((column->texture_id < FREEDOOM_WALL_TEXTURE_COUNT) ?
                            column->texture_id : MEGALDOOM_TEX_FALLBACK);
    const u8 (*tex)[WALL_TEX_DIM] = FREEDOOM_WALL_TEXTURES[tid];
    // Distance fog + side shading fold into one LUT selection per column: the fog
    // level grows with depth, and N/S ("shade") walls add one extra darkening step.
    // g_shade_luts[0] is the identity, so near front walls are unshaded; every level
    // maps 0 -> 0, preserving transparency, and the inner loop stays branch-free.
    u16 fog_level = (u16)(column->depth >> FOG_SHIFT) + (column->shade ? 1u : 0u);
    if (fog_level > (SHADE_LEVELS - 1)) {
        fog_level = SHADE_LEVELS - 1;
    }
    const u8 *shade_map = g_shade_luts[fog_level];
    const u8 *ty_table = MEGALDOOM_WALL_TEX_Y_BY_HEIGHT[wall_h];
    const u8 tex_x = (u8)(column->tex_x & WALL_TEX_DIM_MASK);

    return (WallColumnDescriptor){top, bottom, (const u8 *)tex, shade_map, ty_table,
                                  tex_x, column->tex_y};
}

#if RAY_COL_STRIDE == 2
static u8 sample_wall_descriptor(const WallColumnDescriptor *descriptor,
                                 const RaySceneColors *scene_colors,
                                 u16 y) {
    if (y < descriptor->top) {
        return scene_colors->ceiling_color;
    }
    if (y >= descriptor->bottom) {
        return scene_colors->floor_color;
    }

    return descriptor->shade_map[
        (descriptor->texture[((descriptor->vertical_samples[y - descriptor->top] +
                              descriptor->tex_y) & WALL_TEX_DIM_MASK) * WALL_TEX_DIM +
                               descriptor->tex_x]) & 0x0F];
}
#endif
#endif
#if !BSP_SECTOR_RENDERER
static bool overlay_previously_touched(u16 tile_index) {
    const u16 word = (u16)(tile_index >> 5);
    const u32 mask = (u32)1u << (tile_index & 31);

    return (bool)((g_overlay_previous_bits[word] & mask) != 0);
}
#endif

#if !BSP_SECTOR_RENDERER
static void commit_base_tile(u16 tile_index, const u32 *tile_rows);
#endif

#if (RAY_COL_STRIDE != 4) && (RAY_COL_STRIDE != 2)
#error "build_raycast_tilemap only implements the RAY_COL_STRIDE == 4 and == 2 packers"
#endif

#ifndef RENDERER_REFERENCE_PACKER
#define RENDERER_REFERENCE_PACKER 0
#endif

#if !BSP_SECTOR_RENDERER
#if RAY_COL_STRIDE == 4
// Pre-shade the 32-texel source column once per sampled ray column. The hot
// per-screen-pixel loop then performs only the vertical DDA lookup and one u16
// load. The 64-byte temporaries stay inside the packer's frame, avoiding both a
// large call-stack allocation and renderer-global cache state.
static void build_packed_wall_column(const WallColumnDescriptor *descriptor,
                                     u16 packed_texels[WALL_TEX_DIM]) {
    for (u16 tex_y = 0; tex_y < WALL_TEX_DIM; tex_y++) {
        const u8 texel = descriptor->shade_map[
            descriptor->texture[(tex_y * WALL_TEX_DIM) + descriptor->tex_x] & 0x0F];
        packed_texels[tex_y] = (u16)REP4[texel];
    }
}

static void build_raycast_tilemap(const RayColumn *columns,
                                   const RaySceneColors *scene_colors,
                                   u32 target[][8]) {
    // Each 8px-wide tile column maps to two cast columns (px 0 and 4), each
    // replicated 4x. Pre-shade each column's 32 texels once, then pack directly
    // into each tile row. Whole-tile ceiling/floor bands skip per-row wall
    // sampling, and the two 120-entry u16 strips are gone: the packer writes
    // each output u32 exactly once instead of building and re-reading a
    // full-height temporary (~19 KB of intermediate traffic per base frame).
    const u16 ceiling_packed = (u16)REP4[scene_colors->ceiling_color];
    const u16 floor_packed = (u16)REP4[scene_colors->floor_color];
    const u32 ceiling_row = ((u32)ceiling_packed << 16) | ceiling_packed;
    const u32 floor_row = ((u32)floor_packed << 16) | floor_packed;

    for (u16 tile_x = 0; tile_x < VIEW_TILE_W; tile_x++) {
        const u16 base_col = (u16)(tile_x * 8);
        const WallColumnDescriptor column_a = describe_wall_column(&columns[base_col]);
        const WallColumnDescriptor column_b = describe_wall_column(&columns[base_col + 4]);

        u16 packed_texels_a[WALL_TEX_DIM];
        u16 packed_texels_b[WALL_TEX_DIM];
        build_packed_wall_column(&column_a, packed_texels_a);
        build_packed_wall_column(&column_b, packed_texels_b);

        for (u16 tile_y = 0; tile_y < VIEW_TILE_H; tile_y++) {
            const u16 tile_index = (u16)((tile_y * VIEW_TILE_W) + tile_x);
            const u16 pixel_y = (u16)(tile_y * 8);
            u32 *tile = target[tile_index];

            if ((pixel_y + 7) < column_a.top && (pixel_y + 7) < column_b.top) {
                // Whole-tile ceiling: this 8px band lies entirely above both walls.
                for (u16 row = 0; row < 8; row++) {
                    tile[row] = ceiling_row;
                }
            } else if (pixel_y >= column_a.bottom && pixel_y >= column_b.bottom) {
                // Whole-tile floor: this 8px band lies entirely below both walls.
                for (u16 row = 0; row < 8; row++) {
                    tile[row] = floor_row;
                }
            } else {
                // Mixed band: resolve each of the 8 rows directly from the two
                // pre-shaded columns. Output is byte-identical to the old
                // strip-based packer (each row == (strip_a[py]<<16)|strip_b[py]).
                for (u16 row = 0; row < 8; row++) {
                    const u16 py = (u16)(pixel_y + row);
                    u16 val_a;
                    if (py < column_a.top) {
                        val_a = ceiling_packed;
                    } else if (py >= column_a.bottom) {
                        val_a = floor_packed;
                    } else {
                        val_a = packed_texels_a[
                            (column_a.vertical_samples[py - column_a.top] +
                             column_a.tex_y) & WALL_TEX_DIM_MASK];
                    }
                    u16 val_b;
                    if (py < column_b.top) {
                        val_b = ceiling_packed;
                    } else if (py >= column_b.bottom) {
                        val_b = floor_packed;
                    } else {
                        val_b = packed_texels_b[
                            (column_b.vertical_samples[py - column_b.top] +
                             column_b.tex_y) & WALL_TEX_DIM_MASK];
                    }
                    tile[row] = ((u32)val_a << 16) | val_b;
                }
            }
            commit_base_tile(tile_index, tile);
        }
    }
}
#else /* RAY_COL_STRIDE == 2 */
static void build_raycast_tilemap(const RayColumn *columns,
                                  const RaySceneColors *scene_colors,
                                  u32 target[][8]) {
    // Each 8px-wide tile column maps to four cast columns (px 0, 2, 4, 6), each
    // replicated 2x -> twice the horizontal detail of the stride-4 packer at the
    // same tile count / DMA cost. Describe each column once and pack MSB-first.
    for (u16 tile_x = 0; tile_x < VIEW_TILE_W; tile_x++) {
        const u16 base_col = (u16)(tile_x * 8);
        const WallColumnDescriptor column_a = describe_wall_column(&columns[base_col]);
        const WallColumnDescriptor column_b = describe_wall_column(&columns[base_col + 2]);
        const WallColumnDescriptor column_c = describe_wall_column(&columns[base_col + 4]);
        const WallColumnDescriptor column_d = describe_wall_column(&columns[base_col + 6]);
        const u32 ceiling_row = pack_flat_row(scene_colors->ceiling_color);
        const u32 floor_row = pack_flat_row(scene_colors->floor_color);

        for (u16 tile_y = 0; tile_y < VIEW_TILE_H; tile_y++) {
            const u16 tile_index = (u16)((tile_y * VIEW_TILE_W) + tile_x);
            u16 pixel_y = (u16)(tile_y * 8);

            if (((pixel_y + 7) < column_a.top) && ((pixel_y + 7) < column_b.top) &&
                ((pixel_y + 7) < column_c.top) && ((pixel_y + 7) < column_d.top)) {
                for (u16 row = 0; row < 8; row++) {
                    target[tile_index][row] = ceiling_row;
                }
                continue;
            }

            if ((pixel_y >= column_a.bottom) && (pixel_y >= column_b.bottom) &&
                (pixel_y >= column_c.bottom) && (pixel_y >= column_d.bottom)) {
                for (u16 row = 0; row < 8; row++) {
                    target[tile_index][row] = floor_row;
                }
                continue;
            }

            for (u16 row = 0; row < 8; row++, pixel_y++) {
                target[tile_index][row] =
                    (REP2[sample_wall_descriptor(&column_a, scene_colors, pixel_y)] << 24) |
                    (REP2[sample_wall_descriptor(&column_b, scene_colors, pixel_y)] << 16) |
                    (REP2[sample_wall_descriptor(&column_c, scene_colors, pixel_y)] << 8) |
                    REP2[sample_wall_descriptor(&column_d, scene_colors, pixel_y)];
            }
            commit_base_tile(tile_index, target[tile_index]);
        }
    }
}
#endif
#endif

#if RENDERER_REFERENCE_PACKER
// Test-only implementation of the previous strip-based packer. A validation
// build can compare this buffer with the direct descriptor packer; release builds
// compile it out, so it consumes neither ROM nor work RAM.
static void build_column_strip_reference(const RayColumn *column,
                                         const RaySceneColors *scene_colors,
                                         u8 *strip) {
    const u16 wall_h = column->height;
    const u16 top = (u16)((VIEW_PIXEL_H - wall_h) / 2);
    const u16 bottom = (u16)(top + wall_h);
    const u8 tid = (u8)((column->texture_id < FREEDOOM_WALL_TEXTURE_COUNT) ?
                            column->texture_id : MEGALDOOM_TEX_FALLBACK);
    const u8 (*tex)[WALL_TEX_DIM] = FREEDOOM_WALL_TEXTURES[tid];
    u16 fog_level = (u16)(column->depth >> FOG_SHIFT) + (column->shade ? 1u : 0u);
    if (fog_level > (SHADE_LEVELS - 1)) {
        fog_level = SHADE_LEVELS - 1;
    }
    const u8 *shade_map = g_shade_luts[fog_level];
    const u8 *ty_table = MEGALDOOM_WALL_TEX_Y_BY_HEIGHT[wall_h];
    const u8 tex_x = (u8)(column->tex_x & WALL_TEX_DIM_MASK);
    u16 y = 0;

    for (; y < top; y++) {
        strip[y] = scene_colors->ceiling_color;
    }
    for (; y < bottom; y++) {
        strip[y] = shade_map[tex[(ty_table[y - top] + column->tex_y) & WALL_TEX_DIM_MASK]
                                [tex_x] & 0x0F];
    }
    for (; y < VIEW_PIXEL_H; y++) {
        strip[y] = scene_colors->floor_color;
    }
}

#if RAY_COL_STRIDE == 2
static void build_raycast_tilemap_reference(const RayColumn *columns,
                                            const RaySceneColors *scene_colors,
                                            u32 target[][8]) {
    u8 strip_a[VIEW_PIXEL_H];
    u8 strip_b[VIEW_PIXEL_H];
    u8 strip_c[VIEW_PIXEL_H];
    u8 strip_d[VIEW_PIXEL_H];

    for (u16 tile_x = 0; tile_x < VIEW_TILE_W; tile_x++) {
        const u16 base_col = (u16)(tile_x * 8);
        build_column_strip_reference(&columns[base_col], scene_colors, strip_a);
        build_column_strip_reference(&columns[base_col + 2], scene_colors, strip_b);
        build_column_strip_reference(&columns[base_col + 4], scene_colors, strip_c);
        build_column_strip_reference(&columns[base_col + 6], scene_colors, strip_d);

        for (u16 tile_y = 0; tile_y < VIEW_TILE_H; tile_y++) {
            const u16 tile_index = (u16)((tile_y * VIEW_TILE_W) + tile_x);
            u16 pixel_y = (u16)(tile_y * 8);
            for (u16 row = 0; row < 8; row++, pixel_y++) {
                target[tile_index][row] =
                    (REP2[strip_a[pixel_y]] << 24) | (REP2[strip_b[pixel_y]] << 16) |
                    (REP2[strip_c[pixel_y]] << 8) | REP2[strip_d[pixel_y]];
            }
        }
    }
}
#else /* RAY_COL_STRIDE == 4 */
static void build_raycast_tilemap_reference(const RayColumn *columns,
                                            const RaySceneColors *scene_colors,
                                            u32 target[][8]) {
    u8 strip_a[VIEW_PIXEL_H];
    u8 strip_b[VIEW_PIXEL_H];

    for (u16 tile_x = 0; tile_x < VIEW_TILE_W; tile_x++) {
        const u16 base_col = (u16)(tile_x * 8);
        build_column_strip_reference(&columns[base_col], scene_colors, strip_a);
        build_column_strip_reference(&columns[base_col + 4], scene_colors, strip_b);

        for (u16 tile_y = 0; tile_y < VIEW_TILE_H; tile_y++) {
            const u16 tile_index = (u16)((tile_y * VIEW_TILE_W) + tile_x);
            u16 pixel_y = (u16)(tile_y * 8);
            for (u16 row = 0; row < 8; row++, pixel_y++) {
                target[tile_index][row] =
                    (REP4[strip_a[pixel_y]] << 16) | REP4[strip_b[pixel_y]];
            }
        }
    }
}
#endif

static u32 g_reference_view_tiles[VIEW_TILE_COUNT][8];
#endif

// Exact 32/16=32 unsigned division using two DIVU.W steps (base-65536 long
// division). Produces the same integer quotient as the '/' operator, so it can
// replace the slow compiler-emitted 32-bit unsigned-division helper for
// quotients that exceed 16 bits (e.g. Q16 texture stepping for tall sprites).
static u32 divu32_16_exact(u32 numerator, u16 denominator) {
    const u16 high = (u16)(numerator >> 16);

    // Quotient fits in 16 bits: one DIVU.W is exact.
    if (high < denominator) {
        return divu(numerator, denominator);
    }

    const u16 quotient_high = divu(high, denominator);
    const u16 remainder_high =
        (u16)(high - ((u32)quotient_high * denominator));

    const u32 low_numerator =
        ((u32)remainder_high << 16) | (numerator & 0xFFFFu);
    const u16 quotient_low = divu(low_numerator, denominator);

    return ((u32)quotient_high << 16) | quotient_low;
}

// Draw projected billboards object-by-object. Projection, texture selection,
// colour remapping and the vertical DDA are all shared by the object's columns;
// the per-column loop only resolves horizontal texture position and writes pixels.
static void draw_projected_billboards(const RayColumn *columns,
                                      const ProjectedBillboard *objects,
                                      u16 object_count) {
    for (u16 i = 0; i < object_count; i++) {
        const ProjectedBillboard *object = &objects[i];
        const s16 height = (s16)(object->bottom - object->top + 1);
        const s16 width = (s16)(object->right - object->left + 1);
        const BillboardTex tex = get_billboard_texture(object->visual_id, object->frame);
        const u8 *lut = MEGALDOOM_BILLBOARD_REMAP[
            (object->visual_id < 6) ? object->visual_id : BILLBOARD_VISUAL_BONUS];
        u8 tex_y_by_screen_row[VIEW_PIXEL_H];
        s16 y0 = object->top;
        s16 y1 = object->bottom;
        u32 tex_x_acc = 0;
        u32 tex_x_step;
        s16 last_marked_tile_x = -1;
        u16 opaque_tile_rows = 0;

        if ((height <= 0) || (width <= 0)) {
            continue;
        }
        // tex_x_step = (tex.w<<8)/width; width <= 2*half_w+1 (<= 25) and tex.w
        // <= 24, so the quotient (<= 6144) fits u16 and DIVU.W is exact,
        // avoiding the slow 32-bit unsigned-division helper.
        tex_x_step = divu((u32)tex.w << 8, (u16)width);

        if (y0 < 0) {
            y0 = 0;
        }
        if (y1 >= VIEW_PIXEL_H) {
            y1 = VIEW_PIXEL_H - 1;
        }
        if (y0 > y1) {
            continue;
        }

        // Approximate floor(rel_y * tex.h / height) with one setup divide and a
        // fixed-point DDA instead of a 68k divide for every visible sprite row.
        u32 tex_y_acc = ((u32)(y0 - object->top) * tex.h) << 16;
        // tex_y_step = (tex.h<<16)/height is Q16; for a 48px enemy the numerator
        // is 3,145,728 and small projected heights make the quotient exceed 16
        // bits, so a single DIVU.W cannot hold it. The two-stage divider produces
        // the exact same quotient as '/'.
        const u32 tex_y_step = divu32_16_exact((u32)tex.h << 16, (u16)height);
        for (s16 y = y0; y <= y1; y++) {
            u8 tex_y = (u8)(tex_y_acc >> 16);
            if (tex_y >= tex.h) {
                tex_y = (u8)(tex.h - 1);
            }
            tex_y_by_screen_row[y] = tex_y;
            tex_y_acc += tex_y_step;
        }

        for (s16 col = object->left; col <= object->right; col++) {
            u8 tex_x = (u8)(tex_x_acc >> 8);
            tex_x_acc += tex_x_step;

            if ((col < 0) || (col >= RAY_VIEW_COLS)) {
                continue;
            }
            const u16 wall_col = (u16)(col & ~(RAY_COL_STRIDE - 1));
            // The base image repeats the sampled wall column across this exact
            // block, so billboard depth must use that same sample.
#if !BSP_SECTOR_RENDERER
            const u16 wall_depth = columns[wall_col].depth;
            if (object->depth >= wall_depth) continue;
#endif

            if (tex_x >= tex.w) {
                tex_x = (u8)(tex.w - 1);
            }
            const u16 tile_x = (u16)(col >> 3);
            const u16 shift = (u16)((7 - (col & 7)) * 4);
            const u32 keep_mask = ~((u32)0x0F << shift);
#if BSP_SECTOR_RENDERER
            const u16 sample_x = (u16)(wall_col / RAY_COL_STRIDE);
#endif

            if ((s16)tile_x != last_marked_tile_x) {
                last_marked_tile_x = (s16)tile_x;
                opaque_tile_rows = 0;
            }

            u16 y = (u16)y0;
            const u16 y_end = (u16)(y1 + 1);
            while (y < y_end) {
                const u16 tile_y = (u16)(y >> 3);
                u32 *tile = g_view_tiles[(tile_y * VIEW_TILE_W) + tile_x];
#if BSP_SECTOR_RENDERER
                const u16 *depth_rows = bsp_sector_depth_block(sample_x, tile_y);
#endif
                const u16 next_tile_y = (u16)((tile_y + 1) * 8);
                const u16 stop = (next_tile_y < y_end) ? next_tile_y : y_end;

                for (; y < stop; y++) {
                    const u8 texel = lut[tex.pixels[(tex_y_by_screen_row[y] * tex.w) + tex_x] & 0x0F];
#if BSP_SECTOR_RENDERER
                    const bool depth_visible = object->depth < depth_rows[y & 7];
#else
                    const bool depth_visible = TRUE;
#endif
                    if (texel != 0 && depth_visible) {
                        const u16 row_y = (u16)(y & 7);
                        const u16 tile_bit = (u16)1u << tile_y;
                        if ((opaque_tile_rows & tile_bit) == 0) {
                            renderer_mark_overlay_tile(
                                (u16)(tile_y * VIEW_TILE_W + tile_x));
                            opaque_tile_rows |= tile_bit;
                        }
                        tile[row_y] = (tile[row_y] & keep_mask) | ((u32)texel << shift);
                    }
                }
            }
        }
    }
}

// Draw the static weapon overlay from its precomposed per-tile-row ops (built
// once in the asset generator). Each op is a single u32 read-modify-write,
// so an idle frame costs a few hundred RMWs instead of one per-pixel test across
// the weapon draw box (FREEDOOM_WEAPON_DRAW_W x FREEDOOM_WEAPON_DRAW_H).
// The clear masks are generated alongside the values (static const ROM data),
// so the hot loop is just *dst = (*dst & ~clear) | val with no per-op mask
// reconstruction.
static void draw_weapon_overlay(bool flash) {
    const u16 v = flash ? 1 : 0;
    const u16 *dst_idx = v ? MEGALDOOM_WEAPON_DST_FIRE : MEGALDOOM_WEAPON_DST_IDLE;
    const u32 *values = v ? MEGALDOOM_WEAPON_VALUE_FIRE : MEGALDOOM_WEAPON_VALUE_IDLE;
    const u32 *clear_masks = v ? MEGALDOOM_WEAPON_CLEAR_FIRE : MEGALDOOM_WEAPON_CLEAR_IDLE;
    const u16 count = MEGALDOOM_WEAPON_OP_COUNT[v];
    u32 *base = &g_view_tiles[0][0];
    u16 last_marked_tile = 0xFFFF;

    for (u16 i = 0; i < count; i++) {
        u32 *dst = base + dst_idx[i];
        const u16 tile_index = (u16)(dst_idx[i] / 8);

        if (tile_index != last_marked_tile) {
            renderer_mark_overlay_tile(tile_index);
            last_marked_tile = tile_index;
        }
        *dst = (*dst & ~clear_masks[i]) | values[i];
    }
}

static void draw_overlay_ops(const MegalDoomOverlayRowOp *ops, u16 count) {
    u32 *base = &g_view_tiles[0][0];

    for (u16 i = 0; i < count; i++) {
        const MegalDoomOverlayRowOp *op = &ops[i];
        u32 *dst = base + op->dst;
        renderer_mark_overlay_tile((u16)(op->dst / 8));
        *dst = (*dst & ~op->clear_mask) | op->value;
    }
}

static bool view_bank_tile_is_dirty(u16 bank, u16 tile) {
    const u16 word = (u16)(tile >> 5);
    const u32 mask = (u32)1u << (tile & 31);
    return (bool)((g_view_bank_dirty_bits[bank][word] & mask) != 0);
}

static void clear_view_bank_dirty_bits(u16 bank) {
    for (u16 word = 0; word < VIEW_DIRTY_WORD_COUNT; word++) {
        g_view_bank_dirty_bits[bank][word] = 0;
    }
    g_view_bank_dirty_count[bank] = 0;
}

static u16 count_view_bank_dirty_runs(u16 bank) {
    u16 runs = 0;
    bool in_run = FALSE;

    for (u16 tile = 0; tile < VIEW_TILE_COUNT; tile++) {
        const bool dirty = view_bank_tile_is_dirty(bank, tile);
        if (dirty && !in_run) {
            runs++;
        }
        in_run = dirty;
    }

    return runs;
}

// Predict the number of VDP_loadTileData calls made by the partial uploader.
// Disjoint dirty runs each need a command, and a run crossing the per-vblank
// tile budget needs another command after the uploader's VSync split.
static u16 count_partial_view_bank_commands(u16 bank,
                                            bool split_across_vblanks) {
    u16 commands = 0;
    u16 batch_tiles = 0;

    for (u16 tile = 0; tile < VIEW_TILE_COUNT;) {
        if (!view_bank_tile_is_dirty(bank, tile)) {
            tile++;
            continue;
        }

        u16 first = tile;
        while ((tile < VIEW_TILE_COUNT) && view_bank_tile_is_dirty(bank, tile)) {
            tile++;
        }

        u16 remaining = (u16)(tile - first);
        while (remaining > 0) {
            if (split_across_vblanks &&
                (batch_tiles == VIEW_DMA_TILES_PER_VBLANK)) {
                batch_tiles = 0;
            }

            u16 count = remaining;
            if (split_across_vblanks) {
                const u16 available =
                    (u16)(VIEW_DMA_TILES_PER_VBLANK - batch_tiles);
                if (count > available) {
                    count = available;
                }
            }

            commands++;
            remaining = (u16)(remaining - count);
            batch_tiles = (u16)(batch_tiles + count);
        }
    }

    return commands;
}

static void load_view_tile_run(u16 vram_base, u16 first, u16 count) {
#if DEBUG_PERF
    const u32 issue_start = getSubTick();
#endif
    VDP_loadTileData((const u32 *)&g_view_tiles[first][0],
                     (u16)(vram_base + first),
                     count,
                     DMA);
#if DEBUG_PERF
    s_debug_upload_prepare_subticks += getSubTick() - issue_start;
    s_debug_upload_tiles = (u16)(s_debug_upload_tiles + count);
    s_debug_upload_runs++;
#endif
}

#if DEBUG_PERF
// Wrap the blocking DMA-completion / VBlank waits so CPU time spent issuing
// DMA, time spent blocked inside VDP_waitDMACompletion(), and VBlanks
// deliberately awaited by the uploader are accounted separately. Patch 5 will
// remove the internal VDP_waitVSync() calls; until then this measures them.
static void dbg_wait_dma(void) {
    const u32 t = getSubTick();
    VDP_waitDMACompletion();
    s_debug_dma_wait_subticks += getSubTick() - t;
}
static void dbg_wait_vsync_upload(void) {
    VDP_waitVSync();
    s_debug_awaited_vblanks_in_upload++;
}
#else
#define dbg_wait_dma()            VDP_waitDMACompletion()
#define dbg_wait_vsync_upload()   VDP_waitVSync()
#endif

static void upload_full_view_bank(u16 vram_base, bool split_across_vblanks) {
    if (!split_across_vblanks) {
        load_view_tile_run(vram_base, 0, VIEW_TILE_COUNT);
        dbg_wait_dma();
        return;
    }

    load_view_tile_run(vram_base, 0, VIEW_DMA_TILES_PER_VBLANK);
    dbg_wait_dma();
    dbg_wait_vsync_upload();
    load_view_tile_run(vram_base,
                       VIEW_DMA_TILES_PER_VBLANK,
                       (u16)(VIEW_TILE_COUNT - VIEW_DMA_TILES_PER_VBLANK));
    dbg_wait_dma();
}

static void upload_partial_view_bank(u16 bank,
                                     u16 vram_base,
                                     bool split_across_vblanks) {
    u16 batch_tiles = 0;

    for (u16 tile = 0; tile < VIEW_TILE_COUNT;) {
        if (!view_bank_tile_is_dirty(bank, tile)) {
            tile++;
            continue;
        }

        u16 first = tile;
        while ((tile < VIEW_TILE_COUNT) && view_bank_tile_is_dirty(bank, tile)) {
            tile++;
        }

        u16 remaining = (u16)(tile - first);
        while (remaining > 0) {
            if (split_across_vblanks &&
                (batch_tiles == VIEW_DMA_TILES_PER_VBLANK)) {
                dbg_wait_dma();
                dbg_wait_vsync_upload();
                batch_tiles = 0;
            }

            u16 count = remaining;
            if (split_across_vblanks) {
                const u16 available =
                    (u16)(VIEW_DMA_TILES_PER_VBLANK - batch_tiles);
                if (count > available) {
                    count = available;
                }
            }

            load_view_tile_run(vram_base, first, count);
            first = (u16)(first + count);
            remaining = (u16)(remaining - count);
            batch_tiles = (u16)(batch_tiles + count);
        }
    }

    dbg_wait_dma();
}

static bool upload_view_bank(u16 bank, bool split_across_vblanks) {
    const u16 dirty_count = g_view_bank_dirty_count[bank];
    const u16 run_count = count_view_bank_dirty_runs(bank);
    bool full_upload;
    const u16 vram_base = (u16)(VIEW_TILE_BASE + (bank * VIEW_TILE_COUNT));

    // A base redraw targets the inactive bank, so using both vblanks in the
    // fixed 30fps budget is safe. Prefer the full two-command upload only when
    // it issues fewer DMA commands than the exact partial-upload schedule.
    // Equal-cost contiguous changes remain partial and transfer fewer tiles.
    if (split_across_vblanks && dirty_count > 0) {
        const u16 partial_command_count =
            count_partial_view_bank_commands(bank, TRUE);
        const u16 full_command_count =
            (VIEW_TILE_COUNT > VIEW_DMA_TILES_PER_VBLANK) ? 2 : 1;
        full_upload = (bool)(full_command_count < partial_command_count);
    } else {
        // Overlay-only updates target the displayed bank. Preserve their
        // existing size/fragmentation policy; the two-vblank inactive-bank
        // optimization must never broaden an active-bank DMA.
        full_upload = (bool)((dirty_count >= VIEW_DIRTY_FULL_THRESHOLD) ||
                             (run_count > VIEW_DIRTY_MAX_RUNS));
    }

#if DEBUG_PERF
    s_debug_upload_dirty_tiles = dirty_count;
    s_debug_upload_tiles = 0;
    s_debug_upload_runs = 0;
    s_debug_upload_bank = bank;
    s_debug_upload_full = full_upload;
    s_debug_upload_swap = split_across_vblanks;
#endif

    if (dirty_count == 0) {
        return FALSE;
    }

    // Decide partial versus full before issuing any DMA. The previous path only
    // discovered excessive fragmentation after already scheduling partial runs.
    if (full_upload) {
        upload_full_view_bank(vram_base, split_across_vblanks);
    } else {
        upload_partial_view_bank(bank, vram_base, split_across_vblanks);
    }

    // No bank is declared current until every DMA targeting it has finished.
    clear_view_bank_dirty_bits(bank);
    return TRUE;
}

static void upload_view_tilemap(void) {
    if (g_upload_requires_bank_swap) {
        const u16 next_bank = (u16)(g_view_vram_bank ^ 1);
        if (upload_view_bank(next_bank, TRUE)) {
            renderer_set_view_vram_bank(next_bank);
        }
        g_upload_requires_bank_swap = FALSE;
    } else {
        upload_view_bank(g_view_vram_bank, FALSE);
    }
}

static void clear_compass_tilemap(void) {
    for (u16 i = 0; i < (COMPASS_W * COMPASS_H); i++) {
        g_compass_tilemap[i] = TILE_ATTR_FULL(PAL0, FALSE, FALSE, FALSE, PAIR_TILE_BASE);
    }
}

static void set_compass_tile(s16 x, s16 y, u8 color) {
    if ((x < 0) || (y < 0) || (x >= COMPASS_W) || (y >= COMPASS_H)) {
        return;
    }

    g_compass_tilemap[(y * COMPASS_W) + x] =
        TILE_ATTR_FULL(PAL0, FALSE, FALSE, FALSE, PAIR_TILE_BASE + ((color & 0x0F) << 4) + (color & 0x0F));
}

static void build_compass_tilemap(u16 angle) {
    if (angle == g_last_compass_angle) {
        return;
    }

    const s16 vx = fx_cos(angle);
    const s16 vy = fx_sin(angle);
    const s16 dot_x = (s16)(2 + ((vx * 2) >> FX_SHIFT));
    const s16 dot_y = (s16)(2 + ((vy * 2) >> FX_SHIFT));

    clear_compass_tilemap();
    set_compass_tile(2, 0, 7);
    set_compass_tile(0, 2, 7);
    set_compass_tile(2, 2, 11);
    set_compass_tile(4, 2, 7);
    set_compass_tile(2, 4, 7);
    set_compass_tile(dot_x, dot_y, 13);
    g_last_compass_angle = angle;
    g_compass_dirty = TRUE;
}

static void restore_previous_overlay_tiles(void) {
    for (u16 tile = 0; tile < VIEW_TILE_COUNT; tile++) {
        const u16 word = (u16)(tile >> 5);
        const u32 mask = (u32)1u << (tile & 31);
        if ((g_overlay_previous_bits[word] & mask) == 0) {
            continue;
        }

        for (u16 row = 0; row < 8; row++) {
            g_view_tiles[tile][row] = g_base_view_tiles[tile][row];
        }
        renderer_mark_tile_dirty(tile);
    }
}

static void clear_overlay_bits(void) {
    for (u16 i = 0; i < VIEW_DIRTY_WORD_COUNT; i++) {
        g_overlay_current_bits[i] = 0;
    }
}

static void finish_overlay_bits(void) {
    for (u16 i = 0; i < VIEW_DIRTY_WORD_COUNT; i++) {
        g_overlay_previous_bits[i] = g_overlay_current_bits[i];
    }
}

#if !BSP_SECTOR_RENDERER
static void commit_base_tile(u16 tile_index, const u32 *tile_rows) {
    // Fold the 8 row comparisons into one difference accumulator instead of
    // branching per row, and copy the new rows in the same pass (writing an
    // unchanged value is a no-op but removes the per-row branch and the re-read
    // of just-written data). The tile is dirty if any row changed, an overlay had
    // previously touched it (so the erase must re-upload the base), or this is
    // the first base build (g_base_tiles_valid == FALSE forces a full upload).
    u32 difference =
        (overlay_previously_touched(tile_index) || !g_base_tiles_valid) ? 1u : 0u;
    u32 *base_rows = g_base_view_tiles[tile_index];

    for (u16 row = 0; row < 8; row++) {
        const u32 row_data = tile_rows[row];
        difference |= (base_rows[row] ^ row_data);
        base_rows[row] = row_data;
    }

    if (difference != 0) {
        renderer_mark_tile_dirty(tile_index);
    }
}
#endif

static void upload_compass_tilemap(void) {
    if (!g_compass_dirty) {
        return;
    }

    VDP_setTileMapDataRect(BG_B,
                           g_compass_tilemap,
                           COMPASS_X,
                           COMPASS_Y,
                           COMPASS_W,
                           COMPASS_H,
                           COMPASS_W,
                           CPU);
    g_compass_dirty = FALSE;
}

void renderer_render_scene(const RayColumn *columns,
                           const PlayerState *player,
                           const RaySceneColors *scene_colors,
                           bool base_dirty,
                           bool weapon_flash,
                           bool damage_flash,
                           bool low_health_warning) {
#if DEBUG_PERF
    u32 stage_start = getSubTick();
    renderer_reset_frame_modified();
#endif
    ProjectedBillboard objects[BILLBOARD_MAX_PROJECTED_OBJECTS];
    const u16 object_count = billboard_project_scene(
        player, columns, objects, BILLBOARD_MAX_PROJECTED_OBJECTS);

    if (base_dirty) {
        g_upload_requires_bank_swap = TRUE;
#if DEBUG_PERF
        stage_start = getSubTick();
#endif
#if BSP_SECTOR_RENDERER
        mark_all_view_banks_dirty();
#else
        build_raycast_tilemap(columns, scene_colors, g_view_tiles);
#endif
#if RENDERER_REFERENCE_PACKER
        build_raycast_tilemap_reference(columns, scene_colors, g_reference_view_tiles);
        for (u16 tile = 0; tile < VIEW_TILE_COUNT; tile++) {
            for (u16 row = 0; row < 8; row++) {
                if (g_view_tiles[tile][row] != g_reference_view_tiles[tile][row]) {
                    // Keep the reference output in validation builds so a visual
                    // comparison remains safe even when the optimized path differs.
                    for (u16 copy_tile = 0; copy_tile < VIEW_TILE_COUNT; copy_tile++) {
                        for (u16 copy_row = 0; copy_row < 8; copy_row++) {
                            g_view_tiles[copy_tile][copy_row] = g_reference_view_tiles[copy_tile][copy_row];
                        }
                    }
                    tile = VIEW_TILE_COUNT;
                    break;
                }
            }
        }
#endif
        g_base_tiles_valid = TRUE;
        for (u16 i = 0; i < VIEW_DIRTY_WORD_COUNT; i++) {
            g_overlay_previous_bits[i] = 0;
        }
#if DEBUG_PERF
        s_debug_pack_subticks = getSubTick() - stage_start;
        stage_start = getSubTick();
#endif
    } else {
        restore_previous_overlay_tiles();
#if DEBUG_PERF
        s_debug_pack_subticks = 0;
#endif
    }

    clear_overlay_bits();
#if DEBUG_PERF
    const u32 overlay_stage = getSubTick();
    const u32 bb_start = overlay_stage;
#endif
    draw_projected_billboards(columns, objects, object_count);
#if DEBUG_PERF
    s_debug_billboard_subticks = getSubTick() - bb_start;
    const u32 wpn_start = getSubTick();
#endif
    draw_weapon_overlay(weapon_flash);
    if (damage_flash) {
        draw_overlay_ops(MEGALDOOM_DAMAGE_OVERLAY_OPS, MEGALDOOM_OVERLAY_OP_COUNT[0]);
    } else if (low_health_warning) {
        draw_overlay_ops(MEGALDOOM_LOW_HEALTH_OVERLAY_OPS, MEGALDOOM_OVERLAY_OP_COUNT[1]);
    }
    build_compass_tilemap(player->angle);
    finish_overlay_bits();
#if DEBUG_PERF
    s_debug_weapon_overlay_subticks = getSubTick() - wpn_start;
    s_debug_overlay_subticks = getSubTick() - overlay_stage;
#endif
}

// Push the frame built by renderer_render_scene to VRAM. Call this right after a
// VDP_waitVSync so the ~9.6KB view-tile DMA runs at the fast vblank rate instead
// of stalling the CPU mid active-display (its old call site).
#if DEBUG_PERF
static void draw_upload_debug_stats(void) {
    char text[44];

    // Row 1 — VBlank cadence (the key 2-vs-3 VBlank indicator). V = total
    // VBlanks consumed by the last iteration (set one frame late from main.c);
    // Vup = VBlanks deliberately awaited inside the uploader (Patch 5 drives
    // this to 0). mode = size [N]one/[P]artial/[F]ull + bank [A]ctive/[I]nactive.
    const char size_c = (s_debug_upload_dirty_tiles == 0) ? 'N'
                        : (s_debug_upload_full ? 'F' : 'P');
    const char bank_c = s_debug_upload_swap ? 'I' : 'A';
    sprintf(text, "V=%u Vup=%u %c-%c",
            (unsigned int)s_debug_total_vblanks,
            (unsigned int)s_debug_awaited_vblanks_in_upload,
            size_c, bank_c);
    VDP_drawTextFill(text, 0, 1, 24);

    // Row 2 — CPU cost before the first VBlank, decomposed (subticks).
    // C=cast, P=pack, B=billboard, W=weapon+damage/low-health overlays.
    sprintf(text, "C=%04lu P=%04lu B=%04lu W=%04lu",
            (unsigned long)s_debug_cast_subticks,
            (unsigned long)s_debug_pack_subticks,
            (unsigned long)s_debug_billboard_subticks,
            (unsigned long)s_debug_weapon_overlay_subticks);
    VDP_drawTextFill(text, 0, 2, 32);

    // Row 3 — DMA work. D=dirty tiles, U=uploaded tiles, R=DMA runs,
    // M=tiles modified (distinct CPU writes this frame), Up=DMA-issue CPU
    // subticks, Ud=time blocked in VDP_waitDMACompletion().
    sprintf(text, "D=%03u U=%03u R=%02u M=%03u Up=%04lu Ud=%04lu",
            (unsigned int)s_debug_upload_dirty_tiles,
            (unsigned int)s_debug_upload_tiles,
            (unsigned int)s_debug_upload_runs,
            (unsigned int)renderer_get_frame_modified_count(),
            (unsigned long)s_debug_upload_prepare_subticks,
            (unsigned long)s_debug_dma_wait_subticks);
    VDP_drawTextFill(text, 0, 3, 40);

    // Row 4 — BSP traversal. N=nodes visited, R=cheap rejects (half-plane,
    // no division), P=boxes projected (4 divs), F=near-plane fallbacks,
    // S=segs tested/drawn.
    sprintf(text, "N=%03u R=%03u P=%03u F=%03u S=%03u/%03u",
            (unsigned int)bsp_get_debug_nodes_visited(),
            (unsigned int)bsp_get_debug_boxes_rejected_cheap(),
            (unsigned int)bsp_get_debug_boxes_projected(),
            (unsigned int)bsp_get_debug_near_fallbacks(),
            (unsigned int)bsp_get_debug_segments_tested(),
            (unsigned int)bsp_get_debug_segments_drawn());
    VDP_drawTextFill(text, 0, 4, 40);

    // Row 5 — gameplay spatial queries and lazy BSP ordering. PC/EC are player
    // and enemy collision time, L is LOS time, K/LK are tested candidates, and
    // Sc is time computing node sides not yet cached for this position.
#if BSP_SECTOR_RENDERER
    sprintf(text, "St=%04lu Ss=%04lu Sr=%04lu",
            (unsigned long)bsp_sector_get_debug_transform_subticks(),
            (unsigned long)bsp_sector_get_debug_setup_subticks(),
            (unsigned long)bsp_sector_get_debug_raster_subticks());
#else
    sprintf(text, "PC=%03lu EC=%03lu L=%03lu K=%03u LK=%03u Sc=%03lu",
            (unsigned long)bsp_get_debug_player_collision_subticks(),
            (unsigned long)bsp_get_debug_enemy_collision_subticks(),
            (unsigned long)bsp_get_debug_los_subticks(),
            (unsigned int)bsp_get_debug_collision_candidates(),
            (unsigned int)bsp_get_debug_los_candidates(),
            (unsigned long)bsp_get_debug_side_cache_subticks());
#endif
    VDP_drawTextFill(text, 0, 5, 40);

#if BSP_SECTOR_RENDERER
    sprintf(text, "Sf=%04lu Sw=%04lu Sp=%04lu",
            (unsigned long)bsp_sector_get_debug_flat_subticks(),
            (unsigned long)bsp_sector_get_debug_wall_subticks(),
            (unsigned long)bsp_sector_get_debug_floor_subticks());
    VDP_drawTextFill(text, 0, 6, 32);
#else
    sprintf(text, "O%02u C%02u W%02u D%02u A%02u H%02u M%02u",
            (unsigned int)billboard_get_active_count(),
            (unsigned int)billboard_get_debug_candidate_count(),
            (unsigned int)billboard_get_debug_occluded_count(),
            (unsigned int)billboard_get_debug_projected_count(),
            (unsigned int)billboard_get_debug_simulated_enemy_count(),
            (unsigned int)billboard_get_debug_visibility_cache_hits(),
            (unsigned int)billboard_get_debug_visibility_cache_misses());
    VDP_drawTextFill(text, 0, 6, 40);
#endif
}
#endif

void renderer_upload_scene(void) {
#if DEBUG_PERF
    // Reset the upload cost accumulators so each iteration's numbers are
    // independent. (dirty_tiles/full/swap are set inside upload_view_bank.)
    s_debug_upload_prepare_subticks = 0;
    s_debug_dma_wait_subticks = 0;
    s_debug_awaited_vblanks_in_upload = 0;
#endif
    upload_view_tilemap();
    upload_compass_tilemap();
#if DEBUG_PERF
    draw_upload_debug_stats();
#endif
}
