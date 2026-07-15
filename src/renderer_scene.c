#include "renderer_internal.h"
#include "renderer_perf.h"
#include "bsp_render.h"
#include "generated_assets.h"
#include "generated_billboard_assets.h"
#include "generated_hud_assets.h"
#include "generated_renderer_assets.h"
#include "player_controller.h"

// Flat billboard-texture descriptor. Storing the pixels as a plain const u8* (a
// [rows][cols] array decays cleanly) lets one draw loop sample sprites of any size:
// world sprites and enemies are 24x48. Index as pixels[y*w + x].
typedef struct {
    const u8 *pixels;
    u8 w;
    u8 h;
} BillboardTex;

#define PUFF_WALL_DEPTH_TOLERANCE 64

static bool billboard_depth_visible(const ProjectedBillboard *object,
                                    u16 wall_depth) {
    if (object->depth < wall_depth) return TRUE;
    // PUFF represents a hit on this wall plane. Ray depths vary across the
    // four-pixel rendered block, so allow only this transient a small tolerance
    // around the plane instead of letting the block hide its own impact.
    return (bool)((object->visual_id == BILLBOARD_VISUAL_PUFF) &&
                  ((u16)(object->depth - wall_depth) <= PUFF_WALL_DEPTH_TOLERANCE));
}

static BillboardTex get_billboard_texture(u8 visual_id, u8 frame) {
    switch (visual_id) {
        case BILLBOARD_VISUAL_DUMMY_DAMAGED:
        case BILLBOARD_VISUAL_DUMMY: {
            const u8 f = (frame < FREEDOOM_BILLBOARD_ENEMY_FRAME_COUNT) ? frame : 0;
            return (BillboardTex){(const u8 *)FREEDOOM_BILLBOARD_ENEMY_FRAMES[f],
                                  FREEDOOM_BILLBOARD_ENEMY_W, FREEDOOM_BILLBOARD_ENEMY_H};
        }
        case BILLBOARD_VISUAL_BARREL_EXPLODING: {
            const u8 f = (frame < FREEDOOM_BILLBOARD_BARREL_EXPLOSION_FRAME_COUNT) ? frame : 0;
            return (BillboardTex){(const u8 *)FREEDOOM_BILLBOARD_BARREL_EXPLOSION_FRAMES[f],
                                  FREEDOOM_BILLBOARD_BARREL_EXPLOSION_W,
                                  FREEDOOM_BILLBOARD_BARREL_EXPLOSION_H};
        }
        case BILLBOARD_VISUAL_PUFF: {
            const u8 f = (frame < FREEDOOM_BILLBOARD_PUFF_FRAME_COUNT) ? frame : 0;
            return (BillboardTex){(const u8 *)FREEDOOM_BILLBOARD_PUFF_FRAMES[f],
                                  FREEDOOM_BILLBOARD_PUFF_W, FREEDOOM_BILLBOARD_PUFF_H};
        }
        case BILLBOARD_VISUAL_BLOOD: {
            const u8 f = (frame < FREEDOOM_BILLBOARD_BLOOD_FRAME_COUNT) ? frame : 0;
            return (BillboardTex){(const u8 *)FREEDOOM_BILLBOARD_BLOOD_FRAMES[f],
                                  FREEDOOM_BILLBOARD_BLOOD_W, FREEDOOM_BILLBOARD_BLOOD_H};
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
static u16 g_last_compass_angle = 0xFFFF;
static bool g_upload_requires_bank_swap = FALSE;
static bool g_compass_dirty = TRUE;

typedef struct {
    bool pending;
    bool full;
    bool swap;
    u16 bank;
    u16 cursor;
} ViewUploadState;

static ViewUploadState g_view_upload;

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

static void clear_all_view_banks_dirty(void) {
    for (u16 bank = 0; bank < VIEW_BANK_COUNT; bank++) {
        for (u16 word = 0; word < VIEW_DIRTY_WORD_COUNT; word++) {
            g_view_bank_dirty_bits[bank][word] = 0;
        }
        g_view_bank_dirty_count[bank] = 0;
    }
}

// Pixel-replication table for the active stride (guarded so the unused one isn't
// compiled): REP4[c] == c*0x1111 spreads a colour across 4px (stride 4); REP2[c] ==
// c*0x11 spreads it across 2px (stride 2, four cast columns per 8px tile).
#if RAY_COL_STRIDE == 4
static const u32 REP4[16] = {
    0x0000, 0x1111, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666, 0x7777,
    0x8888, 0x9999, 0xAAAA, 0xBBBB, 0xCCCC, 0xDDDD, 0xEEEE, 0xFFFF,
};
static u16 g_packed_shade_luts[SHADE_LEVELS][16];

static void build_packed_shade_luts(void) {
    for (u16 level = 0; level < SHADE_LEVELS; level++) {
        for (u16 color = 0; color < 16; color++) {
            g_packed_shade_luts[level][color] =
                (u16)REP4[g_shade_luts[level][color]];
        }
    }
}
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

// Precomposed weapon overlay: the weapon bitmap is static per variant (idle /
// fire), so instead of re-testing ~72x54 pixels every redraw we bake, once at
// init, one (dst, clear_mask, value) op per affected tile-row. Drawing then
// becomes a handful of hundred u32 read-modify-writes: dst = (dst & ~clear) | val.
// Result matches the direct pixel composition (each nibble is written by
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
#if RAY_COL_STRIDE == 4
    build_packed_shade_luts();
#endif
    g_last_compass_angle = 0xFFFF;
    g_upload_requires_bank_swap = FALSE;
    g_compass_dirty = TRUE;
    g_view_upload = (ViewUploadState){FALSE, FALSE, FALSE, 0, 0};
    renderer_overlay_reset();
    clear_all_view_banks_dirty();
}

void renderer_invalidate_scene(void) {
    g_last_compass_angle = 0xFFFF;
    g_upload_requires_bank_swap = FALSE;
    g_compass_dirty = TRUE;
    renderer_overlay_reset();
    clear_all_view_banks_dirty();
    g_view_dirty_bank_mask = 0;
}

typedef struct {
    u16 top;
    u16 bottom;
    const u8 *texture;
    const u8 *shade_map;
#if RAY_COL_STRIDE == 4
    const u16 *packed_shade_map;
#endif
    const u8 *vertical_samples;
    u8 tex_x;
    u8 tex_y;
    u8 flags;
} WallColumnDescriptor;

static WallColumnDescriptor describe_textured_column(u16 wall_h,
                                                     u16 depth,
                                                     u8 texture_id,
                                                     u8 tex_x_value,
                                                     u8 tex_y_value,
                                                     u8 side_shade,
                                                     u8 flags) {
    const u16 top = (u16)((VIEW_PIXEL_H - wall_h) / 2);
    const u16 bottom = (u16)(top + wall_h);
    const u8 tid = (u8)((texture_id < FREEDOOM_WALL_TEXTURE_COUNT) ?
                            texture_id : MEGALDOOM_TEX_FALLBACK);
    const u8 (*tex)[WALL_TEX_DIM] = FREEDOOM_WALL_TEXTURES[tid];
    // Distance fog + side shading fold into one LUT selection per column: the fog
    // level grows with depth, and N/S ("shade") walls add one extra darkening step.
    // g_shade_luts[0] is the identity, so near front walls are unshaded; every level
    // maps 0 -> 0, preserving transparency, and the inner loop stays branch-free.
    u16 fog_level = (u16)(depth >> FOG_SHIFT) + (side_shade ? 1u : 0u);
    if (fog_level > (SHADE_LEVELS - 1)) {
        fog_level = SHADE_LEVELS - 1;
    }
    const u8 *shade_map = g_shade_luts[fog_level];
    const u8 *ty_table = MEGALDOOM_WALL_TEX_Y_BY_HEIGHT[wall_h];
    const u8 tex_x = (u8)(tex_x_value & WALL_TEX_DIM_MASK);

#if RAY_COL_STRIDE == 4
    return (WallColumnDescriptor){top, bottom, (const u8 *)tex, shade_map,
                                  g_packed_shade_luts[fog_level], ty_table,
                                  tex_x, tex_y_value, flags};
#else
    return (WallColumnDescriptor){top, bottom, (const u8 *)tex, shade_map, ty_table,
                                  tex_x, tex_y_value, flags};
#endif
}

static WallColumnDescriptor describe_wall_column(const RayColumn *column) {
    return describe_textured_column(column->height, column->depth,
                                    column->texture_id, column->tex_x,
                                    column->tex_y, column->shade, column->flags);
}

static WallColumnDescriptor describe_door_overlay(const RayDoorOverlay *door) {
    return describe_textured_column(door->height, door->depth,
                                    door->texture_id, door->tex_x,
                                    door->tex_y, door->shade,
                                    RAY_COLUMN_FLAG_DOOR);
}

#if RAY_COL_STRIDE == 2
static u8 style_wall_texel(const WallColumnDescriptor *descriptor,
                           u8 tex_y,
                           u8 texel) {
    if ((descriptor->flags & RAY_COLUMN_FLAG_DOOR) == 0) return texel;

    // At 40 sampled wall columns, the original 128px BIGDOOR art collapses
    // into gray noise. Give every interactive slab a stable low-resolution
    // silhouette: a dark metal frame plus a yellow/black moving safety edge.
    // The centre remains the real WAD texture.
    if (descriptor->tex_x < 2 || descriptor->tex_x >= (WALL_TEX_DIM - 2) ||
        tex_y < 2) {
        return 0;
    }
    if (tex_y >= (WALL_TEX_DIM - 4)) {
        return (descriptor->tex_x & 4) ? MEGALDOOM_WORLD_COLOR_WARNING : 0;
    }
    return texel;
}

static u8 sample_wall_descriptor(const WallColumnDescriptor *descriptor,
                                 const RaySceneColors *scene_colors,
                                 u16 y) {
    if (y < descriptor->top) {
        return scene_colors->ceiling_color;
    }
    if (y >= descriptor->bottom) {
        return scene_colors->floor_color;
    }

    const u8 tex_y = (u8)((descriptor->vertical_samples[y - descriptor->top] +
                           descriptor->tex_y) & WALL_TEX_DIM_MASK);
    const u8 texel = descriptor->texture[(tex_y * WALL_TEX_DIM) +
                                          descriptor->tex_x] & 0x0F;
    return descriptor->shade_map[style_wall_texel(descriptor, tex_y, texel)];
}
#else
static u8 style_wall_texel(const WallColumnDescriptor *descriptor,
                           u8 tex_y,
                           u8 texel) {
    if ((descriptor->flags & RAY_COLUMN_FLAG_DOOR) == 0) return texel;
    if (descriptor->tex_x < 2 || descriptor->tex_x >= (WALL_TEX_DIM - 2) ||
        tex_y < 2) {
        return 0;
    }
    if (tex_y >= (WALL_TEX_DIM - 4)) {
        return (descriptor->tex_x & 4) ? MEGALDOOM_WORLD_COLOR_WARNING : 0;
    }
    return texel;
}
#endif
#if (RAY_COL_STRIDE != 4) && (RAY_COL_STRIDE != 2)
#error "build_bsp_tilemap only implements the RAY_COL_STRIDE == 4 and == 2 packers"
#endif


#if RAY_COL_STRIDE == 4
// Pre-shade the 32-texel source column once per sampled ray column. The hot
// per-screen-pixel loop then performs only the vertical DDA lookup and one u16
// load. The 64-byte temporaries stay inside the packer's frame, avoiding both a
// large call-stack allocation and renderer-global cache state.
static void build_packed_wall_column(const WallColumnDescriptor *descriptor,
                                     u16 packed_texels[WALL_TEX_DIM]) {
    for (u16 tex_y = 0; tex_y < WALL_TEX_DIM; tex_y++) {
        u8 texel = descriptor->texture[
            (tex_y * WALL_TEX_DIM) + descriptor->tex_x] & 0x0F;
        texel = style_wall_texel(descriptor, (u8)tex_y, texel);
        packed_texels[tex_y] = descriptor->packed_shade_map[texel];
    }
}

static void build_bsp_tilemap(const RayColumn *columns,
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
                // two sampled columns (each row packs the left/right samples).
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
        }
    }
}
#else /* RAY_COL_STRIDE == 2 */
static void build_bsp_tilemap(const RayColumn *columns,
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
        }
    }
}
#endif

static u8 sample_door_overlay(const WallColumnDescriptor *descriptor,
                              u16 y,
                              u16 lift_pixels) {
    u16 source_y = (u16)(y - descriptor->top + lift_pixels);
    const u16 full_height = (u16)(descriptor->bottom - descriptor->top);
    if (source_y >= full_height) source_y = (u16)(full_height - 1);
    const u8 tex_y = (u8)((descriptor->vertical_samples[source_y] +
                           descriptor->tex_y) & WALL_TEX_DIM_MASK);
    const u8 texel = descriptor->texture[(tex_y * WALL_TEX_DIM) +
                                          descriptor->tex_x] & 0x0F;
    return descriptor->shade_map[style_wall_texel(descriptor, tex_y, texel)];
}

// Moving doors are translucent only in the geometric sense: their raised
// lower gap reveals the fully rendered BSP scene behind, while the remaining
// slab is composited at its own depth without rescaling its texture.
static void draw_door_overlays(const RayColumn *columns, u32 target[][8]) {
    for (u16 x = 0; x < RAY_VIEW_COLS; x += RAY_COL_STRIDE) {
        const RayColumn *column = &columns[x];
        const RayDoorOverlay *door = &column->door;
        if (door->height == 0 || door->depth >= column->depth) continue;

        const WallColumnDescriptor descriptor = describe_door_overlay(door);
        const u16 lift_pixels = (u16)(((u32)door->height * door->lift) >> 8);
        const u16 visible_bottom = (u16)(descriptor.bottom - lift_pixels);
        if (visible_bottom <= descriptor.top) continue;

        const u16 tile_x = (u16)(x >> 3);
#if RAY_COL_STRIDE == 4
        const u16 shift = (u16)((x & 4) ? 0 : 16);
        const u32 keep_mask = ~((u32)0xFFFFu << shift);
#else
        const u16 shift = (u16)((6 - (x & 7)) * 4);
        const u32 keep_mask = ~((u32)0xFFu << shift);
#endif
        for (u16 y = descriptor.top; y < visible_bottom; y++) {
            const u8 color = sample_door_overlay(&descriptor, y, lift_pixels);
#if RAY_COL_STRIDE == 4
            const u32 value = (u32)REP4[color] << shift;
#else
            const u32 value = (u32)REP2[color] << shift;
#endif
            u32 *row = &target[((y >> 3) * VIEW_TILE_W) + tile_x][y & 7];
            *row = (*row & keep_mask) | value;
        }
    }
}

static bool door_overlay_blocks_pixel(const RayColumn *column,
                                      u16 object_depth,
                                      u16 y) {
    const RayDoorOverlay *door = &column->door;
    if (door->height == 0 || door->depth >= column->depth ||
        object_depth < door->depth) {
        return FALSE;
    }
    const u16 top = (u16)((VIEW_PIXEL_H - door->height) / 2);
    const u16 lift_pixels = (u16)(((u32)door->height * door->lift) >> 8);
    return (bool)(y >= top && y < (u16)(top + door->height - lift_pixels));
}


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

// Draw projected billboards object-by-object in painter order. Texture and depth
// decisions remain pixel-exact, but pixels sharing one 8-pixel tile row are
// accumulated into a mask/value pair and committed with one RAM RMW. The old
// column-first path rewrote the same packed u32 once for every opaque pixel.
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
        // 0xFF marks a clipped/wall-hidden screen column. Generated atlas X
        // coordinates are far below 255, so the sentinel cannot alias a texel.
        u8 tex_x_by_screen_col[RAY_VIEW_COLS];
        s16 x0 = object->left;
        s16 x1 = object->right;
        s16 y0 = object->top;
        s16 y1 = object->bottom;
        u32 tex_x_step;
        const u8 atlas_x_last = (u8)(object->atlas_x + object->atlas_w - 1);

        if ((height <= 0) || (width <= 0)) {
            continue;
        }
        // Sample only the occupied atlas rectangle. Transparent letterbox padding
        // must not alter the WAD patch's projected size or origin.
        tex_x_step = divu((u32)object->atlas_w << 8, (u16)width);

        if (x0 < 0) {
            x0 = 0;
        }
        if (x1 >= RAY_VIEW_COLS) {
            x1 = RAY_VIEW_COLS - 1;
        }
        if (y0 < 0) {
            y0 = 0;
        }
        if (y1 >= VIEW_PIXEL_H) {
            y1 = VIEW_PIXEL_H - 1;
        }
        if ((x0 > x1) || (y0 > y1)) {
            continue;
        }

        // Resolve horizontal texture coordinates and whole-column wall depth
        // once. Door slabs remain a per-pixel vertical test below.
        {
            u32 tex_x_acc = ((u32)object->atlas_x << 8) +
                ((u32)(x0 - object->left) * tex_x_step);
            for (s16 col = x0; col <= x1; col++) {
                u8 tex_x = (u8)(tex_x_acc >> 8);
                const u16 wall_col = (u16)(col & ~(RAY_COL_STRIDE - 1));
                tex_x_acc += tex_x_step;

                if (!billboard_depth_visible(object, columns[wall_col].depth)) {
                    tex_x_by_screen_col[col] = 0xFF;
                    continue;
                }
                if (tex_x > atlas_x_last) tex_x = atlas_x_last;
                tex_x_by_screen_col[col] = tex_x;
            }
        }

        // Approximate floor(rel_y * tex.h / height) with one setup divide and a
        // fixed-point DDA instead of a 68k divide for every visible sprite row.
        // tex_y_step = (crop.h<<16)/height is Q16; for a 48px enemy the numerator
        // is 3,145,728 and small projected heights make the quotient exceed 16
        // bits, so a single DIVU.W cannot hold it. The two-stage divider produces
        // the exact same quotient as '/'.
        const u32 tex_y_step = divu32_16_exact((u32)object->atlas_h << 16,
                                               (u16)height);
        u32 tex_y_acc = ((u32)object->atlas_y << 16) +
                        ((u32)(y0 - object->top) * tex_y_step);
        const u8 atlas_y_last = (u8)(object->atlas_y + object->atlas_h - 1);
        for (s16 y = y0; y <= y1; y++) {
            u8 tex_y = (u8)(tex_y_acc >> 16);
            const u16 tile_y = (u16)(y >> 3);
            const u16 row_y = (u16)(y & 7);
            const u16 first_tile_x = (u16)(x0 >> 3);
            const u16 last_tile_x = (u16)(x1 >> 3);
            if (tex_y > atlas_y_last) tex_y = atlas_y_last;
            tex_y_acc += tex_y_step;

            for (u16 tile_x = first_tile_x; tile_x <= last_tile_x; tile_x++) {
                const s16 tile_left = (s16)(tile_x * 8);
                const s16 tile_right = (s16)(tile_left + 7);
                const s16 col_begin = (x0 > tile_left) ? x0 : tile_left;
                const s16 col_end = (x1 < tile_right) ? x1 : tile_right;
                u32 clear_mask = 0;
                u32 value = 0;

                for (s16 col = col_begin; col <= col_end; col++) {
                    const u8 tex_x = tex_x_by_screen_col[col];
                    if (tex_x == 0xFF) continue;

                    const u16 wall_col = (u16)(col & ~(RAY_COL_STRIDE - 1));
                    const u8 texel = lut[
                        tex.pixels[(tex_y * tex.w) + tex_x] & 0x0F];
                    if (texel != 0 && !door_overlay_blocks_pixel(
                            &columns[wall_col], object->depth, (u16)y)) {
                        const u16 shift = (u16)((7 - (col & 7)) * 4);
                        clear_mask |= (u32)0x0F << shift;
                        value |= (u32)texel << shift;
                    }
                }

                if (clear_mask != 0) {
                    const u16 tile_index = (u16)(tile_y * VIEW_TILE_W + tile_x);
                    u32 *dst;
                    renderer_mark_overlay_tile(tile_index);
                    dst = &g_view_tiles[tile_index][row_y];
                    *dst = (*dst & ~clear_mask) | value;
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

static void load_view_tile_run(u16 vram_base, u16 first, u16 count) {
#if DEBUG_PERF
    const u32 issue_start = getSubTick();
#endif
    VDP_loadTileData((const u32 *)&g_view_tiles[first][0],
                     (u16)(vram_base + first),
                     count,
                     DMA);
#if DEBUG_PERF
    renderer_perf_record_upload_run(count, getSubTick() - issue_start);
#endif
}

#if DEBUG_PERF
static void dbg_wait_dma(void) {
    const u32 t = getSubTick();
    VDP_waitDMACompletion();
    renderer_perf_record_dma_wait(getSubTick() - t);
}
#else
#define dbg_wait_dma() VDP_waitDMACompletion()
#endif

static void clear_view_bank_dirty_range(u16 bank, u16 first, u16 end) {
    for (u16 tile = first; tile < end; tile++) {
        const u16 word = (u16)(tile >> 5);
        const u32 mask = (u32)1u << (tile & 31);
        if ((g_view_bank_dirty_bits[bank][word] & mask) != 0) {
            g_view_bank_dirty_bits[bank][word] &= ~mask;
            g_view_bank_dirty_count[bank]--;
        }
    }
}

static bool choose_full_view_upload(u16 bank) {
    const u16 dirty_count = g_view_bank_dirty_count[bank];
    return (bool)((dirty_count >= VIEW_DIRTY_FULL_THRESHOLD) ||
                  (count_view_bank_dirty_runs(bank) > VIEW_DIRTY_MAX_RUNS));
}

void renderer_queue_scene_upload(void) {
    const bool swap = g_upload_requires_bank_swap;
    const u16 bank = swap ? (u16)(g_view_vram_bank ^ 1) : g_view_vram_bank;

    g_upload_requires_bank_swap = FALSE;
    g_view_upload.pending = (bool)(g_view_bank_dirty_count[bank] != 0);
    g_view_upload.full = swap ? TRUE : choose_full_view_upload(bank);
    g_view_upload.swap = swap;
    g_view_upload.bank = bank;
    g_view_upload.cursor = 0;
#if DEBUG_PERF
    renderer_perf_begin_upload(g_view_bank_dirty_count[bank],
                               g_view_upload.full, swap);
#endif
}

bool renderer_scene_upload_pending(void) {
    return g_view_upload.pending;
}

static void finish_view_upload(void) {
    g_view_upload.pending = FALSE;
    if (g_view_upload.full) {
        clear_view_bank_dirty_bits(g_view_upload.bank);
    }
    if (g_view_upload.swap) {
        renderer_set_view_vram_bank(g_view_upload.bank);
    }
}

static void upload_view_tilemap_step(void) {
    u16 budget = VIEW_DMA_TILES_PER_VBLANK;
    const u16 start_cursor = g_view_upload.cursor;
    const u16 vram_base =
        (u16)(VIEW_TILE_BASE + (g_view_upload.bank * VIEW_TILE_COUNT));

    if (!g_view_upload.pending) return;

    if (g_view_upload.full) {
        const u16 remaining = (u16)(VIEW_TILE_COUNT - g_view_upload.cursor);
        const u16 count = (remaining < budget) ? remaining : budget;
        load_view_tile_run(vram_base, g_view_upload.cursor, count);
        g_view_upload.cursor = (u16)(g_view_upload.cursor + count);
    } else {
        while ((g_view_upload.cursor < VIEW_TILE_COUNT) && (budget > 0)) {
            while ((g_view_upload.cursor < VIEW_TILE_COUNT) &&
                   !view_bank_tile_is_dirty(g_view_upload.bank, g_view_upload.cursor)) {
                g_view_upload.cursor++;
            }
            if (g_view_upload.cursor >= VIEW_TILE_COUNT) break;

            const u16 first = g_view_upload.cursor;
            while ((g_view_upload.cursor < VIEW_TILE_COUNT) &&
                   view_bank_tile_is_dirty(g_view_upload.bank, g_view_upload.cursor) &&
                   ((u16)(g_view_upload.cursor - first) < budget)) {
                g_view_upload.cursor++;
            }
            const u16 count = (u16)(g_view_upload.cursor - first);
            load_view_tile_run(vram_base, first, count);
            budget = (u16)(budget - count);
        }
    }

    dbg_wait_dma();
    if (!g_view_upload.full) {
        clear_view_bank_dirty_range(g_view_upload.bank, start_cursor,
                                    g_view_upload.cursor);
    }
    if (g_view_upload.cursor >= VIEW_TILE_COUNT) {
        finish_view_upload();
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
    renderer_perf_reset_overlay_tiles();
#endif
    ProjectedBillboard objects[BILLBOARD_MAX_PROJECTED_TOTAL];
    const u16 object_count = billboard_project_scene(
        player, columns, objects, BILLBOARD_MAX_PROJECTED_TOTAL);
#if DEBUG_PERF
    renderer_perf_set_projection_subticks(getSubTick() - stage_start);
#endif

    if (base_dirty) {
        g_upload_requires_bank_swap = TRUE;
        renderer_prepare_full_base_upload();
        renderer_overlay_base_rebuilt();
#if DEBUG_PERF
        stage_start = getSubTick();
#endif
        build_bsp_tilemap(columns, scene_colors, g_view_tiles);
        draw_door_overlays(columns, g_view_tiles);
#if DEBUG_PERF
        renderer_perf_set_pack_subticks(getSubTick() - stage_start);
        stage_start = getSubTick();
#endif
    } else {
        g_view_dirty_bank_mask = (u16)(1u << g_view_vram_bank);
        renderer_overlay_restore_previous();
#if DEBUG_PERF
        renderer_perf_set_pack_subticks(0);
#endif
    }

    renderer_overlay_begin();
#if DEBUG_PERF
    const u32 bb_start = getSubTick();
#endif
    draw_projected_billboards(columns, objects, object_count);
#if DEBUG_PERF
    renderer_perf_set_billboard_subticks(getSubTick() - bb_start);
    const u32 wpn_start = getSubTick();
#endif
    draw_weapon_overlay(weapon_flash);
    if (damage_flash) {
        draw_overlay_ops(MEGALDOOM_DAMAGE_OVERLAY_OPS, MEGALDOOM_OVERLAY_OP_COUNT[0]);
    } else if (low_health_warning) {
        draw_overlay_ops(MEGALDOOM_LOW_HEALTH_OVERLAY_OPS, MEGALDOOM_OVERLAY_OP_COUNT[1]);
    }
    build_compass_tilemap(player->angle);
    renderer_overlay_finish();
#if DEBUG_PERF
    renderer_perf_set_weapon_subticks(getSubTick() - wpn_start);
#endif
}

void renderer_upload_scene_step(void) {
#if DEBUG_PERF
    const bool was_pending = g_view_upload.pending;
#endif
    upload_view_tilemap_step();
    upload_compass_tilemap();
#if DEBUG_PERF
    renderer_draw_perf_overlay((bool)(was_pending && !g_view_upload.pending));
#endif
}
