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

// Explicit ROM pointer tables avoid GCC's generic 32-bit frame*stride helper
// for non-power-of-two sprite sheets. Selection is one indexed pointer load.
static const u8 *const ENEMY_FRAME_PIXELS[FREEDOOM_BILLBOARD_ENEMY_FRAME_COUNT] = {
    (const u8 *)FREEDOOM_BILLBOARD_ENEMY_FRAMES[0], (const u8 *)FREEDOOM_BILLBOARD_ENEMY_FRAMES[1],
    (const u8 *)FREEDOOM_BILLBOARD_ENEMY_FRAMES[2], (const u8 *)FREEDOOM_BILLBOARD_ENEMY_FRAMES[3],
    (const u8 *)FREEDOOM_BILLBOARD_ENEMY_FRAMES[4], (const u8 *)FREEDOOM_BILLBOARD_ENEMY_FRAMES[5],
    (const u8 *)FREEDOOM_BILLBOARD_ENEMY_FRAMES[6], (const u8 *)FREEDOOM_BILLBOARD_ENEMY_FRAMES[7],
    (const u8 *)FREEDOOM_BILLBOARD_ENEMY_FRAMES[8], (const u8 *)FREEDOOM_BILLBOARD_ENEMY_FRAMES[9],
};
static const u8 *const BARREL_FRAME_PIXELS[FREEDOOM_BILLBOARD_BARREL_EXPLOSION_FRAME_COUNT] = {
    (const u8 *)FREEDOOM_BILLBOARD_BARREL_EXPLOSION_FRAMES[0],
    (const u8 *)FREEDOOM_BILLBOARD_BARREL_EXPLOSION_FRAMES[1],
    (const u8 *)FREEDOOM_BILLBOARD_BARREL_EXPLOSION_FRAMES[2],
    (const u8 *)FREEDOOM_BILLBOARD_BARREL_EXPLOSION_FRAMES[3],
    (const u8 *)FREEDOOM_BILLBOARD_BARREL_EXPLOSION_FRAMES[4],
};
static const u8 *const PUFF_FRAME_PIXELS[FREEDOOM_BILLBOARD_PUFF_FRAME_COUNT] = {
    (const u8 *)FREEDOOM_BILLBOARD_PUFF_FRAMES[0], (const u8 *)FREEDOOM_BILLBOARD_PUFF_FRAMES[1],
    (const u8 *)FREEDOOM_BILLBOARD_PUFF_FRAMES[2], (const u8 *)FREEDOOM_BILLBOARD_PUFF_FRAMES[3],
};
static const u8 *const BLOOD_FRAME_PIXELS[FREEDOOM_BILLBOARD_BLOOD_FRAME_COUNT] = {
    (const u8 *)FREEDOOM_BILLBOARD_BLOOD_FRAMES[0], (const u8 *)FREEDOOM_BILLBOARD_BLOOD_FRAMES[1],
    (const u8 *)FREEDOOM_BILLBOARD_BLOOD_FRAMES[2],
};

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
            return (BillboardTex){ENEMY_FRAME_PIXELS[f],
                                  FREEDOOM_BILLBOARD_ENEMY_W, FREEDOOM_BILLBOARD_ENEMY_H};
        }
        case BILLBOARD_VISUAL_BARREL_EXPLODING: {
            const u8 f = (frame < FREEDOOM_BILLBOARD_BARREL_EXPLOSION_FRAME_COUNT) ? frame : 0;
            return (BillboardTex){BARREL_FRAME_PIXELS[f],
                                  FREEDOOM_BILLBOARD_BARREL_EXPLOSION_W,
                                  FREEDOOM_BILLBOARD_BARREL_EXPLOSION_H};
        }
        case BILLBOARD_VISUAL_PUFF: {
            const u8 f = (frame < FREEDOOM_BILLBOARD_PUFF_FRAME_COUNT) ? frame : 0;
            return (BillboardTex){PUFF_FRAME_PIXELS[f],
                                  FREEDOOM_BILLBOARD_PUFF_W, FREEDOOM_BILLBOARD_PUFF_H};
        }
        case BILLBOARD_VISUAL_BLOOD: {
            const u8 f = (frame < FREEDOOM_BILLBOARD_BLOOD_FRAME_COUNT) ? frame : 0;
            return (BillboardTex){BLOOD_FRAME_PIXELS[f],
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
static s16 g_last_weapon_variant = -1;
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

// Sparse Semantic Framebuffer classify (Phase 1, Task 1 scaffold).
//
// Given the per-column wall/door/overlay 15-bit masks emitted by the packer,
// build a SparseFrameBuild describing exactly which view tiles changed this
// frame. Under RENDERER_SPARSE_FB==0 we treat every tile as dynamic so the
// produced build is byte-for-byte equivalent to the legacy full upload; the
// real mask-OR classify (wall|door|overlay) is enabled only when the flag is
// flipped to 1 in a later phase. This function is intentionally not yet wired
// into the upload path, so it never alters release behavior while the flag is
// 0.
__attribute__((unused))
static void sparse_classify_frame(const u16 wall_mask[VIEW_TILE_W],
                                  const u16 door_mask[VIEW_TILE_W],
                                  const u16 overlay_mask[VIEW_TILE_W],
                                  SparseFrameBuild *build) {
    // Silence unused-parameter warnings on the flag==0 path where the masks
    // are not consulted; harmless when the flag is 1 and they are used.
    (void)wall_mask;
    (void)door_mask;
    (void)overlay_mask;

    u16 slot = 0;
    build->dynamic_tile_count = 0;
    build->dynamic_run_count = 0;
    for (u16 x = 0; x < VIEW_TILE_W; x++) {
        u16 dyn;
#if RENDERER_SPARSE_FB
        dyn = (u16)(wall_mask[x] | door_mask[x] | overlay_mask[x]);
#else
        dyn = 0x7FFFu; // stub: every tile in the column is dynamic (== old full upload)
#endif
        build->dynamic_mask[x] = dyn;
        build->dynamic_rows[x] = dyn;
        build->mixed_tilemap[x] = x;
        if (dyn != 0) {
            SparseTileRun *run = &build->runs[build->dynamic_run_count];
            run->source_y = x;
            run->tile_count = VIEW_TILE_H;
            run->dest_slot = slot;
            slot = (u16)(slot + VIEW_TILE_H);
            for (u16 y = 0; y < VIEW_TILE_H; y++) {
                const u16 tile_index = (u16)(x * VIEW_TILE_H + y);
                build->slot_allocation[tile_index] = (u16)(run->dest_slot + y);
            }
            build->dynamic_tile_count += VIEW_TILE_H;
            build->dynamic_run_count++;
        }
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
static const u8 FLAT_BAYER_4X4[4][4] = {
    {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5},
};

static u8 sample_flat_color(const RayFlatColor *material, u16 x, u16 y) {
    return (FLAT_BAYER_4X4[y & 3][x & 3] < material->secondary_coverage) ?
        material->secondary : material->primary;
}

#if RAY_COL_STRIDE == 4
static const u32 REP4[16] = {
    0x0000, 0x1111, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666, 0x7777,
    0x8888, 0x9999, 0xAAAA, 0xBBBB, 0xCCCC, 0xDDDD, 0xEEEE, 0xFFFF,
};

static u16 pack_flat_quad(const RayFlatColor *material, u16 x, u16 y) {
    return (u16)REP4[sample_flat_color(material, x, y) & 0x0F];
}
#else /* RAY_COL_STRIDE == 2 */
static const u32 REP2[16] = {
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

typedef struct {
    u32 ceiling[4];
    u32 floor[4];
} PackedFlatRows;

static PackedFlatRows build_flat_rows(const RaySceneColors *scene_colors) {
    PackedFlatRows rows;
    const u32 fixed_floor = (u32)MEGALDOOM_WORLD_COLOR_FLOOR * 0x11111111u;
    for (u16 y = 0; y < 4; y++) {
#if RAY_COL_STRIDE == 4
        const u16 ceiling = pack_flat_quad(&scene_colors->ceiling, 0, y);
        rows.ceiling[y] = ((u32)ceiling << 16) | ceiling;
#else
        rows.ceiling[y] = pack_flat_row(&scene_colors->ceiling, 0, y);
#endif
        // Floors are deliberately invariant across sectors. This constant ROM
        // material eliminates both Bayer sampling and the blue/yellow/brown
        // transitions that did not add useful navigation information.
        rows.floor[y] = fixed_floor;
    }
    return rows;
}

// Tile bands always begin at y%4 == 0, so the four prepacked Bayer rows repeat
// exactly twice. Three MOVEM.L instructions replace two eight-iteration C
// loops for every whole ceiling/floor tile without changing a nibble.
static inline void write_repeated_flat_tile(u32 *target, const u32 rows[4]) {
    __asm__ volatile (
        "movem.l (%1),%%d0-%%d3\n\t"
        "movem.l %%d0-%%d3,(%0)\n\t"
        "movem.l %%d0-%%d3,16(%0)"
        :
        : "a" (target), "a" (rows)
        : "d0", "d1", "d2", "d3", "memory");
}

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
// Pack-stage tile-column coherence: when the four wall descriptors feeding a
// tile column are byte-identical to the previous base build, its 15 packed
// tiles are unchanged and re-packing them is pure waste. FALSE forces a full
// repack next build; every discontinuity (init, level reset, menu return, or
// anything that rebuilds the CPU tile buffer from scratch) must clear it so we
// never skip against stale cached descriptors. See build_bsp_tilemap().
static bool s_coherence_valid = FALSE;

void renderer_scene_init(void) {
    build_shade_luts();
    g_last_compass_angle = 0xFFFF;
    g_last_weapon_variant = -1;
    g_upload_requires_bank_swap = FALSE;
    g_compass_dirty = TRUE;
    g_view_upload = (ViewUploadState){FALSE, FALSE, FALSE, 0, 0};
    s_coherence_valid = FALSE;
    renderer_overlay_reset();
    clear_all_view_banks_dirty();
}

void renderer_invalidate_scene(void) {
    g_last_compass_angle = 0xFFFF;
    g_last_weapon_variant = -1;
    g_upload_requires_bank_swap = FALSE;
    g_compass_dirty = TRUE;
    s_coherence_valid = FALSE;
    renderer_overlay_reset();
    clear_all_view_banks_dirty();
    g_view_dirty_bank_mask = 0;
}

typedef struct {
    u16 top;
    u16 bottom;
    const u8 *texture;
    const u8 *shade_map;
    const u8 *vertical_samples;
    u8 tex_x;
    u8 tex_y;
    u8 texture_id;
    u8 shade_level;
    u8 flags;
} WallColumnDescriptor;

#define DOOR_FRAME_TEXELS (WALL_TEX_DIM / 16)
#define DOOR_SAFETY_TEXELS (WALL_TEX_DIM / 8)

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
    return (WallColumnDescriptor){top, bottom, (const u8 *)tex, shade_map, ty_table,
                                  tex_x, tex_y_value, tid, (u8)fog_level, flags};
#else
    return (WallColumnDescriptor){top, bottom, (const u8 *)tex, shade_map, ty_table,
                                  tex_x, tex_y_value, tid, (u8)fog_level, flags};
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

    // At 80 sampled wall columns, preserve a stable interactive silhouette
    // around the original BIGDOOR art: a dark metal frame plus a yellow/black
    // moving safety edge.
    // The centre remains the real WAD texture.
    if (descriptor->tex_x < DOOR_FRAME_TEXELS ||
        descriptor->tex_x >= (WALL_TEX_DIM - DOOR_FRAME_TEXELS) ||
        tex_y < DOOR_FRAME_TEXELS) {
        return 0;
    }
    if (tex_y >= (WALL_TEX_DIM - DOOR_SAFETY_TEXELS)) {
        return (descriptor->tex_x & DOOR_SAFETY_TEXELS) ?
                   MEGALDOOM_WORLD_COLOR_WARNING : 0;
    }
    return texel;
}

#else
static u8 style_wall_texel(const WallColumnDescriptor *descriptor,
                           u8 tex_y,
                           u8 texel) {
    if ((descriptor->flags & RAY_COLUMN_FLAG_DOOR) == 0) return texel;
    if (descriptor->tex_x < DOOR_FRAME_TEXELS ||
        descriptor->tex_x >= (WALL_TEX_DIM - DOOR_FRAME_TEXELS) ||
        tex_y < DOOR_FRAME_TEXELS) {
        return 0;
    }
    if (tex_y >= (WALL_TEX_DIM - DOOR_SAFETY_TEXELS)) {
        return (descriptor->tex_x & DOOR_SAFETY_TEXELS) ?
                   MEGALDOOM_WORLD_COLOR_WARNING : 0;
    }
    return texel;
}
#endif
#if (RAY_COL_STRIDE != 4) && (RAY_COL_STRIDE != 2)
#error "build_bsp_tilemap only implements the RAY_COL_STRIDE == 4 and == 2 packers"
#endif


#if RAY_COL_STRIDE == 4
static void build_bsp_tilemap(const RayColumn *columns,
                                   const RaySceneColors *scene_colors,
                                   u32 target[][8]) {
    const PackedFlatRows flat_rows = build_flat_rows(scene_colors);
    // Each 8px-wide tile column maps to two cast columns (px 0 and 4), each
    // replicated 4x. Pre-shade each column's 32 texels once, then pack directly
    // into each tile row. Whole-tile ceiling/floor bands skip per-row wall
    // sampling, and the two 120-entry u16 strips are gone: the packer writes
    // each output u32 exactly once instead of building and re-reading a
    // full-height temporary (~19 KB of intermediate traffic per base frame).
    for (u16 tile_x = 0; tile_x < VIEW_TILE_W; tile_x++) {
        const u16 base_col = (u16)(tile_x * 8);
        const WallColumnDescriptor column_a = describe_wall_column(&columns[base_col]);
        const WallColumnDescriptor column_b = describe_wall_column(&columns[base_col + 4]);

        const u16 *packed_column_a = FREEDOOM_WALL_PACKED_COLUMNS[
            (column_a.flags & RAY_COLUMN_FLAG_DOOR) != 0]
            [column_a.shade_level][column_a.texture_id][column_a.tex_x];
        const u16 *packed_column_b = FREEDOOM_WALL_PACKED_COLUMNS[
            (column_b.flags & RAY_COLUMN_FLAG_DOOR) != 0]
            [column_b.shade_level][column_b.texture_id][column_b.tex_x];

        for (u16 tile_y = 0; tile_y < VIEW_TILE_H; tile_y++) {
            const u16 tile_index = view_tile_index(tile_x, tile_y);
            const u16 pixel_y = (u16)(tile_y * 8);
            u32 *tile = target[tile_index];

            if ((pixel_y + 7) < column_a.top && (pixel_y + 7) < column_b.top) {
                // Whole-tile ceiling: this 8px band lies entirely above both walls.
                write_repeated_flat_tile(tile, flat_rows.ceiling);
            } else if (pixel_y >= column_a.bottom && pixel_y >= column_b.bottom) {
                // Whole-tile floor: this 8px band lies entirely below both walls.
                write_repeated_flat_tile(tile, flat_rows.floor);
            } else {
                // Mixed band: resolve each of the 8 rows directly from the two
                // pre-shaded columns. Output is byte-identical to the old
                // two sampled columns (each row packs the left/right samples).
                for (u16 row = 0; row < 8; row++) {
                    const u16 py = (u16)(pixel_y + row);
                    u16 val_a;
                    if (py < column_a.top) {
                        val_a = (u16)(flat_rows.ceiling[py & 3] >> 16);
                    } else if (py >= column_a.bottom) {
                        val_a = (u16)(flat_rows.floor[py & 3] >> 16);
                    } else {
                        val_a = packed_column_a[
                            (column_a.vertical_samples[py - column_a.top] +
                             column_a.tex_y) & WALL_TEX_DIM_MASK];
                    }
                    u16 val_b;
                    if (py < column_b.top) {
                        val_b = (u16)flat_rows.ceiling[py & 3];
                    } else if (py >= column_b.bottom) {
                        val_b = (u16)flat_rows.floor[py & 3];
                    } else {
                        val_b = packed_column_b[
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
#ifndef RENDERER_HOTPATH_C_REFERENCE
#define RENDERER_HOTPATH_C_REFERENCE 1
#endif

void renderer_write_mixed_stride2_tile_asm(
    u32 *tile,
    u16 pixel_y,
    const WallColumnDescriptor descriptors[4],
    const u8 *const packed_columns[4],
    const PackedFlatRows *flat_rows);

#if DEBUG_PERF
#define ASM_PROBE_CANARY_A 0x51A7C0DEu
#define ASM_PROBE_CANARY_B 0xC001D00Du
typedef struct {
    u32 before[2];
    u32 tile[8];
    u32 after[2];
} AsmTileProbe;

static AsmTileProbe g_asm_tile_probe;
static u16 g_asm_compare_cursor;

// Check one framebuffer tile per rebuilt frame. A ten-second 30fps route
// therefore covers all 300 tiles without paying for a second full framebuffer
// every frame. Assembly writes only to the guarded scratch tile; C remains the
// displayed result even after a perfect comparison.
static void compare_stride2_tile_asm(u16 tile_index,
                                    bool mixed,
                                    const u32 c_tile[8],
                                    u16 pixel_y,
                                    const WallColumnDescriptor descriptors[4],
                                    const u8 *const packed_columns[4],
                                    const PackedFlatRows *flat_rows) {
    bool mismatch = FALSE;
    bool canary_failure = FALSE;
    bool completed_cycle;

    if (tile_index != g_asm_compare_cursor) return;
    g_asm_tile_probe.before[0] = ASM_PROBE_CANARY_A;
    g_asm_tile_probe.before[1] = ASM_PROBE_CANARY_B;
    g_asm_tile_probe.after[0] = ASM_PROBE_CANARY_B;
    g_asm_tile_probe.after[1] = ASM_PROBE_CANARY_A;

    if (mixed) {
        for (u16 row = 0; row < 8; row++) g_asm_tile_probe.tile[row] = 0xA5A5A5A5u;
        renderer_write_mixed_stride2_tile_asm(g_asm_tile_probe.tile, pixel_y,
                                              descriptors, packed_columns, flat_rows);
        for (u16 row = 0; row < 8; row++) {
            if (g_asm_tile_probe.tile[row] != c_tile[row]) mismatch = TRUE;
        }
    }
    canary_failure = (bool)(g_asm_tile_probe.before[0] != ASM_PROBE_CANARY_A ||
                            g_asm_tile_probe.before[1] != ASM_PROBE_CANARY_B ||
                            g_asm_tile_probe.after[0] != ASM_PROBE_CANARY_B ||
                            g_asm_tile_probe.after[1] != ASM_PROBE_CANARY_A);
    completed_cycle = (bool)(g_asm_compare_cursor == (VIEW_TILE_COUNT - 1));
    renderer_perf_record_asm_compare(tile_index, mismatch, canary_failure,
                                     completed_cycle);
    g_asm_compare_cursor++;
    if (g_asm_compare_cursor == VIEW_TILE_COUNT) g_asm_compare_cursor = 0;
}
#endif

// A mixed tile used to resolve four columns for each row, then shift/OR four
// bytes into a u32. On the big-endian 68000 the four packed pairs are already
// the four bytes of that u32, so write each lane directly. Splitting each lane
// into ceiling/wall/floor runs removes the four per-row branches and all of the
// long shifts from the hottest packing path.
#if RENDERER_HOTPATH_C_REFERENCE
static __attribute__((noinline)) void write_mixed_stride2_tile_reference(
    u32 *tile,
    u16 pixel_y,
    const WallColumnDescriptor descriptors[4],
    const u8 *const packed_columns[4],
    const PackedFlatRows *flat_rows) {
    u8 *const tile_bytes = (u8 *)tile;
    const u8 *const ceiling_bytes = (const u8 *)flat_rows->ceiling;
    const u8 *const floor_bytes = (const u8 *)flat_rows->floor;
    const u16 end_y = (u16)(pixel_y + 8);

    for (u16 lane = 0; lane < 4; lane++) {
        const WallColumnDescriptor *const descriptor = &descriptors[lane];
        const u8 *const packed_column = packed_columns[lane];
        u8 *dst = &tile_bytes[lane];
        u16 y = pixel_y;
        u16 run_end = descriptor->top;
        if (run_end > end_y) run_end = end_y;

        while (y < run_end) {
            *dst = ceiling_bytes[((y & 3) << 2) + lane];
            dst += 4;
            y++;
        }

        if (y < descriptor->top) y = descriptor->top;
        run_end = descriptor->bottom;
        if (run_end > end_y) run_end = end_y;
        while (y < run_end) {
            *dst = packed_column[
                (descriptor->vertical_samples[y - descriptor->top] +
                 descriptor->tex_y) & WALL_TEX_DIM_MASK];
            dst += 4;
            y++;
        }

        while (y < end_y) {
            *dst = floor_bytes[((y & 3) << 2) + lane];
            dst += 4;
            y++;
        }
    }
}
#define write_mixed_stride2_tile write_mixed_stride2_tile_reference
#else
#define write_mixed_stride2_tile renderer_write_mixed_stride2_tile_asm
#endif

// Previous base build's per-tile-column packing inputs, for coherence skipping.
static WallColumnDescriptor s_prev_desc[VIEW_TILE_W][4];
static PackedFlatRows s_prev_flat_rows;
static u8 s_prev_door_active[VIEW_TILE_W];

// A tile column's 15 packed tiles are a pure function of its four wall
// descriptors and the shared flat rows, so field-wise equality of those
// descriptors is sufficient to prove the packed output is unchanged. (Compared
// by field rather than memcmp so the struct's padding byte cannot spuriously
// force a repack.)
static inline bool wall_desc_equal(const WallColumnDescriptor *a,
                                   const WallColumnDescriptor *b) {
    return (bool)(a->top == b->top && a->bottom == b->bottom &&
                  a->texture == b->texture && a->shade_map == b->shade_map &&
                  a->vertical_samples == b->vertical_samples &&
                  a->tex_x == b->tex_x && a->tex_y == b->tex_y &&
                  a->texture_id == b->texture_id &&
                  a->shade_level == b->shade_level && a->flags == b->flags);
}

static inline bool flat_rows_equal(const PackedFlatRows *a,
                                   const PackedFlatRows *b) {
    for (u16 i = 0; i < 4; i++) {
        if (a->ceiling[i] != b->ceiling[i] || a->floor[i] != b->floor[i]) {
            return FALSE;
        }
    }
    return TRUE;
}

// A column carries an active door overlay when any of its four sampled columns
// has a door in front of the wall. draw_door_overlays() (run after packing)
// read-modify-writes those tiles every frame and only rewrites the door's own
// pixel span, so a column with a door now, or one last frame, must be repacked:
// otherwise the wall behind a lifting door would keep stale door pixels in the
// newly revealed gap. Mirrors the guard in draw_door_overlays().
static inline bool column_door_active(const RayColumn *columns, u16 base_col) {
    for (u16 i = 0; i < 8; i += 2) {
        const RayColumn *column = &columns[base_col + i];
        const RayDoorOverlay *door = &column->door;
        if (door->height != 0 && door->depth < column->depth) return TRUE;
    }
    return FALSE;
}

static void build_bsp_tilemap(const RayColumn *columns,
                                  const RaySceneColors *scene_colors,
                                  u32 target[][8]) {
    const PackedFlatRows flat_rows = build_flat_rows(scene_colors);
    // Ceiling/floor colour changes (e.g. lighting) invalidate every column at
    // once; otherwise coherence is decided per column below.
    const bool flat_changed = (bool)(!s_coherence_valid ||
                                     !flat_rows_equal(&flat_rows, &s_prev_flat_rows));
    // Columns a restorable overlay (billboard / damage-flash) baked into
    // g_view_tiles last frame must be re-packed so those pixels are erased back
    // to the wall/flat behind them: the rebuild path never runs restore_previous,
    // so re-packing the base is the only thing that clears a moved overlay.
    const u32 overlay_columns = renderer_overlay_prev_columns();
#if DEBUG_PERF
    const RendererPerfDeepPhase deep_phase = renderer_perf_get_deep_phase();
    const bool measure_mixed = (bool)(deep_phase == RENDERER_PERF_DEEP_PACK_MIXED);
    const bool measure_flat = (bool)(deep_phase == RENDERER_PERF_DEEP_PACK_FLAT);
    // ColumnReuseOracle: count how many tile columns the coherence cache
    // actually repacks this frame. This is a pure measurement of how much a
    // future per-column uploader could skip; it does not change any output.
    u16 oracle_changed_columns = 0;
    // SparseTileOracle: among the tiles this frame actually repacks, how many
    // are wall (dynamic) vs. full ceiling/floor (which a sparse architecture
    // would serve from a shared static tile, 0 DMA during movement). Pure
    // measurement; does not change output.
    u16 sparse_dyn_wall = 0;
    u16 sparse_ceiling = 0;
    u16 sparse_floor = 0;
    u16 sparse_overlay = 0;
    u16 sparse_runs = 0;
#endif
    // Each 8px-wide tile column maps to four cast columns (px 0, 2, 4, 6), each
    // replicated 2x -> twice the horizontal detail of the stride-4 packer at the
    // same tile count / DMA cost. Describe each column once and pack MSB-first.
    for (u16 tile_x = 0; tile_x < VIEW_TILE_W; tile_x++) {
        const u16 base_col = (u16)(tile_x * 8);
        const WallColumnDescriptor descriptors[4] = {
            describe_wall_column(&columns[base_col]),
            describe_wall_column(&columns[base_col + 2]),
            describe_wall_column(&columns[base_col + 4]),
            describe_wall_column(&columns[base_col + 6])
        };
        const bool door_active = column_door_active(columns, base_col);
        // Skip the whole tile column when its packed output cannot have changed:
        // identical descriptors, unchanged flat rows, and no door RMW to redo
        // (neither this frame nor last). g_view_tiles already holds the correct
        // bytes, and the upload ships them, so this only elides redundant packing.
        if (!flat_changed && !door_active && !s_prev_door_active[tile_x] &&
            !(overlay_columns & ((u32)1u << tile_x)) &&
            wall_desc_equal(&descriptors[0], &s_prev_desc[tile_x][0]) &&
            wall_desc_equal(&descriptors[1], &s_prev_desc[tile_x][1]) &&
            wall_desc_equal(&descriptors[2], &s_prev_desc[tile_x][2]) &&
            wall_desc_equal(&descriptors[3], &s_prev_desc[tile_x][3])) {
            continue;
        }
#if DEBUG_PERF
        oracle_changed_columns++;
#endif
        s_prev_desc[tile_x][0] = descriptors[0];
        s_prev_desc[tile_x][1] = descriptors[1];
        s_prev_desc[tile_x][2] = descriptors[2];
        s_prev_desc[tile_x][3] = descriptors[3];
        s_prev_door_active[tile_x] = (u8)door_active;

        const u8 *const packed_columns[4] = {
            FREEDOOM_WALL_PACKED_PAIRS[
                (descriptors[0].flags & RAY_COLUMN_FLAG_DOOR) != 0]
                [descriptors[0].shade_level][descriptors[0].texture_id][descriptors[0].tex_x],
            FREEDOOM_WALL_PACKED_PAIRS[
                (descriptors[1].flags & RAY_COLUMN_FLAG_DOOR) != 0]
                [descriptors[1].shade_level][descriptors[1].texture_id][descriptors[1].tex_x],
            FREEDOOM_WALL_PACKED_PAIRS[
                (descriptors[2].flags & RAY_COLUMN_FLAG_DOOR) != 0]
                [descriptors[2].shade_level][descriptors[2].texture_id][descriptors[2].tex_x],
            FREEDOOM_WALL_PACKED_PAIRS[
                (descriptors[3].flags & RAY_COLUMN_FLAG_DOOR) != 0]
                [descriptors[3].shade_level][descriptors[3].texture_id][descriptors[3].tex_x]
        };
        for (u16 tile_y = 0; tile_y < VIEW_TILE_H; tile_y++) {
            const u16 tile_index = view_tile_index(tile_x, tile_y);
            const u16 pixel_y = (u16)(tile_y * 8);

            if (((pixel_y + 7) < descriptors[0].top) &&
                ((pixel_y + 7) < descriptors[1].top) &&
                ((pixel_y + 7) < descriptors[2].top) &&
                ((pixel_y + 7) < descriptors[3].top)) {
#if DEBUG_PERF
                const u32 flat_start = measure_flat ? getSubTick() : 0;
#endif
                write_repeated_flat_tile(target[tile_index], flat_rows.ceiling);
#if DEBUG_PERF
                sparse_ceiling++;
                if (measure_flat) {
                    renderer_perf_record_deep(RENDERER_PERF_DEEP_PACK_FLAT,
                                              getSubTick() - flat_start, 1);
                }
                compare_stride2_tile_asm(tile_index, FALSE, target[tile_index],
                                         pixel_y, descriptors, packed_columns, &flat_rows);
#endif
                continue;
            }

            if ((pixel_y >= descriptors[0].bottom) &&
                (pixel_y >= descriptors[1].bottom) &&
                (pixel_y >= descriptors[2].bottom) &&
                (pixel_y >= descriptors[3].bottom)) {
#if DEBUG_PERF
                const u32 flat_start = measure_flat ? getSubTick() : 0;
#endif
                write_repeated_flat_tile(target[tile_index], flat_rows.floor);
#if DEBUG_PERF
                sparse_floor++;
                if (measure_flat) {
                    renderer_perf_record_deep(RENDERER_PERF_DEEP_PACK_FLAT,
                                              getSubTick() - flat_start, 1);
                }
                compare_stride2_tile_asm(tile_index, FALSE, target[tile_index],
                                         pixel_y, descriptors, packed_columns, &flat_rows);
#endif
                continue;
            }

#if DEBUG_PERF
            const u32 mixed_start = measure_mixed ? getSubTick() : 0;
#endif
            write_mixed_stride2_tile(target[tile_index], pixel_y,
                                     descriptors, packed_columns, &flat_rows);
#if DEBUG_PERF
            sparse_dyn_wall++;
            if (measure_mixed) {
                renderer_perf_record_deep(RENDERER_PERF_DEEP_PACK_MIXED,
                                          getSubTick() - mixed_start, 1);
            }
            compare_stride2_tile_asm(tile_index, TRUE, target[tile_index],
                                     pixel_y, descriptors, packed_columns, &flat_rows);
#endif
        }
    }
    s_prev_flat_rows = flat_rows;
    s_coherence_valid = TRUE;
#if DEBUG_PERF
    // SparseTileOracle: overlay columns become temporarily-dynamic tiles
    // (copy-on-write of a static floor/ceiling tile). Their 15 tiles were
    // already counted in sparse_dyn_wall/ceiling/floor above, so subtract
    // them there and count them once as overlay to avoid double counting.
    u16 ov_cols = 0;
    for (u16 x = 0; x < VIEW_TILE_W; x++) {
        if (overlay_columns & ((u32)1u << x)) ov_cols++;
    }
    const u16 ov_tiles = (u16)(ov_cols * VIEW_TILE_H);
    sparse_overlay = ov_tiles;
    if (sparse_dyn_wall > ov_tiles) sparse_dyn_wall -= ov_tiles;
    else sparse_dyn_wall = 0;
    sparse_runs = oracle_changed_columns; // ~one wall run per changed column
    const u16 dma_bytes = (u16)(sparse_dyn_wall * 32u + sparse_overlay * 32u + 600u);
    renderer_perf_record_sparse(sparse_dyn_wall, sparse_ceiling, sparse_floor,
                                sparse_overlay, sparse_runs, dma_bytes);
    renderer_perf_record_column_reuse(
        oracle_changed_columns,
        (u16)(VIEW_TILE_W - oracle_changed_columns),
        (u16)(oracle_changed_columns * VIEW_TILE_H));
#endif
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
            u32 *row = &target[view_tile_index(tile_x, (u16)(y >> 3))][y & 7];
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

static u32 scene_mulu_word(u16 left, u16 right) {
    u32 result = left;
    __asm__ volatile (
        "mulu.w %1,%0"
        : "+d" (result)
        : "d" (right)
        : "cc");
    return result;
}

static u32 scene_mul_u32_u16(u32 left, u16 right) {
    return scene_mulu_word((u16)left, right) +
           (scene_mulu_word((u16)(left >> 16), right) << 16);
}

// Generated pickup posts are an exact ROM copy of each source column's opaque
// spans. Only sparse collectible textures take this path; dense sprites remain
// on the row/tile packer where an extra post lookup would cost more than the
// transparent texel test it replaces.
static inline bool pickup_post_contains(u8 visual_id, u8 tex_x, u8 tex_y) {
    const u16 begin = FREEDOOM_BILLBOARD_PICKUP_POST_OFFSETS[visual_id][tex_x];
    const u16 end = FREEDOOM_BILLBOARD_PICKUP_POST_OFFSETS[visual_id][tex_x + 1];

    for (u16 post = begin; post < end; post++) {
        const u8 top = FREEDOOM_BILLBOARD_PICKUP_POSTS[post][0];
        const u8 length = FREEDOOM_BILLBOARD_PICKUP_POSTS[post][1];
        if (tex_y < top) return FALSE;
        if ((u8)(tex_y - top) < length) return TRUE;
    }
    return FALSE;
}

// Draw projected billboards object-by-object in painter order. Texture and depth
// decisions remain pixel-exact, but pixels sharing one 8-pixel tile row are
// accumulated into a mask/value pair and committed with one RAM RMW. The old
// column-first path rewrote the same packed u32 once for every opaque pixel.
static void draw_projected_billboards(const RayColumn *columns,
                                      const ProjectedBillboard *objects,
                                      u16 object_count) {
#if DEBUG_PERF
    const bool measure_pickup_posts = (bool)(
        renderer_perf_get_deep_phase() == RENDERER_PERF_DEEP_PICKUP_POSTS);
#endif
    for (u16 i = 0; i < object_count; i++) {
        const ProjectedBillboard *object = &objects[i];
        const s16 height = (s16)(object->bottom - object->top + 1);
        const s16 width = (s16)(object->right - object->left + 1);
        const BillboardTex tex = get_billboard_texture(object->visual_id, object->frame);
        const u8 *lut = MEGALDOOM_BILLBOARD_REMAP[
            (object->visual_id < 6) ? object->visual_id : BILLBOARD_VISUAL_BONUS];
        const bool use_pickup_posts = (bool)(
            object->visual_id < FREEDOOM_BILLBOARD_PICKUP_TEXTURE_COUNT &&
            FREEDOOM_BILLBOARD_PICKUP_USE_POSTS[object->visual_id]);
        // 0xFF marks a clipped/wall-hidden screen column. Generated atlas X
        // coordinates are far below 255, so the sentinel cannot alias a texel.
        u8 tex_x_by_screen_col[RAY_VIEW_COLS];
        s16 x0 = object->left;
        s16 x1 = object->right;
        s16 y0 = object->top;
        s16 y1 = object->bottom;
        u32 tex_x_step;
        const u8 atlas_x_last = (u8)(object->atlas_x + object->atlas_w - 1);
        u16 first_tile_x;
        u16 last_tile_x;
        bool has_door_overlay = FALSE;

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
#if DEBUG_PERF
        const u32 pickup_post_start = (measure_pickup_posts && use_pickup_posts) ?
                                          getSubTick() : 0;
#endif
        first_tile_x = (u16)(x0 >> 3);
        last_tile_x = (u16)(x1 >> 3);

        // Resolve horizontal texture coordinates and whole-column wall depth
        // once. Door slabs remain a per-pixel vertical test below.
        {
            u32 tex_x_acc = ((u32)object->atlas_x << 8) +
                scene_mulu_word((u16)(x0 - object->left), (u16)tex_x_step);
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

        // Most views have no moving door slab across a sprite. Detect that once
        // per object so the hot pixel loop can short-circuit the full vertical
        // door/depth test instead of repeating it for every opaque texel.
        for (u16 wall_col = (u16)(x0 & ~(RAY_COL_STRIDE - 1));
             wall_col <= (u16)x1;
             wall_col = (u16)(wall_col + RAY_COL_STRIDE)) {
            const RayDoorOverlay *door = &columns[wall_col].door;
            if (door->height != 0 && door->depth < columns[wall_col].depth &&
                object->depth >= door->depth) {
                has_door_overlay = TRUE;
                break;
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
                        scene_mul_u32_u16(tex_y_step,
                                         (u16)(y0 - object->top));
        const u8 atlas_y_last = (u8)(object->atlas_y + object->atlas_h - 1);
        for (s16 y = y0; y <= y1; y++) {
            u8 tex_y = (u8)(tex_y_acc >> 16);
            const u16 tile_y = (u16)(y >> 3);
            const u16 row_y = (u16)(y & 7);
            if (tex_y > atlas_y_last) tex_y = atlas_y_last;
            tex_y_acc += tex_y_step;
            const u8 *tex_row = &tex.pixels[scene_mulu_word(tex_y, tex.w)];

            for (u16 tile_x = first_tile_x; tile_x <= last_tile_x; tile_x++) {
                const s16 tile_left = (s16)(tile_x * 8);
                const s16 tile_right = (s16)(tile_left + 7);
                const s16 col_begin = (x0 > tile_left) ? x0 : tile_left;
                const s16 col_end = (x1 < tile_right) ? x1 : tile_right;
                u32 clear_mask = 0;
                u32 value = 0;

                // Raster one wall sample (two screen pixels) per iteration.
                // Both authored texels are still sampled independently, so the
                // packed framebuffer is byte-identical to the scalar reference.
                for (s16 pair_col = (s16)(col_begin & ~(RAY_COL_STRIDE - 1));
                     pair_col <= col_end; pair_col += RAY_COL_STRIDE) {
                    const u16 wall_col = (u16)pair_col;
                    u32 pair_mask = 0;
                    u32 pair_value = 0;

                    for (u16 pixel = 0; pixel < RAY_COL_STRIDE; pixel++) {
                        const s16 col = (s16)(pair_col + pixel);
                        if (col < col_begin || col > col_end) continue;
                        const u8 tex_x = tex_x_by_screen_col[col];
                        if (tex_x == 0xFF) continue;
                        if (use_pickup_posts &&
                            !pickup_post_contains(object->visual_id, tex_x, tex_y)) {
                            continue;
                        }
                        const u8 texel = lut[tex_row[tex_x] & 0x0F];
                        if (texel != 0) {
                            const u16 shift = (u16)((7 - (col & 7)) * 4);
                            pair_mask |= (u32)0x0F << shift;
                            pair_value |= (u32)texel << shift;
                        }
                    }
                    if (pair_mask != 0 && (!has_door_overlay ||
                            !door_overlay_blocks_pixel(&columns[wall_col],
                                                       object->depth, (u16)y))) {
                        clear_mask |= pair_mask;
                        value |= pair_value;
                    }
                }

                if (clear_mask != 0) {
                    const u16 tile_index = view_tile_index(tile_x, tile_y);
                    u32 *dst;
                    renderer_mark_overlay_tile(tile_index);
                    dst = &g_view_tiles[tile_index][row_y];
                    *dst = (*dst & ~clear_mask) | value;
                }
            }
        }
#if DEBUG_PERF
        if (measure_pickup_posts && use_pickup_posts) {
            renderer_perf_record_deep(RENDERER_PERF_DEEP_PICKUP_POSTS,
                                      getSubTick() - pickup_post_start, 1);
        }
#endif
    }
}

// The weapon is a transparent BG_A tile layer. Camera movement therefore never
// recomposes it into the 300 dynamic view tiles; only idle/fire transitions
// rewrite this small tilemap rectangle.
static void draw_weapon_overlay(bool flash) {
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
    const bool rebuild_base = (bool)(base_dirty ||
        renderer_overlay_requires_base_rebuild());
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

    if (rebuild_base) {
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
