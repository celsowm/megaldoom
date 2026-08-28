#include "bsp_render.h"
#include "bsp_map.h"
#include "fixed_math.h"
#include "generated_assets.h"
#include "renderer_perf.h"
#include "bsp_inv_depth_lut.h"
#include "debug_checkpoint.h"

// draw_seg only computes the height/texture fields at columns on a RAY_COL_STRIDE
// boundary, matching the columns build_bsp_tilemap actually samples. That
// relies on the stride being a power of two (the (x & (STRIDE-1)) test).
#if (RAY_COL_STRIDE & (RAY_COL_STRIDE - 1)) != 0
#error "draw_seg's strided-column optimization requires RAY_COL_STRIDE to be a power of two"
#endif

// --- Projection constants ---------------------------------------------------
// View is RAY_VIEW_COLS (160) px wide; the 80px focal length maps
// |lateral/depth| = 1 to either edge, producing a 90-degree horizontal FOV.
// Division-free frustum-rejection half-plane scales. screen = center +
// perspective_divide(PROJ*lateral, depth); perspective_divide truncates toward
// zero, so the linear plane tests below are a conservative superset of the old
// screen-range reject: a box rejected here would also be rejected after the four
// divisions (never the reverse). LEFT gains +1 because the old left test is a
// strict "max_screen < 0"; RIGHT needs no margin because the old right test is a
// non-strict "min_screen >= RAY_VIEW_COLS" whose integer threshold aligns with
// toward-zero truncation of positive projections.
#define LEFT_REJECT_SCALE (RAY_VIEW_CENTER_X + RAY_COL_STRIDE + 1)
#define RIGHT_REJECT_SCALE (RAY_VIEW_COLS + RAY_COL_STRIDE - RAY_VIEW_CENTER_X)
#define BSP_NEAR 16          // near clip plane, in world units
#define BSP_INV_SCALE 16384  // fixed-point scale for 1/depth interpolation (1<<14)
#define BSP_SAMPLE_COLS (RAY_VIEW_COLS / RAY_COL_STRIDE)
#define BSP_SOLID_WORD_COUNT ((BSP_SAMPLE_COLS + 31) / 32)

// Per-frame view basis + camera, set at the top of bsp_cast_frame().
static s16 g_fwx, g_fwy; // forward axis (cos, sin), Q8
static s16 g_rx, g_ry;   // right axis, Q8
static s32 g_px, g_py;   // camera position, world units

// Successor set for open samples. Once a solid wall claims a sample, its entry
// points at the next still-open sample, so farther overlapping segments skip
// closed runs instead of walking them column by column. The final entry is a
// sentinel one past the view.
static u8 g_next_open[BSP_SAMPLE_COLS + 1];
static u32 g_solid_words[BSP_SOLID_WORD_COUNT];
static u16 g_solid_count;

// Seg endpoints are heavily shared. Cache their view-space transform for the
// current cast so each distinct vertex pays four MULS.W only once. A byte
// generation avoids clearing the 512-entry depth/lateral arrays every frame.
static s16 g_vertex_depth[BSP_MAX_VERTICES];
static s16 g_vertex_lateral[BSP_MAX_VERTICES];
static u8 g_vertex_generation[BSP_MAX_VERTICES];
static u8 g_cast_generation;
static RayColumn *g_columns;

// Far/empty default for a sampled column that no wall claimed this frame.
// g_ray_columns persists across frames, so an unclaimed column would otherwise
// show last frame's wall; this is what the old unconditional pre-seed provided
// for all 80 samples. See seed_unclaimed_columns for why it is now applied only
// to the complement of the solid set.
static void seed_column_default(RayColumn *col) {
    col->height = 1;
    col->depth = 0x7FFF;
    col->tex_x = 0;
    col->tex_y = 0;
    col->texture_id = MEGALDOOM_TEX_FALLBACK;
    col->shade = 0;
    col->flags = 0;
}

// --- Near/far order cache (Patch 3.2) ---------------------------------------
// The front/back traversal order at each BSP node depends only on the player's
// position relative to the partition line — not on the camera angle. During
// pure rotation the position is unchanged, so this cache lets render_node skip
// the per-node cross product (two s32 multiplies + subtract) entirely. One bit
// per node: 1 = front-first (cross >= 0), 0 = back-first. Rebuilt only when
// player.x or player.y changes; invalidated explicitly on teleport/reset.
static u8 g_node_side_bits[(BSP_MAX_NODES + 7) / 8];
/* Position generations only distinguish the current cached pose.  Eight bits
 * are sufficient; the existing wrap clear prevents stale equality. */
static u8 g_node_side_generation[BSP_MAX_NODES];
static u8 g_position_generation;
static s32 g_node_cache_px;
static s32 g_node_cache_py;
static bool g_node_cache_valid;

// Temporary BSP instrumentation: counts how the new division-free frustum
// precheck interacts with traversal. Nodes visited, boxes rejected cheaply
// (by the half-plane test, avoiding 4 divisions), boxes that survived to the
// 4-division projection path, and near-plane fallbacks (expanded to whole
// view). Reset each frame in bsp_cast_frame; read by the DEBUG_PERF overlay.
#if DEBUG_PERF
static u16 g_bsp_dbg_nodes_visited;
static u16 g_bsp_dbg_boxes_rejected_cheap;
static u16 g_bsp_dbg_boxes_projected;
static u16 g_bsp_dbg_near_fallbacks;
static u16 g_bsp_dbg_segments_tested;
static u16 g_bsp_dbg_segments_drawn;
static u16 g_bsp_dbg_visible_subsectors;
static u32 g_bsp_dbg_side_cache_subticks;
static u32 g_bsp_dbg_box_projection_subticks;
static u32 g_bsp_dbg_segment_raster_subticks;
static bool g_bsp_dbg_measure_side;
static bool g_bsp_dbg_measure_box;
static bool g_bsp_dbg_measure_segment;
#define BSP_DBG_INC(c) (g_bsp_dbg_##c)++
#define BSP_DBG_RESET() do { g_bsp_dbg_nodes_visited = 0; \
        g_bsp_dbg_boxes_rejected_cheap = 0; \
        g_bsp_dbg_boxes_projected = 0; \
        g_bsp_dbg_near_fallbacks = 0; \
        g_bsp_dbg_segments_tested = 0; \
        g_bsp_dbg_segments_drawn = 0; \
        g_bsp_dbg_visible_subsectors = 0; \
        g_bsp_dbg_side_cache_subticks = 0; \
        g_bsp_dbg_box_projection_subticks = 0; \
        g_bsp_dbg_segment_raster_subticks = 0; } while (0)
#elif CADENCE_STAGE_PROBE
// Release-speed unit counters: same increment sites as the DEBUG_PERF
// counters, but accumulated across the whole run (no per-frame reset) and
// with no subtick timing, so they don't perturb the measured cadence.
#define BSP_DBG_INC(c) bsp_cadence_inc_##c()
static inline void bsp_cadence_inc_nodes_visited(void) { g_cadence_nodes_visited++; }
static inline void bsp_cadence_inc_boxes_projected(void) { g_cadence_boxes_projected++; }
static inline void bsp_cadence_inc_segments_tested(void) { g_cadence_segs_tested++; }
static inline void bsp_cadence_inc_segments_drawn(void) { g_cadence_segs_drawn++; }
// Traversal attribution (see debug_checkpoint.h): the near-plane polygon branch
// pays up to 8 DIVS.W per box against the fast path's 2, so its share of
// box_calls is what explains project_box_range's cost.
static inline void bsp_cadence_inc_boxes_rejected_cheap(void) { g_cadence_box_cheap_reject++; }
static inline void bsp_cadence_inc_near_fallbacks(void) { g_cadence_box_near_path++; }
#define BSP_DBG_RESET() ((void)0)
#else
#define BSP_DBG_INC(c) ((void)0)
#define BSP_DBG_RESET() ((void)0)
#endif

#if DEBUG_PERF || BILLBOARD_VISIBLE_SUBSECTOR_CULL
static u8 g_visible_subsector_bits[(BSP_MAX_SUBSECTORS + 7) / 8];
#endif

static void render_node(u16 child);
static void bsp_visit_leaf(u16 subsector_id);

// The whole view is filled front-to-back: nothing farther can change the frame.
// Cheap enough (one word compare) to gate the box projection on.
static inline bool bsp_view_fully_closed(void) {
    return (bool)(g_solid_count >= BSP_SAMPLE_COLS);
}

// All E1M1 vertices, node deltas and collision-constrained camera coordinates
// fit signed words (the generated map spans only 4576x2816 units). Keeping this
// primitive word-typed makes every BSP dot/cross product one stock MULS.W.
static s32 native_muls_word(s16 left, s16 right) {
    s32 result = left;
    __asm__ volatile (
        "muls.w %1,%0"
        : "+d" (result)
        : "d" (right)
        : "cc");
    return result;
}

static s32 render_mul(s32 left, s32 right) {
    return native_muls_word((s16)left, (s16)right);
}

static u32 native_mulu_word(u16 left, u16 right) {
    u32 result = left;
    __asm__ volatile (
        "mulu.w %1,%0"
        : "+d" (result)
        : "d" (right)
        : "cc");
    return result;
}

// Exact 32x16 multiply assembled from the 68000's two 16x16 partial products.
// This replaces libgcc's bit-at-a-time __mulsi3 in UV interpolation while
// preserving the full signed 32-bit result used by the reference C renderer.
static s32 native_mul_long_unsigned(s32 left, u16 right) {
    const bool negative = (bool)(left < 0);
    const u32 magnitude = negative ? (u32)(-left) : (u32)left;
    const u32 low = native_mulu_word((u16)magnitude, right);
    const u32 high = native_mulu_word((u16)(magnitude >> 16), right) << 16;
    const u32 product = low + high;
    return negative ? -(s32)product : (s32)product;
}

// These reciprocal quotients are always non-negative and fit in a u16 on the
// valid BSP path. Using the 68000's native DIVU.W avoids the much slower
// compiler-emitted signed 32-bit division helper while preserving the exact
// integer result. Keep a general fallback for unusual map coordinates.
static u16 reciprocal_depth(s32 depth) {
    // Callers clip depth to >= BSP_NEAR (16) first, and the shared inverse
    // table is exact truncating BSP_INV_SCALE/(i+1) (see bsp_inv_depth_lut.h),
    // so table hits are byte-identical to the DIVU they replace. Walls farther
    // than 1024 units keep the native divide.
    if (depth <= 1024) {
        return g_bsp_inv_depth_lut[depth - 1];
    }
    return divu(BSP_INV_SCALE, (u16)depth);
}

static s32 reciprocal_span(s32 span) {
    const u32 numerator = (u32)FX_ONE << FX_SHIFT;

    if (span == 1) {
        return (s32)numerator;
    }
    return (s32)divu(numerator, (u16)span);
}

// Collision keeps the camera inside E1M1, whose generated bounds prove every
// BSP screen/clip/UV quotient fits a signed word. Avoid retaining an unreachable
// generic division branch in each inlined projection call.
static s32 perspective_divide(s32 numerator, s32 denominator) {
    return (s32)divs(numerator, (s16)denominator);
}

static void transform_vertex(u16 vertex_index, s32 *depth, s32 *lateral) {
    if (vertex_index < BSP_MAX_VERTICES &&
        g_vertex_generation[vertex_index] == g_cast_generation) {
        *depth = g_vertex_depth[vertex_index];
        *lateral = g_vertex_lateral[vertex_index];
        return;
    }

    const BspVertex *vertex = &bsp_vertices[vertex_index];
    const s32 relx = (s32)vertex->x - g_px;
    const s32 rely = (s32)vertex->y - g_py;
    const s32 transformed_depth =
        (render_mul(relx, g_fwx) + render_mul(rely, g_fwy)) >> FX_SHIFT;
    const s32 transformed_lateral =
        (render_mul(relx, g_rx) + render_mul(rely, g_ry)) >> FX_SHIFT;

    *depth = transformed_depth;
    *lateral = transformed_lateral;
    if (vertex_index < BSP_MAX_VERTICES) {
        g_vertex_depth[vertex_index] = (s16)transformed_depth;
        g_vertex_lateral[vertex_index] = (s16)transformed_lateral;
        g_vertex_generation[vertex_index] = g_cast_generation;
    }
}

static u16 find_next_open(u16 sample) {
    u16 root = sample;

    while (g_next_open[root] != root) {
        root = g_next_open[root];
    }
    while (sample != root) {
        const u16 next = g_next_open[sample];
        g_next_open[sample] = (u8)root;
        sample = next;
    }
    return root;
}

static void mark_sample_solid(u16 sample) {
    g_solid_words[sample >> 5] |= (u32)1u << (sample & 31);
    g_solid_count++;
    g_next_open[sample] = (u8)find_next_open((u16)(sample + 1));
}

static bool solid_sample_range_filled(u16 left_sample, u16 right_sample) {
    u16 word = (u16)(left_sample >> 5);
    const u16 last_word = (u16)(right_sample >> 5);

    while (word <= last_word) {
        const u16 word_left = (u16)(word << 5);
        const u16 word_right = (u16)(word_left + 31);
        const u16 range_left = (left_sample > word_left) ? left_sample : word_left;
        const u16 range_right = (right_sample < word_right) ? right_sample : word_right;
        const u16 start_bit = (u16)(range_left & 31);
        const u16 end_bit = (u16)(range_right & 31);
        u32 mask = 0xFFFFFFFFu << start_bit;

        if (end_bit < 31) {
            mask &= (u32)((1u << (end_bit + 1)) - 1u);
        }
        if ((g_solid_words[word] & mask) != mask) {
            return FALSE;
        }
        word++;
    }

    return TRUE;
}

// Draw one one-sided wall segment into the RayColumn buffer, respecting the
// solid-column occlusion buffer. Uniform full-height wall. Open doors are
// skipped (rendered as a passable gap).
static void draw_seg(u16 seg_index) {
    if (bsp_seg_is_open(seg_index)) {
        return;
    }

    BSP_DBG_INC(segments_tested);

    const BspSeg *seg = &bsp_segs[seg_index];
    const u16 door_lift = bsp_seg_door_lift(seg_index);
    const bool moving_door = (bool)(seg->type == BSP_SEG_DOOR && door_lift > 0);
    const BspVertex *a = &bsp_vertices[seg->v1];

    // Backface / one-sided cull: draw only when the camera is on the seg's
    // front side (the side its normal points toward).
    const s32 facing = render_mul(g_px - a->x, seg->nx) +
                       render_mul(g_py - a->y, seg->ny);
    if (facing <= 0) {
        return;
    }

    // Transform both endpoints into view space (depth = forward, lat = right).
    // Shared endpoints hit the generation cache after their first segment.
    s32 depthA;
    s32 latA;
    s32 depthB;
    s32 latB;
    transform_vertex(seg->v1, &depthA, &latA);
    transform_vertex(seg->v2, &depthB, &latB);

    // Texture coordinate along the wall (world units), repeating every 256 px
    // like the BSP projection. u goes 0 -> wall_length from v1 -> v2. The wall
    // length is precomputed in ROM (bsp_seg_wall_len) so we avoid two vertex
    // lookups and two abs calls per seg visit.
    const s32 wall_len = bsp_seg_wall_len[seg_index];
    s32 uA = seg->tex_u_offset;
    s32 uB = (s32)seg->tex_u_offset + wall_len;

    // Near-plane clipping.
    if (depthA < BSP_NEAR && depthB < BSP_NEAR) {
        return;
    }
    if (depthA < BSP_NEAR) {
        const s32 t = perspective_divide(((s32)BSP_NEAR - depthA) << FX_SHIFT,
                                         depthB - depthA);
        latA += render_mul(latB - latA, t) >> FX_SHIFT;
        uA += native_mul_long_unsigned(uB - uA, (u16)t) >> FX_SHIFT;
        depthA = BSP_NEAR;
    } else if (depthB < BSP_NEAR) {
        const s32 t = perspective_divide(((s32)BSP_NEAR - depthB) << FX_SHIFT,
                                         depthA - depthB);
        latB += render_mul(latA - latB, t) >> FX_SHIFT;
        uB += native_mul_long_unsigned(uA - uB, (u16)t) >> FX_SHIFT;
        depthB = BSP_NEAR;
    }

    // Project to screen x.
    s32 xa = RAY_VIEW_CENTER_X + perspective_divide(render_mul(latA, RAY_PROJ_X), depthA);
    s32 xb = RAY_VIEW_CENTER_X + perspective_divide(render_mul(latB, RAY_PROJ_X), depthB);
    if (xa == xb) {
        return;
    }

    // Order left -> right, carrying depth/u with each endpoint.
    s32 xL, xR, depthL, depthR, uL, uR;
    if (xa < xb) {
        xL = xa; xR = xb; depthL = depthA; depthR = depthB; uL = uA; uR = uB;
    } else {
        xL = xb; xR = xa; depthL = depthB; depthR = depthA; uL = uB; uR = uA;
    }

    const s32 span = xR - xL; // > 0 (xa != xb guarded above, ordered xL < xR)
    // Perspective-correct interpolation is linear in 1/depth and u/depth.
    const s32 invzL = reciprocal_depth(depthL);
    const s32 invzR = reciprocal_depth(depthR);
    const s32 uzL = native_mul_long_unsigned(uL, (u16)invzL);
    const s32 uzR = native_mul_long_unsigned(uR, (u16)invzR);
    // Reciprocal of the span, Q8, computed once per seg so the inner loop's
    // horizontal fraction is a multiply instead of a per-column divide. Matches
    // the old ((x-xL)<<8)/span to within the reciprocal's truncation.
    const s32 inv_span = reciprocal_span(span);

    const u8 tid = (seg->texture_id < FREEDOOM_WALL_TEXTURE_COUNT) ?
                       seg->texture_id : MEGALDOOM_TEX_FALLBACK;
    const u8 shade = (seg->ny != 0) ? 1 : 0; // N/S walls use the shaded copy
    // Doom textures may have arbitrary widths (including 24 pixels).  Scale
    // world-space U into the 32-column runtime copy while preserving the source
    // repeat period.  Q12 avoids a division in the sampled-column loop.
    const u16 u_scale_q12 = FREEDOOM_WALL_TEXTURE_USCALE_Q12[tid];

    s32 x0 = xL;
    s32 x1 = xR - 1;
    if (x0 < 0) x0 = 0;
    if (x1 > RAY_VIEW_COLS - 1) x1 = RAY_VIEW_COLS - 1;
    if (x0 > x1) {
        return;
    }

    const u16 first_sample = (u16)((x0 + RAY_COL_STRIDE - 1) / RAY_COL_STRIDE);
    const u16 last_sample = (u16)(x1 / RAY_COL_STRIDE);
    if (first_sample > last_sample) {
        return;
    }

    bool drew_any = FALSE;
#if CADENCE_STAGE_PROBE && CADENCE_DRAWSEG_SPLIT
    const u32 sample_loop_start = getSubTick();
#endif
    u16 sample = find_next_open(first_sample);
    while (sample <= last_sample) {
        const s32 x = (s32)sample * RAY_COL_STRIDE;
        // x - xL fits u16: x <= 159 and xL >= -(s16 quotient + margin), so the
        // difference is in [0, ~33008]. inv_span <= 65536/2 fits u16 (the
        // span == 1 case never reaches the multiply). One MULU.W therefore
        // produces the exact same product as the 32x16 helper it replaces.
        const s32 sfix = (span == 1) ? 0 :
            (s32)(native_mulu_word((u16)(x - xL), (u16)inv_span) >> FX_SHIFT);
        RayColumn *col = &g_columns[x];

        const s32 invz = invzL + (render_mul(invzR - invzL, sfix) >> FX_SHIFT);
        if (invz <= 0) {
            sample = find_next_open((u16)(sample + 1));
            continue;
        }

        // BSP_INV_SCALE/invz via a precomputed table: draw_seg's near-plane
        // clip guarantees 1 <= invz <= 1024 here (see bsp_inv_depth_lut.h's
        // range proof), so the table covers every value this call site can
        // ever produce. The divu fallback stays permanently for any future
        // change that violates that invariant; it is not expected to fire.
        s32 depth_col;
        const u16 invz_u16 = (u16)invz;
        if (invz_u16 <= 1024) {
            depth_col = g_bsp_inv_depth_lut[invz_u16 - 1];
#if DEBUG_PERF
            renderer_perf_record_cast_lut(invz_u16, FALSE);
#endif
        } else {
            depth_col = divu(BSP_INV_SCALE, invz_u16);
#if DEBUG_PERF
            renderer_perf_record_cast_lut(invz_u16, TRUE);
#endif
        }
        if (depth_col < 1) {
            depth_col = 1;
        }

        const s32 uz = uzL + (native_mul_long_unsigned(uzR - uzL, (u16)sfix) >> FX_SHIFT);
        const s32 u_col = perspective_divide(uz, invz);

        // height = RAY_PROJ_Y*RAY_WORLD_WALL_HEIGHT / depth_col, but depth_col =
        // INV_SCALE/invz, so fold to a multiply+shift and skip a divide.
        s32 height = render_mul(RAY_PROJ_Y * RAY_WORLD_WALL_HEIGHT, invz) >> 14;
        if (height < 1) {
            height = 1;
        } else if (height > RAY_VIEW_ROWS) {
            height = RAY_VIEW_ROWS;
        }

        // u_col is a DIVS.W quotient (fits s16 by the same map-bounds contract
        // perspective_divide relies on) and is non-negative: uz interpolates
        // between two non-negative endpoint products, so a single MULU.W is
        // exact. Max product 32767 * 65535 still fits u32/s32.
        const s32 scaled_u = (s32)(native_mulu_word((u16)u_col, u_scale_q12) >> 12);
        if (moving_door) {
            RayDoorOverlay *door = &col->door;
            if (door->height != 0 && depth_col >= door->depth) {
                sample = find_next_open((u16)(sample + 1));
                continue;
            }
            door->height = (u16)height;
            door->depth = (u16)depth_col;
            door->lift = door_lift;
            door->tex_x = (u8)(scaled_u & WALL_TEX_WIDTH_MASK);
            door->tex_y = seg->tex_v_offset;
            door->texture_id = tid;
            door->shade = shade;
        } else {
            col->height = (u16)height;
            col->depth = (u16)depth_col;
            col->tex_x = (u8)(scaled_u & WALL_TEX_WIDTH_MASK);
            col->tex_y = seg->tex_v_offset;
            col->texture_id = tid;
            col->shade = shade;
            col->flags = (seg->type == BSP_SEG_DOOR) ? RAY_COLUMN_FLAG_DOOR : 0;
            mark_sample_solid(sample);
        }
        drew_any = TRUE;
#if CADENCE_STAGE_PROBE
        g_cadence_samples++;
#endif
        sample = moving_door ? find_next_open((u16)(sample + 1)) :
                               find_next_open(sample);
    }
#if CADENCE_STAGE_PROBE && CADENCE_DRAWSEG_SPLIT
    g_cadence_sample_subticks += getSubTick() - sample_loop_start;
#endif
    if (drew_any) {
        BSP_DBG_INC(segments_drawn);
    }
}

// Project a child's axis-aligned world-space box to a conservative horizontal
// screen range. Any near-plane ambiguity expands to the whole view; only boxes
// proven completely behind the camera or outside the expanded viewport are cut.
static bool project_box_range(const BspBox *box, s16 *left, s16 *right) {
#if CADENCE_STAGE_PROBE
    g_cadence_box_calls++;
#endif
    if ((box->min_x > box->max_x) || (box->min_y > box->max_y)) {
#if CADENCE_STAGE_PROBE
        g_cadence_box_early_out++;
#endif
        *left = 0;
        *right = RAY_VIEW_COLS - 1;
        return TRUE;
    }

    if ((g_px >= box->min_x) && (g_px <= box->max_x) &&
        (g_py >= box->min_y) && (g_py <= box->max_y)) {
#if CADENCE_STAGE_PROBE
        g_cadence_box_early_out++;
#endif
        *left = 0;
        *right = RAY_VIEW_COLS - 1;
        return TRUE;
    }

    // Transform the min/min corner once, then add transformed X/Y extents.
    // Keeping the Q8 dot products unshifted until each corner is assembled is
    // algebraically identical to four independent transforms (including signed
    // truncation), while halving the number of multiplications from 16 to 8.
    const s32 relx = (s32)box->min_x - g_px;
    const s32 rely = (s32)box->min_y - g_py;
    const s32 span_x = (s32)box->max_x - box->min_x;
    const s32 span_y = (s32)box->max_y - box->min_y;
    const s32 depth_q8 = render_mul(relx, g_fwx) + render_mul(rely, g_fwy);
    const s32 lateral_q8 = render_mul(relx, g_rx) + render_mul(rely, g_ry);
    const s32 depth_dx_q8 = render_mul(span_x, g_fwx);
    const s32 lateral_dx_q8 = render_mul(span_x, g_rx);
    const s32 depth_dy_q8 = render_mul(span_y, g_fwy);
    const s32 lateral_dy_q8 = render_mul(span_y, g_ry);

    // Corner extrema without assembling the four corners: every corner is the
    // base Q8 dot product plus an independent subset of {dx, dy} extent terms,
    // and >> FX_SHIFT is monotonic, so min/max over the four corners equals the
    // min/max Q8 combination shifted once. These are EXACTLY the extrema the
    // old 4-corner loop computed (floor of the min is the min of the floors).
    const s32 ddx_neg = (depth_dx_q8 < 0) ? depth_dx_q8 : 0;
    const s32 ddx_pos = depth_dx_q8 - ddx_neg;
    const s32 ddy_neg = (depth_dy_q8 < 0) ? depth_dy_q8 : 0;
    const s32 ddy_pos = depth_dy_q8 - ddy_neg;
    const s32 max_depth = (depth_q8 + ddx_pos + ddy_pos) >> FX_SHIFT;

    if (max_depth < BSP_NEAR) {
#if CADENCE_STAGE_PROBE
        g_cadence_box_early_out++;
#endif
        return FALSE;
    }

    const s32 min_depth = (depth_q8 + ddx_neg + ddy_neg) >> FX_SHIFT;

    // Clip a near-plane-crossing box polygon instead of expanding it to the
    // whole view. The old fallback was safe but caused large adjacent BSP
    // subtrees to be visited when walking through doorways.
    if (min_depth < BSP_NEAR) {
        s32 depths[4];
        s32 laterals[4];
        depths[0] = depth_q8 >> FX_SHIFT;
        laterals[0] = lateral_q8 >> FX_SHIFT;
        depths[1] = (depth_q8 + depth_dx_q8) >> FX_SHIFT;
        laterals[1] = (lateral_q8 + lateral_dx_q8) >> FX_SHIFT;
        depths[2] = (depth_q8 + depth_dx_q8 + depth_dy_q8) >> FX_SHIFT;
        laterals[2] = (lateral_q8 + lateral_dx_q8 + lateral_dy_q8) >> FX_SHIFT;
        depths[3] = (depth_q8 + depth_dy_q8) >> FX_SHIFT;
        laterals[3] = (lateral_q8 + lateral_dy_q8) >> FX_SHIFT;
        BSP_DBG_INC(near_fallbacks);
        s32 min_screen = 0x7FFFFFFF;
        s32 max_screen = -0x7FFFFFFF;
        bool any = FALSE;
        BSP_DBG_INC(boxes_projected);
        for (u16 i = 0; i < 4; i++) {
            const u16 j = (u16)((i + 1) & 3);
            if (depths[i] >= BSP_NEAR) {
                const s32 screen = RAY_VIEW_CENTER_X + perspective_divide(
                    render_mul(laterals[i], RAY_PROJ_X), depths[i]);
                if (screen < min_screen) min_screen = screen;
                if (screen > max_screen) max_screen = screen;
                any = TRUE;
            }
            if ((depths[i] < BSP_NEAR) != (depths[j] < BSP_NEAR)) {
                const s32 denom = depths[j] - depths[i];
                const s32 t = perspective_divide(((s32)BSP_NEAR - depths[i]) << FX_SHIFT,
                                                 denom);
                const s32 lateral = laterals[i] +
                    (render_mul(laterals[j] - laterals[i], t) >> FX_SHIFT);
                const s32 screen = RAY_VIEW_CENTER_X +
                    perspective_divide(render_mul(lateral, RAY_PROJ_X), BSP_NEAR);
                if (screen < min_screen) min_screen = screen;
                if (screen > max_screen) max_screen = screen;
                any = TRUE;
            }
        }
        if (!any) return FALSE;
        min_screen -= RAY_COL_STRIDE;
        max_screen += RAY_COL_STRIDE;
        if ((max_screen < 0) || (min_screen >= RAY_VIEW_COLS)) return FALSE;
        if (min_screen < 0) min_screen = 0;
        if (max_screen >= RAY_VIEW_COLS) max_screen = RAY_VIEW_COLS - 1;
        *left = (s16)min_screen;
        *right = (s16)max_screen;
        return TRUE;
    }

    // All four corners are in front of the near plane (depth > 0), so the
    // half-plane signs are valid. Reject boxes proven completely outside the
    // expanded viewport without paying for the two perspective divisions below.
    //
    // The planes are evaluated axis-decomposed on the SHIFTED base/extent
    // values instead of per assembled corner. A corner's shifted value differs
    // from the decomposed floor sum by at most +2 (one +1 per addition folded
    // under the floor), so padding the decomposed extremum by
    // 2 * (RAY_PROJ_X + scale) bounds the old per-corner extremum from the
    // safe side: this test only ever rejects boxes the old exact test also
    // rejected (the hairline band falls through to the division path below,
    // which still culls or clips them exactly).
    const s32 d0 = depth_q8 >> FX_SHIFT;
    const s32 l0 = lateral_q8 >> FX_SHIFT;
    const s32 sdx = depth_dx_q8 >> FX_SHIFT;
    const s32 slx = lateral_dx_q8 >> FX_SHIFT;
    const s32 sdy = depth_dy_q8 >> FX_SHIFT;
    const s32 sly = lateral_dy_q8 >> FX_SHIFT;
    const s32 proj_lx = render_mul(RAY_PROJ_X, slx);
    const s32 proj_ly = render_mul(RAY_PROJ_X, sly);

    const s32 left_base = render_mul(RAY_PROJ_X, l0) + render_mul(LEFT_REJECT_SCALE, d0);
    const s32 left_dx = proj_lx + render_mul(LEFT_REJECT_SCALE, sdx);
    const s32 left_dy = proj_ly + render_mul(LEFT_REJECT_SCALE, sdy);
    s32 max_left_plane = left_base + (2 * (RAY_PROJ_X + LEFT_REJECT_SCALE));
    if (left_dx > 0) max_left_plane += left_dx;
    if (left_dy > 0) max_left_plane += left_dy;
    if (max_left_plane <= 0) {
        BSP_DBG_INC(boxes_rejected_cheap);
        return FALSE;
    }

    const s32 right_base = render_mul(RAY_PROJ_X, l0) - render_mul(RIGHT_REJECT_SCALE, d0);
    const s32 right_dx = proj_lx - render_mul(RIGHT_REJECT_SCALE, sdx);
    const s32 right_dy = proj_ly - render_mul(RIGHT_REJECT_SCALE, sdy);
    s32 min_right_plane = right_base - (2 * RIGHT_REJECT_SCALE);
    if (right_dx < 0) min_right_plane += right_dx;
    if (right_dy < 0) min_right_plane += right_dy;
    if (min_right_plane >= 0) {
        BSP_DBG_INC(boxes_rejected_cheap);
        return FALSE;
    }

    // Lateral extrema via the same exact monotonic-shift decomposition as the
    // depth extrema above.
    const s32 ldx_neg = (lateral_dx_q8 < 0) ? lateral_dx_q8 : 0;
    const s32 ldx_pos = lateral_dx_q8 - ldx_neg;
    const s32 ldy_neg = (lateral_dy_q8 < 0) ? lateral_dy_q8 : 0;
    const s32 ldy_pos = lateral_dy_q8 - ldy_neg;
    const s32 min_lateral = (lateral_q8 + ldx_neg + ldy_neg) >> FX_SHIFT;
    const s32 max_lateral = (lateral_q8 + ldx_pos + ldy_pos) >> FX_SHIFT;

    BSP_DBG_INC(boxes_projected);
    // Bound the four projected ratios by the enclosing lateral/depth rectangle.
    // For negative lateral the nearest depth is the most-negative projection;
    // for positive lateral it is the most-positive one. The opposite extrema
    // use max_depth. This is a conservative superset of the old four exact
    // corner projections and costs only two native DIVS.W.
    const s32 min_denominator = (min_lateral < 0) ? min_depth : max_depth;
    const s32 max_denominator = (max_lateral > 0) ? min_depth : max_depth;
    s32 min_screen = RAY_VIEW_CENTER_X + perspective_divide(
        render_mul(min_lateral, RAY_PROJ_X), min_denominator);
    s32 max_screen = RAY_VIEW_CENTER_X + perspective_divide(
        render_mul(max_lateral, RAY_PROJ_X), max_denominator);

    // Cover integer projection/truncation at box edges and the renderer's
    // horizontal sample stride before making an outside-FOV decision.
    min_screen -= RAY_COL_STRIDE;
    max_screen += RAY_COL_STRIDE;
    if ((max_screen < 0) || (min_screen >= RAY_VIEW_COLS)) {
        return FALSE;
    }

    if (min_screen < 0) min_screen = 0;
    if (max_screen >= RAY_VIEW_COLS) max_screen = RAY_VIEW_COLS - 1;
    *left = (s16)min_screen;
    *right = (s16)max_screen;
    return TRUE;
}

static void render_boxed_child(u16 child, const BspBox *box) {
    s16 left;
    s16 right;

    // Gate the expensive projection on full coverage. This test used to live at
    // the top of render_node, where it was dead: render_node is only reached
    // from here after solid_sample_range_filled() returned FALSE, which proves
    // some sample is still open, which proves g_solid_count < BSP_SAMPLE_COLS.
    // Here it does real work — once the view closes, every sibling still
    // pending on the recursion stack used to pay a full project_box_range plus
    // a range query before being rejected.
    if (bsp_view_fully_closed()) {
        return;
    }

#if DEBUG_PERF
    const u32 projection_start = g_bsp_dbg_measure_box ? getSubTick() : 0;
#endif

#if CADENCE_TRAVERSE_SPLIT
    const u32 box_start = getSubTick();
#endif
    const bool projected = project_box_range(box, &left, &right);
#if CADENCE_TRAVERSE_SPLIT
    g_cadence_box_subticks += getSubTick() - box_start;
#endif
#if DEBUG_PERF
    if (g_bsp_dbg_measure_box) {
        const u32 elapsed = getSubTick() - projection_start;
        g_bsp_dbg_box_projection_subticks += elapsed;
        renderer_perf_record_deep(RENDERER_PERF_DEEP_BSP_BOX, elapsed, 1);
    }
#endif
    if (!projected) {
        return;
    }

    const u16 left_sample = (u16)((left + RAY_COL_STRIDE - 1) / RAY_COL_STRIDE);
    const u16 right_sample = (u16)(right / RAY_COL_STRIDE);
    if (left_sample > right_sample) {
        return;
    }

    // Full-height solid walls make horizontal coverage sufficient: when every
    // column in the child's conservative range is already filled front-to-back,
    // no geometry in that child can change the frame. Test the range one word at
    // a time; rotation can make many child boxes cover most of the view.
#if CADENCE_STAGE_PROBE
    g_cadence_range_closed_calls++;
#endif
#if CADENCE_TRAVERSE_SPLIT
    const u32 range_start = getSubTick();
    const bool closed = solid_sample_range_filled(left_sample, right_sample);
    g_cadence_range_closed_subticks += getSubTick() - range_start;
    if (!closed) {
        render_node(child);
    }
#else
    if (!solid_sample_range_filled(left_sample, right_sample)) {
        render_node(child);
    }
#endif
}

static void render_node(u16 child) {
    // No full-coverage test here — see render_boxed_child, which owns it now.
    BSP_DBG_INC(nodes_visited);
    if (BSP_CHILD_IS_SUBSECTOR(child)) {
        bsp_visit_leaf(BSP_CHILD_INDEX(child));
        return;
    }

    const BspNode *n = &bsp_nodes[child];
    // Near/far order depends only on player position, not angle. The cache is
    // rebuilt only when position changes (see bsp_cast_frame), so pure rotation
    // reuses the cached bit instead of recomputing the partition cross product.
    const u8 side_bit = (u8)(1u << (child & 7));
    if (g_node_side_generation[child] != g_position_generation) {
#if DEBUG_PERF
        const u32 side_start = g_bsp_dbg_measure_side ? getSubTick() : 0;
#endif
        const s32 cross = render_mul(g_px - n->px, n->dy) -
                          render_mul(g_py - n->py, n->dx);
        if (cross >= 0) g_node_side_bits[child >> 3] |= side_bit;
        else g_node_side_bits[child >> 3] &= (u8)~side_bit;
        g_node_side_generation[child] = g_position_generation;
#if DEBUG_PERF
        if (g_bsp_dbg_measure_side) {
            const u32 elapsed = getSubTick() - side_start;
            g_bsp_dbg_side_cache_subticks += elapsed;
            renderer_perf_record_deep(RENDERER_PERF_DEEP_BSP_SIDE, elapsed, 1);
        }
#endif
    }
    if (g_node_side_bits[child >> 3] & side_bit) {
        render_boxed_child(n->front, &n->front_box); // near side first
        render_boxed_child(n->back, &n->back_box);
    } else {
        render_boxed_child(n->back, &n->back_box);
        render_boxed_child(n->front, &n->front_box);
    }
}

// Conservative front-to-back BSP walk. Children are visited near side first,
// boxes outside the FOV are rejected, and the solid-sample occlusion buffer
// prunes ranges. This used to take leaf/range-closed/all-closed callbacks
// through a bsp_traverse.h vtable, but it only ever had one caller passing one
// fixed set, and GCC would not devirtualize through the recursion: every node,
// box and leaf paid a memory-indirect jsr plus argument pushes. The callbacks
// are now called directly.
static void bsp_traverse_front_to_back(const PlayerState *player) {
    BSP_DBG_RESET();
    g_fwx = fx_cos(player->angle);
    g_fwy = fx_sin(player->angle);
    g_rx = (s16)-g_fwy;
    g_ry = g_fwx;
    g_px = player->x;
    g_py = player->y;

#if DEBUG_PERF
    {
        const RendererPerfDeepPhase phase = renderer_perf_get_deep_phase();
        g_bsp_dbg_measure_side = (bool)(phase == RENDERER_PERF_DEEP_BSP_SIDE);
        g_bsp_dbg_measure_box = (bool)(phase == RENDERER_PERF_DEEP_BSP_BOX);
        g_bsp_dbg_measure_segment = (bool)(phase == RENDERER_PERF_DEEP_BSP_SEGMENT);
    }
#endif

    g_cast_generation++;
    if (g_cast_generation == 0) {
        for (u16 i = 0; i < BSP_MAX_VERTICES; i++) g_vertex_generation[i] = 0;
        g_cast_generation = 1;
    }

    if (!g_node_cache_valid || g_px != g_node_cache_px || g_py != g_node_cache_py) {
        g_position_generation++;
        if (g_position_generation == 0) {
            for (u16 i = 0; i < BSP_MAX_NODES; i++) g_node_side_generation[i] = 0;
            g_position_generation = 1;
        }
        g_node_cache_px = g_px;
        g_node_cache_py = g_py;
        g_node_cache_valid = TRUE;
    }

    render_node(bsp_root_node);
}

static void bsp_visit_leaf(u16 subsector_id) {
    const BspSubsector *ss = &bsp_subsectors[subsector_id];
#if DEBUG_PERF || BILLBOARD_VISIBLE_SUBSECTOR_CULL
    // The default build keeps this as an oracle. The optional cull consumes it
    // only after proving the billboard's whole horizontal footprint remains in
    // this leaf; boundary sprites retain the old full-list behavior.
    if (subsector_id < BSP_MAX_SUBSECTORS) {
        const u16 byte = subsector_id >> 3;
        const u8 bit = (u8)(1u << (subsector_id & 7));
        if ((g_visible_subsector_bits[byte] & bit) == 0) {
            g_visible_subsector_bits[byte] |= bit;
#if DEBUG_PERF
            g_bsp_dbg_visible_subsectors++;
#endif
        }
    }
#endif
    for (u16 i = 0; i < ss->seg_count; i++) {
#if DEBUG_PERF
        const u32 raster_start = g_bsp_dbg_measure_segment ? getSubTick() : 0;
#elif CADENCE_STAGE_PROBE && CADENCE_DRAWSEG_SPLIT
        const u32 raster_start = getSubTick();
#endif
        draw_seg((u16)(ss->first_seg + i));
#if DEBUG_PERF
        if (g_bsp_dbg_measure_segment) {
            const u32 elapsed = getSubTick() - raster_start;
            g_bsp_dbg_segment_raster_subticks += elapsed;
            renderer_perf_record_deep(RENDERER_PERF_DEEP_BSP_SEGMENT, elapsed, 1);
        }
#elif CADENCE_STAGE_PROBE && CADENCE_DRAWSEG_SPLIT
        g_cadence_drawseg_subticks += getSubTick() - raster_start;
#endif
    }
}

void bsp_invalidate_node_cache(void) {
    g_node_cache_valid = FALSE;
}

void bsp_init(void) {
    // The near/far order cache is rebuilt on the first bsp_cast_frame (position
    // will differ from the bss-zeroed sentinel). Collision/LOS still computes
    // seg AABBs inline from the vertex table (no per-seg RAM on the 64KB MD).
    g_node_cache_valid = FALSE;
    g_position_generation = 1;
    for (u16 i = 0; i < BSP_MAX_NODES; i++) g_node_side_generation[i] = 0;
    g_cast_generation = 0;
    for (u16 i = 0; i < BSP_MAX_VERTICES; i++) g_vertex_generation[i] = 0;
}

// Apply the far/empty default to exactly the samples no wall claimed.
//
// draw_seg's non-door branch writes all seven wall fields and calls
// mark_sample_solid together, and nothing else writes them, so "sample is
// solid" is equivalent to "this frame wrote its wall fields". Seeding all 80
// samples up front therefore stored 14 fields per sample -- ~264 cycles each,
// ~21k cycles per rebuild -- to have almost every one of them overwritten
// moments later. A temporary probe counted the unclaimed columns across all
// four routes (checkpoints, slow-turn, stationary-combat and the 197-rebuild
// tour-east-combat): zero, every frame. E1M1 is fully enclosed, so walls always
// claim all 80 samples and this loop's body never runs; it stays as the correct
// fallback for a view that does not close (open sky, a map with a void), where
// the alternative is a column showing last frame's wall. The word-at-a-time
// scan costs three compares when the view is fully covered.
static void seed_unclaimed_columns(RayColumn *columns) {
    for (u16 word = 0; word < BSP_SOLID_WORD_COUNT; word++) {
        const u16 base = (u16)(word << 5);
        const u16 count = ((u16)(BSP_SAMPLE_COLS - base) < 32u) ?
                              (u16)(BSP_SAMPLE_COLS - base) : 32u;
        u32 open = ~g_solid_words[word];

        // The final word runs past BSP_SAMPLE_COLS; those bits are never set,
        // so mask them off rather than treating them as unclaimed samples.
        if (count < 32) {
            open &= ((u32)1u << count) - 1u;
        }
        if (open == 0) {
            continue;
        }
        for (u16 bit = 0; bit < count; bit++) {
            if (open & ((u32)1u << bit)) {
                seed_column_default(&columns[(u16)((base + bit) * RAY_COL_STRIDE)]);
            }
        }
    }
}

void bsp_cast_frame(const PlayerState *player, RayColumn *columns, RaySceneColors *scene_colors) {
    g_columns = columns;

#if DEBUG_PERF || BILLBOARD_VISIBLE_SUBSECTOR_CULL
    for (u16 i = 0; i < (BSP_MAX_SUBSECTORS + 7) / 8; i++) {
        g_visible_subsector_bits[i] = 0;
    }
#endif

    const u16 subsector = bsp_find_subsector(player->x, player->y);
    u16 sector = (subsector < bsp_subsector_count) ? bsp_subsector_sector[subsector] : 0;
    if (sector >= FREEDOOM_SECTOR_VISUAL_COUNT) sector = 0;
    const u8 *visual = FREEDOOM_SECTOR_VISUALS[sector];
    scene_colors->ceiling = (RayFlatColor){visual[0], visual[1], visual[2]};
    scene_colors->floor = (RayFlatColor){visual[3], visual[4], visual[5]};
    scene_colors->sector = sector;

    // Clear occlusion and seed every column with a far/empty default so columns
    // no wall covers still render (as distant, mostly sky/floor).
    g_solid_count = 0;
    for (u16 i = 0; i < BSP_SOLID_WORD_COUNT; i++) {
        g_solid_words[i] = 0;
    }
    // Only door.height needs clearing up front: it is the sentinel every door
    // consumer short-circuits on (draw_seg here, draw_door_overlays and
    // column_door_active in the pack stage), and draw_seg writes the whole
    // RayDoorOverlay whenever it writes height, so the other six door fields are
    // never read while height is 0. The wall fields are deferred to
    // seed_unclaimed_columns below.
    for (u16 sample = 0; sample < BSP_SAMPLE_COLS; sample++) {
        g_next_open[sample] = (u8)sample;
        columns[(u16)(sample * RAY_COL_STRIDE)].door.height = 0;
    }
    g_next_open[BSP_SAMPLE_COLS] = BSP_SAMPLE_COLS;

    bsp_traverse_front_to_back(player);
    seed_unclaimed_columns(columns);
}

#if DEBUG_PERF || BILLBOARD_VISIBLE_SUBSECTOR_CULL
bool bsp_subsector_was_visited(u16 subsector_id) {
    if (subsector_id >= BSP_MAX_SUBSECTORS) return FALSE;
    return (bool)(g_visible_subsector_bits[subsector_id >> 3] &
                  (u8)(1u << (subsector_id & 7)));
}
#endif

#if DEBUG_PERF
// These accessors are consumed only by the optional perf overlay in main.c.
// Keep them visible across SGDK's LTO link even though this translation unit
// itself has no callers for them.
#define BSP_DEBUG_EXPORT __attribute__((used, externally_visible))
BSP_DEBUG_EXPORT u16 bsp_get_debug_nodes_visited(void) { return g_bsp_dbg_nodes_visited; }
BSP_DEBUG_EXPORT u16 bsp_get_debug_boxes_rejected_cheap(void) { return g_bsp_dbg_boxes_rejected_cheap; }
BSP_DEBUG_EXPORT u16 bsp_get_debug_boxes_projected(void) { return g_bsp_dbg_boxes_projected; }
BSP_DEBUG_EXPORT u16 bsp_get_debug_near_fallbacks(void) { return g_bsp_dbg_near_fallbacks; }
BSP_DEBUG_EXPORT u16 bsp_get_debug_segments_tested(void) { return g_bsp_dbg_segments_tested; }
BSP_DEBUG_EXPORT u16 bsp_get_debug_segments_drawn(void) { return g_bsp_dbg_segments_drawn; }
BSP_DEBUG_EXPORT u16 bsp_get_debug_visible_subsector_count(void) {
    return g_bsp_dbg_visible_subsectors;
}
BSP_DEBUG_EXPORT u32 bsp_get_debug_side_cache_subticks(void) { return g_bsp_dbg_side_cache_subticks; }
BSP_DEBUG_EXPORT u32 bsp_get_debug_box_projection_subticks(void) { return g_bsp_dbg_box_projection_subticks; }
BSP_DEBUG_EXPORT u32 bsp_get_debug_segment_raster_subticks(void) { return g_bsp_dbg_segment_raster_subticks; }
#undef BSP_DEBUG_EXPORT
#endif
