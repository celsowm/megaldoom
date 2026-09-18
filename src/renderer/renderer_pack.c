#include "renderer_pack_internal.h"
#include "debug_checkpoint.h"
#include "renderer_perf.h"
#include "generated_assets.h"
#include "level_bank.h"
#include "generated_wall_scalers.h"

// Wall shading, off by default (see WALL_SHADE_MODE below). When on: walls are
// darkened in discrete steps the farther they are. Level 0 is identity; each
// higher level applies the luminance-derived mapping emitted alongside PAL3, so
// no hand-authored palette indices can shift a hue to blue. N/S ("shade") walls
// add one extra level.
//
// The level count is a property of the baked asset, not of the renderer: it is
// how many shade planes tools/world_assets.py emitted into
// the level wall packs. Deriving it here means changing the bake cannot
// leave the runtime indexing planes that were never generated.
#define SHADE_LEVELS FREEDOOM_WORLD_SHADE_LEVELS
// depth (world units) >> FOG_SHIFT picks the base fog level. Tuned so mid-room
// walls sit around level 1-2 and distant walls saturate at the darkest level.
#define FOG_SHIFT 9
// 2 = distance fog + N/S side shading (Doom-like), 1 = side shading only,
// 0 = flat, every wall at full brightness. Costs nothing either way: the shade
// is baked into the level wall packs and picked once per column.
// The shade chain never darkens into the ceiling or floor colour (see
// build_shade_map's `reserved` in tools/world_assets.py), so distance fog can
// no longer make a far wall merge into a flat -- it now does the opposite, and
// gives walls the depth cue the flats stopped providing when they went
// level-wide.
#ifndef WALL_SHADE_MODE
#define WALL_SHADE_MODE 2
#endif
// Pack-stage tile-column coherence: when the four wall descriptors feeding a
// tile column are byte-identical to the previous base build, its 15 packed
// tiles are unchanged and re-packing them is pure waste. FALSE forces a full
// repack next build; every discontinuity (init, level reset, menu return, or
// anything that rebuilds the CPU tile buffer from scratch) must clear it so we
// never skip against stale cached descriptors. See build_bsp_tilemap().
static bool s_coherence_valid = FALSE;

_Static_assert(WALL_TEX_HEIGHT == 1 << 7 && WALL_TEX_WIDTH == 1 << 6 &&
               FREEDOOM_WORLD_SHADE_LEVELS == 4 &&
               MEGALDOOM_LEVEL_PACK_BLOCK_BYTES == 1L << 15,
               "packed_wall_column's shifts encode the pack block layout");

// describe_textured_column selects a clip-delta row with VIEW_PIXEL_H >> 7,
// which is 0 for a 120-row viewport and 1 for a 128-row one. A preset of any
// other pixel height would silently pick the wrong row and mis-sample every
// clipped wall, so pin the presets here rather than trusting the shift.
_Static_assert((RAY_VIEW_SIZE_0_H * 8) == 120 && (RAY_VIEW_SIZE_1_H * 8) == 120 &&
               (RAY_VIEW_SIZE_2_H * 8) == 128 && RAY_VIEW_SIZE_COUNT == 3 &&
               MEGALDOOM_WALL_SAMPLE_ROWS == (RAY_VIEW_TILE_H_MAX * 8),
               "MEGALDOOM_WALL_CLIP_DELTA is indexed by VIEW_PIXEL_H >> 7; only "
               "120- and 128-row viewports are representable");

// Reads the banked pack of the loaded level (src/bsp/level_bank.c). A block is
// [shade][tex_x][tex_y] with 64 columns of 128 rows, so a column starts at
// (shade * 64 + tex_x) * 128 -- the offset the old monolithic
// [shade][texture][tex_x] table had within one texture.
const u8 *packed_wall_column(const WallColumnDescriptor *descriptor) {
    const u32 column = ((u32)descriptor->shade_level << 13) +
                       ((u32)descriptor->tex_x << 7);
    if (descriptor->flags & RAY_COLUMN_FLAG_DOOR) {
        const u8 *const door = g_level_door_bases[descriptor->texture_id];
        if (door != NULL) {
            return door + column;
        }
    }
    return g_level_wall_bases[descriptor->texture_id] + column;
}

#if PERF_FIXED_POSE
// Pose-locked harness only (see debug_checkpoint.h). A pinned camera would make
// the coherence check skip all 20 tile columns and report pack as free; motion
// repacks every column, so drop the cache each frame to measure that instead.
// Deliberately NOT pack_stage_reset(): rebuilding the shade LUTs and the flat
// rows would time work a real motion frame does not do.
void pack_stage_invalidate_coherence(void) {
    s_coherence_valid = FALSE;
}
#endif

void pack_stage_reset(void) {
    flat_rows_invalidate();
    s_coherence_valid = FALSE;
}

// The clamped projected span the vertical DDA table is indexed by.
static inline u16 column_sample_height(u16 projected_wall_h) {
    return (projected_wall_h > RAY_MAX_PROJECTED_WALL_HEIGHT) ?
               RAY_MAX_PROJECTED_WALL_HEIGHT :
           (projected_wall_h < 1 ? 1 : projected_wall_h);
}

// A column's screen bounds. Shared by describe_textured_column() and
// wall_column_top() so there is exactly one expression for where a column
// starts -- the window compositor reads that `top` to find where the far
// geometry's ceiling ends, and a second copy of this arithmetic is precisely
// the kind of thing that drifts.
//
// `full_top` is the UNSHORTENED slab's top. A floor-aligned column (a low
// courtyard parapet with sky above it) is only the visible lower portion of a
// full projected slab, so its DDA has to skip the omitted rows; the caller
// advances by top - full_top. For a centred column full_top == top, so that
// advance is zero and this stays the plain centring it always was.
static inline void column_slab_bounds(u16 wall_h, u16 sample_height, u8 flags,
                                      u16 *top, u16 *bottom, u16 *full_top) {
    if (flags & RAY_COLUMN_FLAG_FLOOR_ALIGNED) {
        // Reconstruct the complete slab's clipped bottom and anchor the wall
        // there. This preserves clipping at point-blank range while leaving
        // sky above the wall at distance.
        const u16 full_visible_height =
            (sample_height > VIEW_PIXEL_H) ? VIEW_PIXEL_H : sample_height;
        *full_top = (u16)((VIEW_PIXEL_H - full_visible_height) / 2);
        *bottom = (u16)(*full_top + full_visible_height);
        *top = (wall_h < *bottom) ? (u16)(*bottom - wall_h) : 0;
    } else {
        *top = (u16)((VIEW_PIXEL_H - wall_h) / 2);
        *full_top = *top;
        *bottom = (u16)(*top + wall_h);
    }
}

static WallColumnDescriptor describe_textured_column(u16 wall_h,
                                                     u16 projected_wall_h,
                                                     u16 depth,
                                                     u8 texture_id,
                                                     u8 tex_x_value,
                                                     u8 tex_y_value,
                                                     u8 side_shade,
                                                     u8 flags) {
    u16 top;
    u16 bottom;
    const u8 tid = (u8)((texture_id < FREEDOOM_WALL_TEXTURE_COUNT) ?
                            texture_id : MEGALDOOM_TEX_FALLBACK);
    // Distance fog + side shading fold into one plane selection per column: the
    // fog level grows with depth, and N/S ("shade") walls add one extra
    // darkening step. Level 0 is the unshaded bake, so near front walls are
    // untouched; the planes are pre-shaded at bake time (see
    // world_assets.build_shade_planes), so the inner loop stays branch-free.
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
    // wall_h is the visible span after viewport clipping. The texture lookup
    // must use the unclipped projected span, otherwise a near wall/closed door
    // remaps its entire 128-row texture into the 120 visible rows.
    const u16 sample_height = column_sample_height(projected_wall_h);
    // The table's centring clip is baked for the TALLEST viewport (128 rows);
    // a 120-row viewport clips 4 rows less off a tall wall, so it starts that
    // much further into the same row. Zero for every unclipped column, and zero
    // for every column at the 128-row preset.
    const u8 clip_delta = MEGALDOOM_WALL_CLIP_DELTA[VIEW_PIXEL_H >> 7][sample_height];
    const u8 *ty_table = MEGALDOOM_WALL_TEX_Y_BY_HEIGHT[sample_height] + clip_delta;
    u16 full_top;
    column_slab_bounds(wall_h, sample_height, flags, &top, &bottom, &full_top);
    // Advance the DDA past the rows a floor-aligned wall omits above its top.
    // column_slab_bounds reports full_top == top for a centred column, so this
    // is a +0 for every ordinary wall.
    ty_table += top - full_top;
    const u8 tex_x = (u8)(tex_x_value & WALL_TEX_WIDTH_MASK);
    // A generated scaler is the column's whole wall unrolled: it writes rows
    // wall_h-1..0 of MEGALDOOM_WALL_TEX_Y_BY_HEIGHT[sample_height] at a fixed
    // tex_y. Every assumption it makes is decided here, once per column:
    //   * centred, so its DDA starts at row 0 (top == full_top);
    //   * the column's clip delta is the one the routines were baked with
    //     (the 120-row viewport's, tools/gen_wall_scalers.py): true for every
    //     column at a 120-row preset, and for the unclipped ones at 128 rows,
    //     where both deltas are 0. A 128-row preset's CLIPPED columns start
    //     4 rows earlier in the table than the routines assume, so they keep
    //     the generic post;
    //   * within the table's own rows (see tools/gen_wall_scalers.py).
    // tex_y is folded into the column pointer, and a read that wraps past the
    // texture height is renderer_hotpath.s's business, not this function's:
    // it tests the routine's own last sample and splits the column there.
    // Until 2026-09-18 a wrapping column kept the generic post (33-100% of
    // wall rows at six of the 17 sweep poses), and the MAX_TY lookup that
    // decided it cost every column. tex_y < 128 is what the asm's split
    // assumes (tex_y + sample < 256); every non-floor-aligned column has it.
    // Anything else keeps the generic post.
    // MEGALDOOM_NO_WALL_SCALERS=1 forces every column onto the generic post, so
    // a sweep can measure the scalers against an otherwise byte-identical build
    // at the same pose. Measurement only; never define it in a shipping build.
    u16 scaler_height = 0;
#if !MEGALDOOM_NO_WALL_SCALERS
    // Ordered cheapest-first: `rows` is already in registers, the clip-delta
    // load is not. (The tex_y-wrap clause and its MAX_TY load moved into the
    // asm dispatch on 2026-09-18.) Three clauses that used to be here are gone because they
    // cannot fail -- `rows <= sample_height` (height is min(projected,
    // RAY_VIEW_ROWS) and projected_height is min(projected, 640), and
    // RAY_VIEW_ROWS <= 128 < 640) and `sample_height < SCALER_HEIGHTS`
    // (column_sample_height clamps to 640, and HEIGHTS is 641). A predicate
    // that can never be false is not a safety net, it is per-column cost.
    if (!(flags & RAY_COLUMN_FLAG_FLOOR_ALIGNED)) {
        const u16 rows = (u16)(bottom - top);
        if (rows != 0 && rows <= MEGALDOOM_WALL_SCALER_ROWS &&
            clip_delta == MEGALDOOM_WALL_CLIP_DELTA[0][sample_height] &&
            tex_y_value < WALL_TEX_HEIGHT) {
            scaler_height = sample_height;
#if MEGALDOOM_NO_SCALER_WRAP
            // A/B baseline only: the pre-2026-09-18 rule, generic post.
            if ((u16)(tex_y_value + MEGALDOOM_WALL_SCALER_MAX_TY[sample_height]) >=
                WALL_TEX_HEIGHT) {
                scaler_height = 0;
            }
#endif
        }
    }
#endif

    return (WallColumnDescriptor){top, bottom, ty_table,
                                  tex_x, tex_y_value, tid, (u8)fog_level, flags,
                                  scaler_height};
}

WallColumnDescriptor describe_wall_column(const RayColumn *column) {
#if CADENCE_WALL_REASONS
    const WallColumnDescriptor d = describe_textured_column(
        column->height, column->projected_height, column->depth,
        column->texture_id, column->tex_x, column->tex_y, column->shade,
        column->flags);
    // Why this column's wall rows do or do not take a generated scaler, in
    // the eligibility test's own order (first failing clause wins).
    const u16 rows = (u16)(d.bottom - d.top);
    const u16 s = column_sample_height(column->projected_height);
    u16 reason;
    if (d.scaler_height != 0) {
        reason = ((u16)(d.tex_y + MEGALDOOM_WALL_SCALER_MAX_TY[d.scaler_height]) >=
                  WALL_TEX_HEIGHT) ? CADENCE_WALL_SCALER_WRAPPED : CADENCE_WALL_SCALER;
    }
    else if (rows == 0) reason = CADENCE_WALL_EMPTY;
    else if (d.flags & RAY_COLUMN_FLAG_FLOOR_ALIGNED) reason = CADENCE_WALL_FLOOR_ALIGNED;
    else if (rows > MEGALDOOM_WALL_SCALER_ROWS) reason = CADENCE_WALL_TOO_TALL;
    else if (MEGALDOOM_WALL_CLIP_DELTA[VIEW_PIXEL_H >> 7][s] !=
             MEGALDOOM_WALL_CLIP_DELTA[0][s]) reason = CADENCE_WALL_CLIP_DELTA;
    else reason = CADENCE_WALL_OTHER;
    g_cadence_wall_rows[reason] += rows;
    return d;
#else
    return describe_textured_column(column->height, column->projected_height,
                                    column->depth,
                                    column->texture_id, column->tex_x,
                                    column->tex_y, column->shade, column->flags);
#endif
}

// describe_wall_column(column).top without the descriptor: no
// MEGALDOOM_WALL_TEX_Y_BY_HEIGHT[641][120] index, no fog level, no texture
// height / v-scale lookups, no 20-byte struct returned by value. The window
// compositor calls this once per overlay column on a floor-aligned wall.
u16 wall_column_top(const RayColumn *column) {
    u16 top;
    u16 bottom;
    u16 full_top;
    column_slab_bounds(column->height,
                       column_sample_height(column->projected_height),
                       column->flags, &top, &bottom, &full_top);
    return top;
}

WallColumnDescriptor describe_door_overlay(const RayDoorOverlay *door) {
    // Windows and camouflaged SECRET doors keep ordinary wall material. Only
    // an ordinary door gets the framed interactive silhouette baked for it.
    return describe_textured_column(door->height, door->height,
                                    door->depth,
                                    door->texture_id, door->tex_x,
                                    door->tex_y, door->shade,
                                    (ray_overlay_is_window(door) ||
                                     ray_overlay_is_plain_door(door)) ?
                                        0u : RAY_COLUMN_FLAG_DOOR);
}

// PACK_LANES (sampled cast columns per 8px tile column: 4 at stride 2, 2 at
// stride 4) is defined in renderer_pack_abi.h, which the asm hotpath shares.

// Previous base build's per-tile-column packing inputs, for coherence skipping.
static WallColumnDescriptor s_prev_desc[RAY_VIEW_TILE_W_MAX][PACK_LANES];
static RaySceneColors s_prev_scene_flats;
static u8 s_prev_door_active[RAY_VIEW_TILE_W_MAX];

// A tile column's 15 packed tiles are a pure function of its wall descriptors
// and the shared flat rows, so field-wise equality of those descriptors is
// sufficient to prove the packed output is unchanged. (Compared by field
// rather than memcmp so the struct's padding byte cannot spuriously force a
// repack.)
//
// `texture` and `shade_map` are pure functions of fields already compared
// below. `vertical_samples` is also generated data, but it depends on the
// unclipped projected height, which is intentionally not present in the ABI
// descriptor; compare the pointer so a near wall cannot reuse a cache entry
// with the same visible 120px bounds but a different texture slice.
static inline bool wall_desc_equal(const WallColumnDescriptor *a,
                                   const WallColumnDescriptor *b) {
    return (bool)(a->top == b->top && a->bottom == b->bottom &&
                  a->vertical_samples == b->vertical_samples &&
                  a->tex_x == b->tex_x && a->tex_y == b->tex_y &&
                  a->texture_id == b->texture_id &&
                  a->shade_level == b->shade_level && a->flags == b->flags);
}

// A column carries an active door overlay when any of its sampled columns
// has a door in front of the wall. draw_door_overlays() (run after packing)
// read-modify-writes those tiles every frame and only rewrites the door's own
// pixel span, so a column with a door now, or one last frame, must be repacked:
// otherwise the wall behind a lifting door would keep stale door pixels in the
// newly revealed gap. Mirrors the guard in draw_door_overlays().
static inline bool column_door_active(const RayColumn *columns, u16 base_sample) {
    for (u16 i = 0; i < RAY_TILE_SAMPLES; i++) {
        const RayColumn *column = &columns[base_sample + i];
        const RayDoorOverlay *door = &column->door;
        if (door->height != 0 && door->depth < column->depth) return TRUE;
    }
    return FALSE;
}

// Only the stride-2 packer exists. RAY_COL_STRIDE is fixed at 2
// (src/raycast.h); the stride-4 comparison packer was deleted 2026-09-18,
// having been dead since the stride-4 revert (LOG, 2026-07-27).
_Static_assert(RAY_COL_STRIDE == 2, "build_bsp_tilemap only implements the stride-2 packer");

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

#if RENDERER_ASM_DIFF_ENABLED
#define ASM_PROBE_CANARY_A 0x51A7C0DEu
#define ASM_PROBE_CANARY_B 0xC001D00Du
typedef struct {
    u32 before[2];
    u32 tiles[RAY_VIEW_TILE_H_MAX][8];
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
#endif /* RENDERER_ASM_DIFF_ENABLED */

// A mixed tile used to resolve four columns for each row, then shift/OR four
// bytes into a u32. On the big-endian 68000 the four packed pairs are already
// the four bytes of that u32, so write each lane directly. Splitting each lane
// into ceiling/wall/floor runs removes the four per-row branches and all of the
// long shifts from the hottest packing path.
#if RENDERER_ASM_DIFF_ENABLED || RENDERER_HOTPATH_C_REFERENCE
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
            *dst = ceiling_bytes[((y & (PACK_CEILING_ROW_COUNT - 1)) << 2) + lane];
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
// guard above on purpose: a RENDERER_ASM_DIFF build compiles the reference in
// so the harness can run it, but must still ship the asm, or the probe would be
// measuring and comparing the C path against itself.
#if RENDERER_HOTPATH_C_REFERENCE
#define write_mixed_stride2_span write_mixed_stride2_span_reference
#else
#define write_mixed_stride2_span renderer_write_mixed_stride2_span_asm
#endif

void build_bsp_tilemap(const RayColumn *columns,
                                  const RaySceneColors *scene_colors,
                                  u32 target[][8]) {
    PackedFlatRows *const flat_rows = build_flat_rows(scene_colors);
    // Ceiling/floor colour changes (e.g. lighting) invalidate every column at
    // once; otherwise coherence is decided per column below. Under a sky that
    // includes a heading change, because scene_flats_equal folds sky_offset in
    // whenever `sky` is set -- see the note there.
    const bool flat_changed = (bool)(!s_coherence_valid ||
                                     !scene_flats_equal(scene_colors, &s_prev_scene_flats));
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
        const u16 base_sample = (u16)(tile_x * RAY_TILE_SAMPLES);
#if CADENCE_PACK_SPLIT
        const u32 desc_start = getSubTick();
#endif
        const WallColumnDescriptor descriptors[4] = {
            describe_wall_column(&columns[base_sample]),
            describe_wall_column(&columns[base_sample + 1]),
            describe_wall_column(&columns[base_sample + 2]),
            describe_wall_column(&columns[base_sample + 3])
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
            !column_door_active(columns, base_sample)) {
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
        s_prev_door_active[tile_x] = (u8)column_door_active(columns, base_sample);

        // Under a sky the ceiling is 2D: point the (row-indexed) ceiling table
        // at this tile's own sky column, so the horizon has horizontal
        // structure and slides with the heading. One pointer store per repacked
        // column -- the ceiling post and the asm are unchanged, they just read a
        // different base. Indoors the table is heading-invariant and this is
        // skipped entirely.
        if (scene_colors->sky) {
            flat_rows->ceiling = sky_column_rows(tile_x, scene_colors->sky_offset);
        }

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
            write_ceiling_tile(target[view_tile_index(tile_x, tile_y)],
                               flat_rows->ceiling, (u16)(tile_y * 8));
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
                                     descriptors, packed_columns, flat_rows);
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
                                     flat_rows->floor);
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
#if RENDERER_ASM_DIFF_ENABLED
        {
            // Timed separately and subtracted back out of pack_subticks: this
            // call exists only to verify the asm packer, not to build the
            // frame, and its cost is not something a real build ever pays.
            const u32 asm_cmp_start = getSubTick();
            compare_stride2_column_asm(tile_x, ceiling_tiles, mixed_tiles,
                                       descriptors, packed_columns, flat_rows);
            renderer_perf_add_asm_compare_overhead(getSubTick() - asm_cmp_start);
        }
#endif
#if CADENCE_PACK_SPLIT
        g_cadence_pack_tiles_subticks += getSubTick() - tiles_start;
#endif
    }
    s_prev_scene_flats = *scene_colors;
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
