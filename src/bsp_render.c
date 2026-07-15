#include "bsp_render.h"
#include "bsp_map.h"
#include "bsp_traverse.h"
#include "fixed_math.h"
#include "generated_assets.h"

// draw_seg only computes the height/texture fields at columns on a RAY_COL_STRIDE
// boundary, matching the columns build_bsp_tilemap actually samples. That
// relies on the stride being a power of two (the (x & (STRIDE-1)) test).
#if (RAY_COL_STRIDE & (RAY_COL_STRIDE - 1)) != 0
#error "draw_seg's strided-column optimization requires RAY_COL_STRIDE to be a power of two"
#endif

// --- Projection constants ---------------------------------------------------
// View is RAY_VIEW_COLS (160) px wide; center column is 80. PROJ is tuned so the
// horizontal field of view is 48 degrees:
//   at the screen edge, |lateral/depth| = tan(24deg) ~= 0.445 maps to +/-80,
//   so PROJ = 80 / 0.445 ~= 180.
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
#define BSP_CEILING_COLOR 4  // dark gray
#define BSP_FLOOR_COLOR 11   // standard gray
#define BSP_SAMPLE_COLS (RAY_VIEW_COLS / RAY_COL_STRIDE)
#define BSP_SOLID_WORD_COUNT ((BSP_SAMPLE_COLS + 31) / 32)

// Per-frame view basis + camera, set at the top of bsp_cast_frame().
static s16 g_fwx, g_fwy; // forward axis (cos, sin), Q8
static s16 g_rx, g_ry;   // right axis, Q8
static s32 g_px, g_py;   // camera position, world units
static bool g_native_word_math;

// Successor set for open samples. Once a solid wall claims a sample, its entry
// points at the next still-open sample, so farther overlapping segments skip
// closed runs instead of walking them column by column. The final entry is a
// sentinel one past the view.
static u8 g_next_open[BSP_SAMPLE_COLS + 1];
static u32 g_solid_words[BSP_SOLID_WORD_COUNT];
static u16 g_solid_count;
static RayColumn *g_columns;
static BspTraverseLeafFn g_visit_leaf;
static BspTraverseRangeClosedFn g_range_closed;
static BspTraverseAllClosedFn g_all_closed;
static void *g_traverse_context;

// --- Near/far order cache (Patch 3.2) ---------------------------------------
// The front/back traversal order at each BSP node depends only on the player's
// position relative to the partition line — not on the camera angle. During
// pure rotation the position is unchanged, so this cache lets render_node skip
// the per-node cross product (two s32 multiplies + subtract) entirely. One bit
// per node: 1 = front-first (cross >= 0), 0 = back-first. Rebuilt only when
// player.x or player.y changes; invalidated explicitly on teleport/reset.
static u8 g_node_side_bits[(BSP_MAX_NODES + 7) / 8];
static u16 g_node_side_generation[BSP_MAX_NODES];
static u16 g_position_generation;
static s32 g_node_cache_px;
static s32 g_node_cache_py;
static bool g_node_cache_valid;

#if DEBUG_PERF
// Temporary BSP instrumentation: counts how the new division-free frustum
// precheck interacts with traversal. Nodes visited, boxes rejected cheaply
// (by the half-plane test, avoiding 4 divisions), boxes that survived to the
// 4-division projection path, and near-plane fallbacks (expanded to whole
// view). Reset each frame in bsp_cast_frame; read by the DEBUG_PERF overlay.
static u16 g_bsp_dbg_nodes_visited;
static u16 g_bsp_dbg_boxes_rejected_cheap;
static u16 g_bsp_dbg_boxes_projected;
static u16 g_bsp_dbg_near_fallbacks;
static u16 g_bsp_dbg_segments_tested;
static u16 g_bsp_dbg_segments_drawn;
static u32 g_bsp_dbg_side_cache_subticks;
static u32 g_bsp_dbg_box_projection_subticks;
static u32 g_bsp_dbg_segment_raster_subticks;
#define BSP_DBG_INC(c) (g_bsp_dbg_##c)++
#define BSP_DBG_RESET() do { g_bsp_dbg_nodes_visited = 0; \
        g_bsp_dbg_boxes_rejected_cheap = 0; \
        g_bsp_dbg_boxes_projected = 0; \
        g_bsp_dbg_near_fallbacks = 0; \
        g_bsp_dbg_segments_tested = 0; \
        g_bsp_dbg_segments_drawn = 0; \
        g_bsp_dbg_side_cache_subticks = 0; \
        g_bsp_dbg_box_projection_subticks = 0; \
        g_bsp_dbg_segment_raster_subticks = 0; } while (0)
#else
#define BSP_DBG_INC(c) ((void)0)
#define BSP_DBG_RESET() ((void)0)
#endif

static void render_node(u16 child);

// A signed-word multiply maps directly to the stock 68000 MULS.W instruction.
// The generated map bounds let bsp_traverse_front_to_back prove once per frame
// that all camera-relative geometry fits s16. Unusual cameras retain the exact
// compiler-provided 32-bit multiply path.
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
    if (g_native_word_math) {
        return native_muls_word((s16)left, (s16)right);
    }
    return left * right;
}

static bool camera_supports_native_word_math(s32 px, s32 py) {
    return (bool)(
        px >= (s32)bsp_map_max_x - 32767 &&
        px <= (s32)bsp_map_min_x + 32768 &&
        py >= (s32)bsp_map_max_y - 32767 &&
        py <= (s32)bsp_map_min_y + 32768);
}

// These reciprocal quotients are always non-negative and fit in a u16 on the
// valid BSP path. Using the 68000's native DIVU.W avoids the much slower
// compiler-emitted signed 32-bit division helper while preserving the exact
// integer result. Keep a general fallback for unusual map coordinates.
static u16 reciprocal_depth(s32 depth) {
    if ((depth > 0) && (depth <= 0xFFFF)) {
        return divu(BSP_INV_SCALE, (u16)depth);
    }

    return (u16)(BSP_INV_SCALE / depth);
}

static s32 reciprocal_span(s32 span) {
    const u32 numerator = (u32)FX_ONE << FX_SHIFT;

    if (span == 1) {
        return (s32)numerator;
    }
    if ((span > 1) && (span <= 0xFFFF)) {
        return (s32)divu(numerator, (u16)span);
    }

    return (s32)(numerator / (u32)span);
}

// DIVS.W produces a 16-bit quotient. Screen projection and texture coordinates
// normally stay in that range; use it when they do and retain the old 32-bit
// expression for an exact overflow-safe fallback.
static s32 perspective_divide(s32 numerator, s32 denominator) {
    if ((denominator > 0) && (denominator <= 0x7FFF)) {
        const s32 min_quotient_numerator = -((s32)denominator << 15);
        const s32 max_quotient_numerator = ((s32)denominator << 15) - denominator;

        if ((numerator >= min_quotient_numerator) &&
            (numerator <= max_quotient_numerator)) {
            return (s32)divs(numerator, (s16)denominator);
        }
    }

    return numerator / denominator;
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
    const BspVertex *b = &bsp_vertices[seg->v2];

    // Backface / one-sided cull: draw only when the camera is on the seg's
    // front side (the side its normal points toward).
    const s32 facing = render_mul(g_px - a->x, seg->nx) +
                       render_mul(g_py - a->y, seg->ny);
    if (facing <= 0) {
        return;
    }

    // Transform both endpoints into view space (depth = forward, lat = right).
    s32 relx = (s32)a->x - g_px;
    s32 rely = (s32)a->y - g_py;
    s32 depthA = (render_mul(relx, g_fwx) + render_mul(rely, g_fwy)) >> FX_SHIFT;
    s32 latA = (render_mul(relx, g_rx) + render_mul(rely, g_ry)) >> FX_SHIFT;

    relx = (s32)b->x - g_px;
    rely = (s32)b->y - g_py;
    s32 depthB = (render_mul(relx, g_fwx) + render_mul(rely, g_fwy)) >> FX_SHIFT;
    s32 latB = (render_mul(relx, g_rx) + render_mul(rely, g_ry)) >> FX_SHIFT;

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
        uA += ((uB - uA) * t) >> FX_SHIFT;
        depthA = BSP_NEAR;
    } else if (depthB < BSP_NEAR) {
        const s32 t = perspective_divide(((s32)BSP_NEAR - depthB) << FX_SHIFT,
                                         depthA - depthB);
        latB += render_mul(latA - latB, t) >> FX_SHIFT;
        uB += ((uA - uB) * t) >> FX_SHIFT;
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
    const s32 uzL = uL * invzL;
    const s32 uzR = uR * invzR;
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
    u16 sample = find_next_open(first_sample);
    while (sample <= last_sample) {
        const s32 x = (s32)sample * RAY_COL_STRIDE;
        const s32 sfix = (((x - xL) * inv_span) >> FX_SHIFT); // 0..256 across span
        RayColumn *col = &g_columns[x];

        const s32 invz = invzL + (render_mul(invzR - invzL, sfix) >> FX_SHIFT);
        if (invz <= 0) {
            sample = find_next_open((u16)(sample + 1));
            continue;
        }

        s32 depth_col = divu(BSP_INV_SCALE, (u16)invz);
        if (depth_col < 1) {
            depth_col = 1;
        }

        const s32 uz = uzL + (((uzR - uzL) * sfix) >> FX_SHIFT);
        const s32 u_col = perspective_divide(uz, invz);

        // height = RAY_PROJ_Y*RAY_WORLD_WALL_HEIGHT / depth_col, but depth_col =
        // INV_SCALE/invz, so fold to a multiply+shift and skip a divide.
        s32 height = render_mul(RAY_PROJ_Y * RAY_WORLD_WALL_HEIGHT, invz) >> 14;
        if (height < 1) {
            height = 1;
        } else if (height > RAY_VIEW_ROWS) {
            height = RAY_VIEW_ROWS;
        }

        const s32 scaled_u = (u_col * (s32)u_scale_q12) >> 12;
        if (moving_door) {
            RayDoorOverlay *door = &col->door;
            if (door->height != 0 && depth_col >= door->depth) {
                sample = find_next_open((u16)(sample + 1));
                continue;
            }
            door->height = (u16)height;
            door->depth = (u16)depth_col;
            door->lift = door_lift;
            door->tex_x = (u8)(scaled_u & WALL_TEX_DIM_MASK);
            door->tex_y = seg->tex_v_offset;
            door->texture_id = tid;
            door->shade = shade;
        } else {
            col->height = (u16)height;
            col->depth = (u16)depth_col;
            col->tex_x = (u8)(scaled_u & WALL_TEX_DIM_MASK);
            col->tex_y = seg->tex_v_offset;
            col->texture_id = tid;
            col->shade = shade;
            col->flags = (seg->type == BSP_SEG_DOOR) ? RAY_COLUMN_FLAG_DOOR : 0;
            mark_sample_solid(sample);
        }
        drew_any = TRUE;
        sample = moving_door ? find_next_open((u16)(sample + 1)) :
                               find_next_open(sample);
    }
    if (drew_any) {
        BSP_DBG_INC(segments_drawn);
    }
}

// Project a child's axis-aligned world-space box to a conservative horizontal
// screen range. Any near-plane ambiguity expands to the whole view; only boxes
// proven completely behind the camera or outside the expanded viewport are cut.
static bool project_box_range(const BspBox *box, s16 *left, s16 *right) {
    if ((box->min_x > box->max_x) || (box->min_y > box->max_y)) {
        *left = 0;
        *right = RAY_VIEW_COLS - 1;
        return TRUE;
    }

    if ((g_px >= box->min_x) && (g_px <= box->max_x) &&
        (g_py >= box->min_y) && (g_py <= box->max_y)) {
        *left = 0;
        *right = RAY_VIEW_COLS - 1;
        return TRUE;
    }

    s32 depths[4];
    s32 laterals[4];
    s32 min_depth = 0x7FFFFFFF;
    s32 max_depth = -0x7FFFFFFF;
    // Track the division-free half-plane extrema in the same pass that computes
    // depth/lateral, so surviving boxes reuse these arrays for the projections
    // below without a second transform pass.
    s32 max_left_plane = -0x7FFFFFFF;
    s32 min_right_plane = 0x7FFFFFFF;

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

    depths[0] = depth_q8 >> FX_SHIFT;
    laterals[0] = lateral_q8 >> FX_SHIFT;
    depths[1] = (depth_q8 + depth_dx_q8) >> FX_SHIFT;
    laterals[1] = (lateral_q8 + lateral_dx_q8) >> FX_SHIFT;
    depths[2] = (depth_q8 + depth_dx_q8 + depth_dy_q8) >> FX_SHIFT;
    laterals[2] = (lateral_q8 + lateral_dx_q8 + lateral_dy_q8) >> FX_SHIFT;
    depths[3] = (depth_q8 + depth_dy_q8) >> FX_SHIFT;
    laterals[3] = (lateral_q8 + lateral_dy_q8) >> FX_SHIFT;

    for (u16 i = 0; i < 4; i++) {
        const s32 depth = depths[i];
        const s32 lateral = laterals[i];
        if (depth < min_depth) min_depth = depth;
        if (depth > max_depth) max_depth = depth;

        const s32 projected_lateral = render_mul(RAY_PROJ_X, lateral);
        const s32 left_plane = projected_lateral + render_mul(LEFT_REJECT_SCALE, depth);
        const s32 right_plane = projected_lateral - render_mul(RIGHT_REJECT_SCALE, depth);
        if (left_plane > max_left_plane) max_left_plane = left_plane;
        if (right_plane < min_right_plane) min_right_plane = right_plane;
    }

    if (max_depth < BSP_NEAR) {
        return FALSE;
    }

    // Clip a near-plane-crossing box polygon instead of expanding it to the
    // whole view. The old fallback was safe but caused large adjacent BSP
    // subtrees to be visited when walking through doorways.
    if (min_depth < BSP_NEAR) {
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
    // expanded viewport without paying for the four perspective divisions below.
    if (max_left_plane <= 0) {
        BSP_DBG_INC(boxes_rejected_cheap);
        return FALSE;
    }
    if (min_right_plane >= 0) {
        BSP_DBG_INC(boxes_rejected_cheap);
        return FALSE;
    }

    s32 min_screen = 0x7FFFFFFF;
    s32 max_screen = -0x7FFFFFFF;
    BSP_DBG_INC(boxes_projected);
    // Four native DIVS.W projections are cheaper on a 68000 than the old two
    // divisions plus twelve generic 32-bit cross-products used to select ratio
    // extrema. Taking min/max of the exact projected values preserves the same
    // conservative range.
    for (u16 i = 0; i < 4; i++) {
        const s32 screen = RAY_VIEW_CENTER_X + perspective_divide(
            render_mul(laterals[i], RAY_PROJ_X), depths[i]);
        if (screen < min_screen) min_screen = screen;
        if (screen > max_screen) max_screen = screen;
    }

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
#if DEBUG_PERF
    const u32 projection_start = getSubTick();
#endif

    const bool projected = project_box_range(box, &left, &right);
#if DEBUG_PERF
    g_bsp_dbg_box_projection_subticks += getSubTick() - projection_start;
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
    if (!g_range_closed(left_sample, right_sample, g_traverse_context)) {
        render_node(child);
    }
}

static void render_node(u16 child) {
    if (g_all_closed(g_traverse_context)) {
        return; // whole view already filled front-to-back
    }

    BSP_DBG_INC(nodes_visited);
    if (BSP_CHILD_IS_SUBSECTOR(child)) {
        g_visit_leaf(BSP_CHILD_INDEX(child), g_traverse_context);
        return;
    }

    const BspNode *n = &bsp_nodes[child];
    // Near/far order depends only on player position, not angle. The cache is
    // rebuilt only when position changes (see bsp_cast_frame), so pure rotation
    // reuses the cached bit instead of recomputing the partition cross product.
    const u8 side_bit = (u8)(1u << (child & 7));
    if (g_node_side_generation[child] != g_position_generation) {
#if DEBUG_PERF
        const u32 side_start = getSubTick();
#endif
        const s32 cross = render_mul(g_px - n->px, n->dy) -
                          render_mul(g_py - n->py, n->dx);
        if (cross >= 0) g_node_side_bits[child >> 3] |= side_bit;
        else g_node_side_bits[child >> 3] &= (u8)~side_bit;
        g_node_side_generation[child] = g_position_generation;
#if DEBUG_PERF
        g_bsp_dbg_side_cache_subticks += getSubTick() - side_start;
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

void bsp_traverse_front_to_back(const PlayerState *player,
                                BspTraverseLeafFn visit_leaf,
                                BspTraverseRangeClosedFn range_closed,
                                BspTraverseAllClosedFn all_closed,
                                void *context) {
    BSP_DBG_RESET();
    g_fwx = fx_cos(player->angle);
    g_fwy = fx_sin(player->angle);
    g_rx = (s16)-g_fwy;
    g_ry = g_fwx;
    g_px = player->x;
    g_py = player->y;
    g_native_word_math = camera_supports_native_word_math(g_px, g_py);
    g_visit_leaf = visit_leaf;
    g_range_closed = range_closed;
    g_all_closed = all_closed;
    g_traverse_context = context;

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

static void bsp_visit_leaf(u16 subsector_id, void *context) {
    const BspSubsector *ss = &bsp_subsectors[subsector_id];
    (void)context;
    for (u16 i = 0; i < ss->seg_count; i++) {
#if DEBUG_PERF
        const u32 raster_start = getSubTick();
#endif
        draw_seg((u16)(ss->first_seg + i));
#if DEBUG_PERF
        g_bsp_dbg_segment_raster_subticks += getSubTick() - raster_start;
#endif
    }
}

static bool bsp_range_closed(u16 left_sample, u16 right_sample, void *context) {
    (void)context;
    return solid_sample_range_filled(left_sample, right_sample);
}

static bool bsp_all_closed(void *context) {
    (void)context;
    return g_solid_count >= BSP_SAMPLE_COLS;
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
}

void bsp_cast_frame(const PlayerState *player, RayColumn *columns, RaySceneColors *scene_colors) {
    g_columns = columns;

    scene_colors->ceiling_color = BSP_CEILING_COLOR;
    scene_colors->floor_color = BSP_FLOOR_COLOR;

    // Clear occlusion and seed every column with a far/empty default so columns
    // no wall covers still render (as distant, mostly sky/floor).
    g_solid_count = 0;
    for (u16 i = 0; i < BSP_SOLID_WORD_COUNT; i++) {
        g_solid_words[i] = 0;
    }
    for (u16 sample = 0; sample < BSP_SAMPLE_COLS; sample++) {
        const u16 c = (u16)(sample * RAY_COL_STRIDE);
        g_next_open[sample] = (u8)sample;
        columns[c].height = 1;
        columns[c].depth = 0x7FFF;
        columns[c].tex_x = 0;
        columns[c].tex_y = 0;
        columns[c].texture_id = MEGALDOOM_TEX_FALLBACK;
        columns[c].shade = 0;
        columns[c].flags = 0;
        columns[c].door.height = 0;
        columns[c].door.depth = 0x7FFF;
        columns[c].door.lift = 0;
        columns[c].door.tex_x = 0;
        columns[c].door.tex_y = 0;
        columns[c].door.texture_id = MEGALDOOM_TEX_FALLBACK;
        columns[c].door.shade = 0;
    }
    g_next_open[BSP_SAMPLE_COLS] = BSP_SAMPLE_COLS;

    bsp_traverse_front_to_back(player, bsp_visit_leaf, bsp_range_closed,
                               bsp_all_closed, NULL);
}

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
BSP_DEBUG_EXPORT u32 bsp_get_debug_side_cache_subticks(void) { return g_bsp_dbg_side_cache_subticks; }
BSP_DEBUG_EXPORT u32 bsp_get_debug_box_projection_subticks(void) { return g_bsp_dbg_box_projection_subticks; }
BSP_DEBUG_EXPORT u32 bsp_get_debug_segment_raster_subticks(void) { return g_bsp_dbg_segment_raster_subticks; }
#undef BSP_DEBUG_EXPORT
#endif
