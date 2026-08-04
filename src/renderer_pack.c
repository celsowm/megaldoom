#include "renderer_pack_internal.h"
#include "debug_checkpoint.h"
#include "renderer_perf.h"
#include "generated_assets.h"

// Distance fog: walls are darkened in discrete steps the farther they are. Level
// 0 is identity; each higher level applies the luminance-derived mapping emitted
// alongside PAL3, so no hand-authored palette indices can shift a hue to blue.
// N/S ("shade") walls add one extra level.
#define SHADE_LEVELS 4
// depth (world units) >> FOG_SHIFT picks the base fog level. Tuned so mid-room
// walls sit around level 1-2 and distant walls saturate at the darkest level.
#define FOG_SHIFT 9
static u8 g_shade_luts[SHADE_LEVELS][16];

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

// Pack-stage tile-column coherence: when the four wall descriptors feeding a
// tile column are byte-identical to the previous base build, its 15 packed
// tiles are unchanged and re-packing them is pure waste. FALSE forces a full
// repack next build; every discontinuity (init, level reset, menu return, or
// anything that rebuilds the CPU tile buffer from scratch) must clear it so we
// never skip against stale cached descriptors. See build_bsp_tilemap().
static bool s_coherence_valid = FALSE;

void pack_stage_reset(void) {
    build_shade_luts();
    s_coherence_valid = FALSE;
}

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

WallColumnDescriptor describe_wall_column(const RayColumn *column) {
    return describe_textured_column(column->height, column->depth,
                                    column->texture_id, column->tex_x,
                                    column->tex_y, column->shade, column->flags);
}

WallColumnDescriptor describe_door_overlay(const RayDoorOverlay *door) {
    return describe_textured_column(door->height, door->depth,
                                    door->texture_id, door->tex_x,
                                    door->tex_y, door->shade,
                                    RAY_COLUMN_FLAG_DOOR);
}

// Sampled cast columns feeding one 8px-wide tile column (4 at stride 2, 2 at
// stride 4).
#define PACK_LANES (8 / RAY_COL_STRIDE)

// Previous base build's per-tile-column packing inputs, for coherence skipping.
static WallColumnDescriptor s_prev_desc[VIEW_TILE_W][PACK_LANES];
static PackedFlatRows s_prev_flat_rows;
static u8 s_prev_door_active[VIEW_TILE_W];

// A tile column's 15 packed tiles are a pure function of its wall descriptors
// and the shared flat rows, so field-wise equality of those descriptors is
// sufficient to prove the packed output is unchanged. (Compared by field
// rather than memcmp so the struct's padding byte cannot spuriously force a
// repack.)
//
// `texture`, `shade_map`, and `vertical_samples` are omitted: each is a pure
// function of a field already compared below (describe_textured_column sets
// texture = FREEDOOM_WALL_TEXTURES[texture_id], shade_map = g_shade_luts[shade_level],
// vertical_samples = MEGALDOOM_WALL_TEX_Y_BY_HEIGHT[bottom-top]), so equality
// of texture_id/shade_level/(top,bottom) already implies their equality.
static inline bool wall_desc_equal(const WallColumnDescriptor *a,
                                   const WallColumnDescriptor *b) {
    return (bool)(a->top == b->top && a->bottom == b->bottom &&
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

// A column carries an active door overlay when any of its sampled columns
// has a door in front of the wall. draw_door_overlays() (run after packing)
// read-modify-writes those tiles every frame and only rewrites the door's own
// pixel span, so a column with a door now, or one last frame, must be repacked:
// otherwise the wall behind a lifting door would keep stale door pixels in the
// newly revealed gap. Mirrors the guard in draw_door_overlays().
static inline bool column_door_active(const RayColumn *columns, u16 base_col) {
    for (u16 i = 0; i < 8; i += RAY_COL_STRIDE) {
        const RayColumn *column = &columns[base_col + i];
        const RayDoorOverlay *door = &column->door;
        if (door->height != 0 && door->depth < column->depth) return TRUE;
    }
    return FALSE;
}

#if (RAY_COL_STRIDE != 4) && (RAY_COL_STRIDE != 2)
#error "build_bsp_tilemap only implements the RAY_COL_STRIDE == 4 and == 2 packers"
#endif

#if RAY_COL_STRIDE == 4
// Stride-4 mixed tile: two sampled columns per tile, each covering two
// adjacent byte lanes. Reuses the stride-2 FREEDOOM_WALL_PACKED_PAIRS table —
// each u8 holds one shaded texel replicated across 2px, so storing it to both
// bytes of the lane pair replicates it across this stride's 4px. Same
// run-splitting structure as the stride-2 C reference below; there is no asm
// hotpath for this stride.
static __attribute__((noinline)) void write_mixed_stride4_tile(
    u32 *tile,
    u16 pixel_y,
    const WallColumnDescriptor descriptors[2],
    const u8 *const packed_columns[2],
    const PackedFlatRows *flat_rows) {
    u8 *const tile_bytes = (u8 *)tile;
    const u8 *const ceiling_bytes = (const u8 *)flat_rows->ceiling;
    const u8 *const floor_bytes = (const u8 *)flat_rows->floor;
    const u16 end_y = (u16)(pixel_y + 8);

    for (u16 lane = 0; lane < 2; lane++) {
        const WallColumnDescriptor *const descriptor = &descriptors[lane];
        const u8 *const packed_column = packed_columns[lane];
        u8 *dst = &tile_bytes[lane * 2];
        u16 y = pixel_y;
        u16 run_end = descriptor->top;
        if (run_end > end_y) run_end = end_y;

        while (y < run_end) {
            const u8 flat = ceiling_bytes[((y & 3) << 2) + (lane * 2)];
            dst[0] = flat;
            dst[1] = flat;
            dst += 4;
            y++;
        }

        if (y < descriptor->top) y = descriptor->top;
        run_end = descriptor->bottom;
        if (run_end > end_y) run_end = end_y;
        while (y < run_end) {
            const u8 pair = packed_column[
                (descriptor->vertical_samples[y - descriptor->top] +
                 descriptor->tex_y) & WALL_TEX_DIM_MASK];
            dst[0] = pair;
            dst[1] = pair;
            dst += 4;
            y++;
        }

        while (y < end_y) {
            const u8 flat = floor_bytes[((y & 3) << 2) + (lane * 2)];
            dst[0] = flat;
            dst[1] = flat;
            dst += 4;
            y++;
        }
    }
}

// Same coherence contract as the stride-2 packer below, with two descriptors
// per tile column (px 0 and 4). No DEBUG_PERF oracles or asm-compare harness
// at this stride — the release cadence probe is the ground truth here.
void build_bsp_tilemap(const RayColumn *columns,
                       const RaySceneColors *scene_colors,
                       u32 target[][8]) {
    const PackedFlatRows flat_rows = build_flat_rows(scene_colors);
    const bool flat_changed = (bool)(!s_coherence_valid ||
                                     !flat_rows_equal(&flat_rows, &s_prev_flat_rows));
    const u32 overlay_columns = renderer_overlay_prev_columns();
    for (u16 tile_x = 0; tile_x < VIEW_TILE_W; tile_x++) {
        const u16 base_col = (u16)(tile_x * 8);
        const WallColumnDescriptor descriptors[2] = {
            describe_wall_column(&columns[base_col]),
            describe_wall_column(&columns[base_col + 4])
        };
        if (!flat_changed && !s_prev_door_active[tile_x] &&
            !(overlay_columns & ((u32)1u << tile_x)) &&
            wall_desc_equal(&descriptors[0], &s_prev_desc[tile_x][0]) &&
            wall_desc_equal(&descriptors[1], &s_prev_desc[tile_x][1]) &&
            !column_door_active(columns, base_col)) {
            continue;
        }
        s_prev_desc[tile_x][0] = descriptors[0];
        s_prev_desc[tile_x][1] = descriptors[1];
        s_prev_door_active[tile_x] = (u8)column_door_active(columns, base_col);

        u16 min_top = descriptors[0].top;
        if (descriptors[1].top < min_top) min_top = descriptors[1].top;
        u16 max_bottom = descriptors[0].bottom;
        if (descriptors[1].bottom > max_bottom) max_bottom = descriptors[1].bottom;

        const u8 *const packed_columns[2] = {
            FREEDOOM_WALL_PACKED_PAIRS[
                (descriptors[0].flags & RAY_COLUMN_FLAG_DOOR) != 0]
                [descriptors[0].shade_level][descriptors[0].texture_id][descriptors[0].tex_x],
            FREEDOOM_WALL_PACKED_PAIRS[
                (descriptors[1].flags & RAY_COLUMN_FLAG_DOOR) != 0]
                [descriptors[1].shade_level][descriptors[1].texture_id][descriptors[1].tex_x]
        };
        for (u16 tile_y = 0; tile_y < VIEW_TILE_H; tile_y++) {
            const u16 tile_index = view_tile_index(tile_x, tile_y);
            const u16 pixel_y = (u16)(tile_y * 8);

            if ((pixel_y + 7) < min_top) {
                write_repeated_flat_tile(target[tile_index], flat_rows.ceiling);
#if CADENCE_STAGE_PROBE
                g_cadence_pack_flat_tiles++;
#endif
            } else if (pixel_y >= max_bottom) {
                write_repeated_flat_tile(target[tile_index], flat_rows.floor);
#if CADENCE_STAGE_PROBE
                g_cadence_pack_flat_tiles++;
#endif
            } else {
                write_mixed_stride4_tile(target[tile_index], pixel_y,
                                         descriptors, packed_columns, &flat_rows);
#if CADENCE_STAGE_PROBE
                g_cadence_pack_mixed_tiles++;
#endif
            }
        }
    }
    s_prev_flat_rows = flat_rows;
    s_coherence_valid = TRUE;
}
#else /* RAY_COL_STRIDE == 2 */
// 0 = ship the hand-written renderer_hotpath.s mixed-tile packer (measured
// -39.6% pack_subticks on checkpoints.txt, 2026-07-21); 1 = use the C
// reference implementation below. The DEBUG_PERF probe byte-verified the asm
// against the C reference on every tile across all routes (asm_mismatches=0)
// before the default flipped. Override with
// EXTRA_FLAGS="-DRENDERER_HOTPATH_C_REFERENCE=1" to fall back / re-verify.
#ifndef RENDERER_HOTPATH_C_REFERENCE
#define RENDERER_HOTPATH_C_REFERENCE 0
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

void build_bsp_tilemap(const RayColumn *columns,
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
        // Skip the whole tile column when its packed output cannot have changed:
        // identical descriptors, unchanged flat rows, and no door RMW to redo
        // (neither this frame nor last). g_view_tiles already holds the correct
        // bytes, and the upload ships them, so this only elides redundant packing.
        // Ordered cheapest-first so the door-overlay rescan (column_door_active,
        // which walks 4 RayColumns) only runs once every cheaper check already
        // passed, instead of unconditionally on every column.
        if (!flat_changed && !s_prev_door_active[tile_x] &&
            !(overlay_columns & ((u32)1u << tile_x)) &&
            wall_desc_equal(&descriptors[0], &s_prev_desc[tile_x][0]) &&
            wall_desc_equal(&descriptors[1], &s_prev_desc[tile_x][1]) &&
            wall_desc_equal(&descriptors[2], &s_prev_desc[tile_x][2]) &&
            wall_desc_equal(&descriptors[3], &s_prev_desc[tile_x][3]) &&
            !column_door_active(columns, base_col)) {
            continue;
        }
#if DEBUG_PERF
        oracle_changed_columns++;
#endif
#if CADENCE_STAGE_PROBE
        g_cadence_pack_columns++;
#endif
        s_prev_desc[tile_x][0] = descriptors[0];
        s_prev_desc[tile_x][1] = descriptors[1];
        s_prev_desc[tile_x][2] = descriptors[2];
        s_prev_desc[tile_x][3] = descriptors[3];
        s_prev_door_active[tile_x] = (u8)column_door_active(columns, base_col);

        // Column-invariant bounds, hoisted out of the 15-tile loop below: a
        // tile is whole-ceiling iff it lies above every descriptor's top
        // (pixel_y+7 < min(top)) and whole-floor iff it lies below every
        // descriptor's bottom (pixel_y >= max(bottom)) — mathematically
        // identical to the original per-tile 4-way min/max, just computed once.
        u16 min_top = descriptors[0].top;
        if (descriptors[1].top < min_top) min_top = descriptors[1].top;
        if (descriptors[2].top < min_top) min_top = descriptors[2].top;
        if (descriptors[3].top < min_top) min_top = descriptors[3].top;
        u16 max_bottom = descriptors[0].bottom;
        if (descriptors[1].bottom > max_bottom) max_bottom = descriptors[1].bottom;
        if (descriptors[2].bottom > max_bottom) max_bottom = descriptors[2].bottom;
        if (descriptors[3].bottom > max_bottom) max_bottom = descriptors[3].bottom;

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

            if ((pixel_y + 7) < min_top) {
#if DEBUG_PERF
                const u32 flat_start = measure_flat ? getSubTick() : 0;
#endif
                write_repeated_flat_tile(target[tile_index], flat_rows.ceiling);
#if CADENCE_STAGE_PROBE
                g_cadence_pack_flat_tiles++;
#endif
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

            if (pixel_y >= max_bottom) {
#if DEBUG_PERF
                const u32 flat_start = measure_flat ? getSubTick() : 0;
#endif
                write_repeated_flat_tile(target[tile_index], flat_rows.floor);
#if CADENCE_STAGE_PROBE
                g_cadence_pack_flat_tiles++;
#endif
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
#if CADENCE_STAGE_PROBE
            g_cadence_pack_mixed_tiles++;
#endif
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
