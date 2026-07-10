#include "renderer_internal.h"
#include "generated_assets.h"
#include "generated_billboard_assets.h"
#include "generated_hud_assets.h"
#include "generated_renderer_assets.h"

// Flat billboard-texture descriptor. Storing the pixels as a plain const u8* (a
// [rows][cols] array decays cleanly) lets one draw loop sample sprites of any size:
// the enemy is 24x48, every other billboard stays 16x16. Index as pixels[y*w + x].
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
        case BILLBOARD_VISUAL_DECOR_DAMAGED:
        case BILLBOARD_VISUAL_DECOR:
            return (BillboardTex){(const u8 *)FREEDOOM_BILLBOARD_DECOR_TEXTURE, 16, 16};
        case BILLBOARD_VISUAL_KEY:
            return (BillboardTex){(const u8 *)FREEDOOM_BILLBOARD_KEY_TEXTURE, 16, 16};
        case BILLBOARD_VISUAL_BONUS:
        default:
            return (BillboardTex){(const u8 *)FREEDOOM_BILLBOARD_BONUS_TEXTURE, 16, 16};
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
static bool g_compass_dirty = TRUE;

void renderer_mark_overlay_tile(u16 tile_index) {
    const u16 word = (u16)(tile_index >> 5);
    g_overlay_current_bits[word] |= (u32)1u << (tile_index & 31);
    renderer_mark_tile_dirty(tile_index);
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

// Pixel-replication table for the active stride (guarded so the unused one isn't
// compiled): REP4[c] == c*0x1111 spreads a colour across 4px (stride 4); REP2[c] ==
// c*0x11 spreads it across 2px (stride 2, four cast columns per 8px tile).
#if RAY_COL_STRIDE == 4
static const u32 REP4[16] = {
    0x0000, 0x1111, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666, 0x7777,
    0x8888, 0x9999, 0xAAAA, 0xBBBB, 0xCCCC, 0xDDDD, 0xEEEE, 0xFFFF,
};

static u32 pack_flat_row(u8 color) {
    const u32 replicated = REP4[color & 0x0F];
    return (replicated << 16) | replicated;
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
    g_compass_dirty = TRUE;
}

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

#if (RAY_COL_STRIDE != 4) && (RAY_COL_STRIDE != 2)
#error "build_raycast_tilemap only implements the RAY_COL_STRIDE == 4 and == 2 packers"
#endif

#ifndef RENDERER_REFERENCE_PACKER
#define RENDERER_REFERENCE_PACKER 0
#endif

#if RAY_COL_STRIDE == 4
static void build_raycast_tilemap(const RayColumn *columns,
                                  const RaySceneColors *scene_colors,
                                  u32 target[][8]) {
    // Each 8px-wide tile column maps to two cast columns (px 0 and 4), each
    // replicated 4x. Describe both columns once, then sample directly while
    // packing each tile row.
    for (u16 tile_x = 0; tile_x < VIEW_TILE_W; tile_x++) {
        const u16 base_col = (u16)(tile_x * 8);
        const WallColumnDescriptor column_a = describe_wall_column(&columns[base_col]);
        const WallColumnDescriptor column_b = describe_wall_column(&columns[base_col + 4]);
        const u32 ceiling_row = pack_flat_row(scene_colors->ceiling_color);
        const u32 floor_row = pack_flat_row(scene_colors->floor_color);

        for (u16 tile_y = 0; tile_y < VIEW_TILE_H; tile_y++) {
            const u16 tile_index = (u16)((tile_y * VIEW_TILE_W) + tile_x);
            u16 pixel_y = (u16)(tile_y * 8);

            if (((pixel_y + 7) < column_a.top) && ((pixel_y + 7) < column_b.top)) {
                for (u16 row = 0; row < 8; row++) {
                    target[tile_index][row] = ceiling_row;
                }
                continue;
            }

            if ((pixel_y >= column_a.bottom) && (pixel_y >= column_b.bottom)) {
                for (u16 row = 0; row < 8; row++) {
                    target[tile_index][row] = floor_row;
                }
                continue;
            }

            for (u16 row = 0; row < 8; row++, pixel_y++) {
                target[tile_index][row] =
                    (REP4[sample_wall_descriptor(&column_a, scene_colors, pixel_y)] << 16) |
                    REP4[sample_wall_descriptor(&column_b, scene_colors, pixel_y)];
            }
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
        }
    }
}
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
#endif

static u32 g_reference_view_tiles[VIEW_TILE_COUNT][8];
#endif

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

        if ((height <= 0) || (width <= 0)) {
            continue;
        }
        tex_x_step = ((u32)tex.w << 8) / (u16)width;

        if (y0 < 0) {
            y0 = 0;
        }
        if (y1 >= VIEW_PIXEL_H) {
            y1 = VIEW_PIXEL_H - 1;
        }
        if (y0 > y1) {
            continue;
        }

        // Exact equivalent of floor(rel_y * tex.h / height), built only for the
        // visible screen rows. VIEW_PIXEL_H bounds the temp table even for very
        // close sprites whose projected height exceeds 64px.
        for (s16 y = y0; y <= y1; y++) {
            tex_y_by_screen_row[y] = (u8)((((u32)(y - object->top)) * tex.h) / (u16)height);
        }

        for (s16 col = object->left; col <= object->right; col++) {
            u8 tex_x = (u8)(tex_x_acc >> 8);
            tex_x_acc += tex_x_step;

            if ((col < 0) || (col >= RAY_VIEW_COLS)) {
                continue;
            }
            if (object->depth >= columns[col].depth) {
                continue;
            }

            if (tex_x >= tex.w) {
                tex_x = (u8)(tex.w - 1);
            }
            const u16 tile_x = (u16)(col >> 3);
            const u16 shift = (u16)((7 - (col & 7)) * 4);
            const u32 keep_mask = ~((u32)0x0F << shift);

            if ((s16)tile_x != last_marked_tile_x) {
                for (u16 overlay_tile_y = (u16)(y0 >> 3); overlay_tile_y <= (u16)(y1 >> 3); overlay_tile_y++) {
                    renderer_mark_overlay_tile((u16)(overlay_tile_y * VIEW_TILE_W + tile_x));
                }
                last_marked_tile_x = (s16)tile_x;
            }

            u16 y = (u16)y0;
            const u16 y_end = (u16)(y1 + 1);
            while (y < y_end) {
                const u16 tile_y = (u16)(y >> 3);
                u32 *tile = g_view_tiles[(tile_y * VIEW_TILE_W) + tile_x];
                const u16 next_tile_y = (u16)((tile_y + 1) * 8);
                const u16 stop = (next_tile_y < y_end) ? next_tile_y : y_end;

                for (; y < stop; y++) {
                    const u8 texel = lut[tex.pixels[(tex_y_by_screen_row[y] * tex.w) + tex_x] & 0x0F];
                    if (texel != 0) {
                        const u16 row_y = (u16)(y & 7);
                        tile[row_y] = (tile[row_y] & keep_mask) | ((u32)texel << shift);
                    }
                }
            }
        }
    }
}

// Draw the static weapon overlay from its precomposed per-tile-row ops (built
// once in build_weapon_overlay_ops). Each op is a single u32 read-modify-write,
// so an idle frame costs a few hundred RMWs instead of ~72x54 per-pixel tests.
static void draw_weapon_overlay(bool flash) {
    const u16 v = flash ? 1 : 0;
    const u16 *dst_idx = v ? MEGALDOOM_WEAPON_DST_FIRE : MEGALDOOM_WEAPON_DST_IDLE;
    const u32 *values = v ? MEGALDOOM_WEAPON_VALUE_FIRE : MEGALDOOM_WEAPON_VALUE_IDLE;
    const u16 count = MEGALDOOM_WEAPON_OP_COUNT[v];
    u32 *base = &g_view_tiles[0][0];

    for (u16 i = 0; i < count; i++) {
        const u32 val = values[i];
        // Rebuild clear_mask: set every nibble of val that has any bit -> 0x0F.
        // t marks nonzero nibbles (low bit each); (t<<4)-t expands 0x1 -> 0xF per
        // nibble with no cross-nibble carry (0xF < 0x10). Exact for 4bpp texels.
        const u32 t = (val | (val >> 1) | (val >> 2) | (val >> 3)) & 0x11111111u;
        const u32 clear_mask = (t << 4) - t;
        u32 *dst = base + dst_idx[i];
        *dst = (*dst & ~clear_mask) | val;
        renderer_mark_overlay_tile((u16)(dst_idx[i] / 8));
    }
}

static void draw_overlay_ops(const MegalDoomOverlayRowOp *ops, u16 count) {
    u32 *base = &g_view_tiles[0][0];

    for (u16 i = 0; i < count; i++) {
        const MegalDoomOverlayRowOp *op = &ops[i];
        u32 *dst = base + op->dst;
        *dst = (*dst & ~op->clear_mask) | op->value;
        renderer_mark_overlay_tile((u16)(op->dst / 8));
    }
}

static void upload_view_tilemap(void) {
    if (g_view_dirty_count == 0) {
        return;
    }

    if (g_view_dirty_count >= VIEW_DIRTY_FULL_THRESHOLD) {
        VDP_loadTileData((const u32 *)g_view_tiles, VIEW_TILE_BASE, VIEW_TILE_COUNT, DMA);
        for (u16 i = 0; i < VIEW_DIRTY_WORD_COUNT; i++) {
            g_view_dirty_bits[i] = 0;
        }
        g_view_dirty_count = 0;
        return;
    }

    u16 run_count = 0;
    for (u16 tile = 0; tile < VIEW_TILE_COUNT;) {
        const u16 word = (u16)(tile >> 5);
        const u32 mask = (u32)1u << (tile & 31);

        if ((g_view_dirty_bits[word] & mask) == 0) {
            tile++;
            continue;
        }

        const u16 first = tile;
        while (tile < VIEW_TILE_COUNT) {
            const u16 run_word = (u16)(tile >> 5);
            const u32 run_mask = (u32)1u << (tile & 31);
            if ((g_view_dirty_bits[run_word] & run_mask) == 0) {
                break;
            }
            tile++;
        }

        run_count++;
        if (run_count > VIEW_DIRTY_MAX_RUNS) {
            VDP_loadTileData((const u32 *)g_view_tiles, VIEW_TILE_BASE, VIEW_TILE_COUNT, DMA);
            for (u16 i = 0; i < VIEW_DIRTY_WORD_COUNT; i++) {
                g_view_dirty_bits[i] = 0;
            }
            g_view_dirty_count = 0;
            return;
        }

        VDP_loadTileData((const u32 *)&g_view_tiles[first][0],
                         VIEW_TILE_BASE + first,
                         (u16)(tile - first),
                         DMA);
    }

    for (u16 i = 0; i < VIEW_DIRTY_WORD_COUNT; i++) {
        g_view_dirty_bits[i] = 0;
    }
    g_view_dirty_count = 0;
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

static bool overlay_previously_touched(u16 tile_index) {
    const u16 word = (u16)(tile_index >> 5);
    const u32 mask = (u32)1u << (tile_index & 31);

    return (bool)((g_overlay_previous_bits[word] & mask) != 0);
}

static void commit_base_tiles_from_view(void) {
    for (u16 tile = 0; tile < VIEW_TILE_COUNT; tile++) {
        bool changed = (bool)(!g_base_tiles_valid || overlay_previously_touched(tile));

        for (u16 row = 0; row < 8; row++) {
            const u32 row_data = g_view_tiles[tile][row];
            if (g_base_view_tiles[tile][row] != row_data) {
                g_base_view_tiles[tile][row] = row_data;
                changed = TRUE;
            }
        }

        if (changed) {
            renderer_mark_tile_dirty(tile);
        }
    }

    g_base_tiles_valid = TRUE;
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
    ProjectedBillboard objects[BILLBOARD_MAX_PROJECTED_OBJECTS];
    const u16 object_count = billboard_project_scene(player, objects, BILLBOARD_MAX_PROJECTED_OBJECTS);

    if (base_dirty) {
        build_raycast_tilemap(columns, scene_colors, g_view_tiles);
#if RENDERER_REFERENCE_PACKER && RAY_COL_STRIDE == 2
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
        commit_base_tiles_from_view();
        for (u16 i = 0; i < VIEW_DIRTY_WORD_COUNT; i++) {
            g_overlay_previous_bits[i] = 0;
        }
    } else {
        restore_previous_overlay_tiles();
    }

    clear_overlay_bits();
    draw_projected_billboards(columns, objects, object_count);
    draw_weapon_overlay(weapon_flash);
    if (damage_flash) {
        draw_overlay_ops(MEGALDOOM_DAMAGE_OVERLAY_OPS, MEGALDOOM_OVERLAY_OP_COUNT[0]);
    } else if (low_health_warning) {
        draw_overlay_ops(MEGALDOOM_LOW_HEALTH_OVERLAY_OPS, MEGALDOOM_OVERLAY_OP_COUNT[1]);
    }
    build_compass_tilemap(player->angle);
    finish_overlay_bits();
}

// Push the frame built by renderer_render_scene to VRAM. Call this right after a
// VDP_waitVSync so the ~9.6KB view-tile DMA runs at the fast vblank rate instead
// of stalling the CPU mid active-display (its old call site).
void renderer_upload_scene(void) {
    upload_view_tilemap();
    upload_compass_tilemap();
}
