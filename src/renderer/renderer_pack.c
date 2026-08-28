#include "renderer_pack_internal.h"
#include "debug_checkpoint.h"
#include "renderer_perf.h"
#include "generated_assets.h"

// Wall shading, off by default (see WALL_SHADE_MODE below). When on: walls are
// darkened in discrete steps the farther they are. Level 0 is identity; each
// higher level applies the luminance-derived mapping emitted alongside PAL3, so
// no hand-authored palette indices can shift a hue to blue. N/S ("shade") walls
// add one extra level.
#define SHADE_LEVELS 4
// depth (world units) >> FOG_SHIFT picks the base fog level. Tuned so mid-room
// walls sit around level 1-2 and distant walls saturate at the darkest level.
#define FOG_SHIFT 9
// 2 = distance fog + N/S side shading (Doom-like), 1 = side shading only,
// 0 = flat, every wall at full brightness. Costs nothing either way: the shade
// is baked into FREEDOOM_WALL_PACKED_PAIRS and picked once per column.
// The shade chain never darkens into the ceiling or floor colour (see
// build_shade_map's `reserved` in tools/world_assets.py), so distance fog can
// no longer make a far wall merge into a flat -- it now does the opposite, and
// gives walls the depth cue the flats stopped providing when they went
// level-wide.
#ifndef WALL_SHADE_MODE
#define WALL_SHADE_MODE 2
#endif
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

static const u8 *packed_wall_column(const WallColumnDescriptor *descriptor) {
    if (descriptor->flags & RAY_COLUMN_FLAG_DOOR) {
        const u8 door_index = FREEDOOM_WALL_DOOR_TEXTURE_INDEX[
            descriptor->texture_id];
        if (door_index != 0xFF) {
            return FREEDOOM_WALL_DOOR_PACKED_PAIRS[
                descriptor->shade_level][door_index][descriptor->tex_x];
        }
    }
    return FREEDOOM_WALL_PACKED_PAIRS[
        descriptor->shade_level][descriptor->texture_id][descriptor->tex_x];
}

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
    const u8 (*tex)[WALL_TEX_WIDTH] = FREEDOOM_WALL_TEXTURES[tid];
    // Distance fog + side shading fold into one LUT selection per column: the fog
    // level grows with depth, and N/S ("shade") walls add one extra darkening step.
    // g_shade_luts[0] is the identity, so near front walls are unshaded; every level
    // maps 0 -> 0, preserving transparency, and the inner loop stays branch-free.
#if WALL_SHADE_MODE == 2
    u16 fog_level = (u16)(depth >> FOG_SHIFT) + (side_shade ? 1u : 0u);
    if (fog_level > (SHADE_LEVELS - 1)) {
        fog_level = SHADE_LEVELS - 1;
    }
#elif WALL_SHADE_MODE == 1
    (void)depth;
    const u16 fog_level = side_shade ? 1u : 0u;
#else
    (void)depth;
    (void)side_shade;
    const u16 fog_level = 0u;
#endif
    const u8 *shade_map = g_shade_luts[fog_level];
    const u8 *ty_table = MEGALDOOM_WALL_TEX_Y_BY_HEIGHT[wall_h];
    const u8 tex_x = (u8)(tex_x_value & WALL_TEX_WIDTH_MASK);
    const u8 texture_height = (u8)FREEDOOM_WALL_TEXTURE_HEIGHT[tid];
    const u16 v_scale_q12 = FREEDOOM_WALL_TEXTURE_VSCALE_Q12[tid];

#if RAY_COL_STRIDE == 4
    return (WallColumnDescriptor){top, bottom, (const u8 *)tex, shade_map, ty_table,
                                  tex_x, tex_y_value, tid, (u8)fog_level, flags,
                                  texture_height, v_scale_q12};
#else
    return (WallColumnDescriptor){top, bottom, (const u8 *)tex, shade_map, ty_table,
                                  tex_x, tex_y_value, tid, (u8)fog_level, flags,
                                  texture_height, v_scale_q12};
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
                wall_packed_y(descriptor, (u16)(y - descriptor->top))];
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
            packed_wall_column(&descriptors[0]),
            packed_wall_column(&descriptors[1])
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

void renderer_write_mixed_stride2_span_asm(
    u32 *tiles,
    u16 pixel_y,
    u16 row_count,
    const WallColumnDescriptor descriptors[4],
    const u8 *const packed_columns[4],
    const PackedFlatRows *flat_rows);

#if DEBUG_PERF
#define ASM_PROBE_CANARY_A 0x51A7C0DEu
#define ASM_PROBE_CANARY_B 0xC001D00Du
typedef struct {
    u32 before[2];
    u32 tiles[VIEW_TILE_H][8];
    u32 after[2];
} AsmColumnProbe;

static AsmColumnProbe g_asm_col_probe;
static AsmColumnProbe g_c_col_probe;
static u16 g_asm_compare_cursor;

static void write_mixed_stride2_tile_reference(
    u32 *tile, u16 pixel_y,
    const WallColumnDescriptor descriptors[4],
    const u8 *const packed_columns[4],
    const PackedFlatRows *flat_rows);

static void probe_arm(AsmColumnProbe *probe) {
    probe->before[0] = ASM_PROBE_CANARY_A;
    probe->before[1] = ASM_PROBE_CANARY_B;
    probe->after[0] = ASM_PROBE_CANARY_B;
    probe->after[1] = ASM_PROBE_CANARY_A;
    for (u16 t = 0; t < VIEW_TILE_H; t++) {
        for (u16 row = 0; row < 8; row++) probe->tiles[t][row] = 0xA5A5A5A5u;
    }
}

static bool probe_canary_broken(const AsmColumnProbe *probe) {
    return (bool)(probe->before[0] != ASM_PROBE_CANARY_A ||
                  probe->before[1] != ASM_PROBE_CANARY_B ||
                  probe->after[0] != ASM_PROBE_CANARY_B ||
                  probe->after[1] != ASM_PROBE_CANARY_A);
}

// Check one tile column per rebuilt frame; a route covers all 20 without paying
// for a second framebuffer every frame. Both implementations write only into
// guarded scratch blocks, so the displayed framebuffer is untouched either way.
//
// Two properties are checked at once, and the second is the whole reason the
// harness moved from a tile to a column:
//   1. the asm agrees with the C reference, and
//   2. ONE asm call spanning N*8 rows equals N separate 8-row tile writes --
//      i.e. that the stride-4 walk really is blind to the tile boundary.
// The asm side runs as a single span; the C side is built tile by tile from the
// per-tile reference, which is exactly the concatenation the span must equal.
//
// It also runs BOTH implementations locally rather than comparing against the
// framebuffer the packer already produced. Taking the latter as the C side
// silently stopped being a differential the moment RENDERER_HOTPATH_C_REFERENCE
// defaulted to 0 and the shipped writer became the asm itself: from then on it
// compared asm against asm and could not report a mismatch whatever the asm
// did. Keep both sides computed here so which implementation ships cannot
// disarm the check.
static void compare_stride2_column_asm(u16 tile_x,
                                       u16 first_tile_y,
                                       u16 mixed_tiles,
                                       const WallColumnDescriptor descriptors[4],
                                       const u8 *const packed_columns[4],
                                       const PackedFlatRows *flat_rows) {
    bool mismatch = FALSE;
    bool canary_failure;
    bool completed_cycle;

    if (tile_x != g_asm_compare_cursor) return;
    probe_arm(&g_asm_col_probe);
    probe_arm(&g_c_col_probe);

    if (mixed_tiles != 0) {
        const u16 first_pixel_y = (u16)(first_tile_y * 8);
        renderer_write_mixed_stride2_span_asm(
            &g_asm_col_probe.tiles[0][0], first_pixel_y,
            (u16)(mixed_tiles * 8), descriptors, packed_columns, flat_rows);
        for (u16 t = 0; t < mixed_tiles; t++) {
            write_mixed_stride2_tile_reference(
                g_c_col_probe.tiles[t], (u16)(first_pixel_y + (t * 8)),
                descriptors, packed_columns, flat_rows);
        }
        for (u16 t = 0; t < mixed_tiles; t++) {
            for (u16 row = 0; row < 8; row++) {
                if (g_asm_col_probe.tiles[t][row] != g_c_col_probe.tiles[t][row]) {
                mismatch = TRUE;
            }
        }
    }
    }
    canary_failure = (bool)(probe_canary_broken(&g_asm_col_probe) ||
                            probe_canary_broken(&g_c_col_probe));
    completed_cycle = (bool)(g_asm_compare_cursor == (VIEW_TILE_W - 1));
    renderer_perf_record_asm_compare(tile_x, mismatch, canary_failure,
                                     completed_cycle);
    g_asm_compare_cursor++;
    if (g_asm_compare_cursor == VIEW_TILE_W) g_asm_compare_cursor = 0;
}
#endif

// A mixed tile used to resolve four columns for each row, then shift/OR four
// bytes into a u32. On the big-endian 68000 the four packed pairs are already
// the four bytes of that u32, so write each lane directly. Splitting each lane
// into ceiling/wall/floor runs removes the four per-row branches and all of the
// long shifts from the hottest packing path.
#if DEBUG_PERF || RENDERER_HOTPATH_C_REFERENCE
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
                wall_packed_y(descriptor, (u16)(y - descriptor->top))];
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

// The C fallback for a whole run of tiles is literally the per-tile reference
// applied tile by tile -- that is the definition the asm span has to match.
static void write_mixed_stride2_span_reference(
    u32 *tiles,
    u16 pixel_y,
    u16 row_count,
    const WallColumnDescriptor descriptors[4],
    const u8 *const packed_columns[4],
    const PackedFlatRows *flat_rows) {
    for (u16 t = 0; t < (u16)(row_count / 8); t++) {
        write_mixed_stride2_tile_reference(&tiles[t * 8],
                                           (u16)(pixel_y + (t * 8)),
                                           descriptors, packed_columns, flat_rows);
    }
}
#endif

// Which implementation actually fills the framebuffer. Kept separate from the
// guard above on purpose: a DEBUG_PERF build compiles the reference in so the
// harness can run it, but must still ship the asm, or the probe would be
// measuring and comparing the C path against itself.
#if RENDERER_HOTPATH_C_REFERENCE
#define write_mixed_stride2_span write_mixed_stride2_span_reference
#else
#define write_mixed_stride2_span renderer_write_mixed_stride2_span_asm
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
#if CADENCE_PACK_SPLIT
        const u32 desc_start = getSubTick();
#endif
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
#if CADENCE_PACK_SPLIT
            g_cadence_pack_desc_subticks += getSubTick() - desc_start;
#endif
            continue;
        }
#if CADENCE_PACK_SPLIT
        g_cadence_pack_desc_subticks += getSubTick() - desc_start;
#endif
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
            packed_wall_column(&descriptors[0]),
            packed_wall_column(&descriptors[1]),
            packed_wall_column(&descriptors[2]),
            packed_wall_column(&descriptors[3])
        };
#if CADENCE_PACK_SPLIT
        const u32 tiles_start = getSubTick();
#endif
        // The three tile classes form contiguous runs, so they are sliced once
        // instead of re-deciding per tile. A tile is whole-ceiling iff
        // 8*tile_y + 7 < min_top, which is exactly tile_y < min_top / 8; it is
        // whole-floor iff 8*tile_y >= max_bottom, exactly
        // tile_y >= (max_bottom + 7) / 8. min_top <= max_bottom always (every
        // top <= its own bottom), so the mixed run in between never inverts.
        const u16 ceiling_tiles = (u16)(min_top / 8);
        u16 first_floor_tile = (u16)((max_bottom + 7) / 8);
        if (first_floor_tile > VIEW_TILE_H) first_floor_tile = VIEW_TILE_H;
        const u16 mixed_tiles = (u16)(first_floor_tile - ceiling_tiles);

        for (u16 tile_y = 0; tile_y < ceiling_tiles; tile_y++) {
#if DEBUG_PERF
            const u32 flat_start = measure_flat ? getSubTick() : 0;
#endif
            write_repeated_flat_tile(target[view_tile_index(tile_x, tile_y)],
                                     flat_rows.ceiling);
#if CADENCE_STAGE_PROBE
            g_cadence_pack_flat_tiles++;
#endif
#if DEBUG_PERF
            sparse_ceiling++;
            if (measure_flat) {
                renderer_perf_record_deep(RENDERER_PERF_DEEP_PACK_FLAT,
                                          getSubTick() - flat_start, 1);
            }
#endif
        }

        // One call for the whole wall run: see renderer_hotpath.s on why a
        // column's tiles are contiguous and row y of lane L lands at 4*y + L,
        // which makes the tile boundary invisible to the stride-4 walk.
        if (mixed_tiles != 0) {
#if DEBUG_PERF
            const u32 mixed_start = measure_mixed ? getSubTick() : 0;
#endif
            write_mixed_stride2_span(target[view_tile_index(tile_x, ceiling_tiles)],
                                     (u16)(ceiling_tiles * 8),
                                     (u16)(mixed_tiles * 8),
                                     descriptors, packed_columns, &flat_rows);
#if CADENCE_STAGE_PROBE
            g_cadence_pack_mixed_tiles += mixed_tiles;
#endif
#if DEBUG_PERF
            sparse_dyn_wall = (u16)(sparse_dyn_wall + mixed_tiles);
            if (measure_mixed) {
                renderer_perf_record_deep(RENDERER_PERF_DEEP_PACK_MIXED,
                                          getSubTick() - mixed_start, mixed_tiles);
            }
#endif
        }

        for (u16 tile_y = first_floor_tile; tile_y < VIEW_TILE_H; tile_y++) {
#if DEBUG_PERF
            const u32 flat_start = measure_flat ? getSubTick() : 0;
#endif
            write_repeated_flat_tile(target[view_tile_index(tile_x, tile_y)],
                                     flat_rows.floor);
#if CADENCE_STAGE_PROBE
            g_cadence_pack_flat_tiles++;
#endif
#if DEBUG_PERF
            sparse_floor++;
            if (measure_flat) {
                renderer_perf_record_deep(RENDERER_PERF_DEEP_PACK_FLAT,
                                          getSubTick() - flat_start, 1);
            }
#endif
        }
#if DEBUG_PERF
        compare_stride2_column_asm(tile_x, ceiling_tiles, mixed_tiles,
                                   descriptors, packed_columns, &flat_rows);
#endif
#if CADENCE_PACK_SPLIT
        g_cadence_pack_tiles_subticks += getSubTick() - tiles_start;
#endif
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
