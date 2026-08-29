/* BSP front-to-back traversal and projected child bounds. */
#include "bsp_render_internal.h"

bool bsp_project_box_range(const BspBox *box, s16 *left, s16 *right) {
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
    const s32 depth_q8 = bsp_render_mul(relx, g_fwx) + bsp_render_mul(rely, g_fwy);
    const s32 lateral_q8 = bsp_render_mul(relx, g_rx) + bsp_render_mul(rely, g_ry);
    const s32 depth_dx_q8 = bsp_render_mul(span_x, g_fwx);
    const s32 lateral_dx_q8 = bsp_render_mul(span_x, g_rx);
    const s32 depth_dy_q8 = bsp_render_mul(span_y, g_fwy);
    const s32 lateral_dy_q8 = bsp_render_mul(span_y, g_ry);

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
                const s32 screen = RAY_VIEW_CENTER_X + bsp_perspective_divide(
                    bsp_render_mul(laterals[i], RAY_PROJ_X), depths[i]);
                if (screen < min_screen) min_screen = screen;
                if (screen > max_screen) max_screen = screen;
                any = TRUE;
            }
            if ((depths[i] < BSP_NEAR) != (depths[j] < BSP_NEAR)) {
                const s32 denom = depths[j] - depths[i];
                const s32 t = bsp_perspective_divide(((s32)BSP_NEAR - depths[i]) << FX_SHIFT,
                                                 denom);
                const s32 lateral = laterals[i] +
                    (bsp_render_mul(laterals[j] - laterals[i], t) >> FX_SHIFT);
                const s32 screen = RAY_VIEW_CENTER_X +
                    bsp_perspective_divide(bsp_render_mul(lateral, RAY_PROJ_X), BSP_NEAR);
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
    const s32 proj_lx = bsp_render_mul(RAY_PROJ_X, slx);
    const s32 proj_ly = bsp_render_mul(RAY_PROJ_X, sly);

    const s32 left_base = bsp_render_mul(RAY_PROJ_X, l0) + bsp_render_mul(LEFT_REJECT_SCALE, d0);
    const s32 left_dx = proj_lx + bsp_render_mul(LEFT_REJECT_SCALE, sdx);
    const s32 left_dy = proj_ly + bsp_render_mul(LEFT_REJECT_SCALE, sdy);
    s32 max_left_plane = left_base + (2 * (RAY_PROJ_X + LEFT_REJECT_SCALE));
    if (left_dx > 0) max_left_plane += left_dx;
    if (left_dy > 0) max_left_plane += left_dy;
    if (max_left_plane <= 0) {
        BSP_DBG_INC(boxes_rejected_cheap);
        return FALSE;
    }

    const s32 right_base = bsp_render_mul(RAY_PROJ_X, l0) - bsp_render_mul(RIGHT_REJECT_SCALE, d0);
    const s32 right_dx = proj_lx - bsp_render_mul(RIGHT_REJECT_SCALE, sdx);
    const s32 right_dy = proj_ly - bsp_render_mul(RIGHT_REJECT_SCALE, sdy);
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
    s32 min_screen = RAY_VIEW_CENTER_X + bsp_perspective_divide(
        bsp_render_mul(min_lateral, RAY_PROJ_X), min_denominator);
    s32 max_screen = RAY_VIEW_CENTER_X + bsp_perspective_divide(
        bsp_render_mul(max_lateral, RAY_PROJ_X), max_denominator);

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

void bsp_render_boxed_child(u16 child, const BspBox *box) {
    s16 left;
    s16 right;

    // Gate the expensive projection on full coverage. This test used to live at
    // the top of bsp_render_node, where it was dead: bsp_render_node is only reached
    // from here after bsp_solid_sample_range_filled() returned FALSE, which proves
    // some sample is still open, which proves g_solid_count < BSP_SAMPLE_COLS.
    // Here it does real work — once the view closes, every sibling still
    // pending on the recursion stack used to pay a full bsp_project_box_range plus
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
    const bool projected = bsp_project_box_range(box, &left, &right);
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
    const bool closed = bsp_solid_sample_range_filled(left_sample, right_sample);
    g_cadence_range_closed_subticks += getSubTick() - range_start;
    if (!closed) {
        bsp_render_node(child);
    }
#else
    if (!bsp_solid_sample_range_filled(left_sample, right_sample)) {
        bsp_render_node(child);
    }
#endif
}

void bsp_render_node(u16 child) {
    // No full-coverage test here — see bsp_render_boxed_child, which owns it now.
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
        const s32 cross = bsp_render_mul(g_px - n->px, n->dy) -
                          bsp_render_mul(g_py - n->py, n->dx);
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
        bsp_render_boxed_child(n->front, &n->front_box); // near side first
        bsp_render_boxed_child(n->back, &n->back_box);
    } else {
        bsp_render_boxed_child(n->back, &n->back_box);
        bsp_render_boxed_child(n->front, &n->front_box);
    }
}

void bsp_traverse_front_to_back(const PlayerState *player) {
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

    bsp_render_node(bsp_root_node);
}

void bsp_visit_leaf(u16 subsector_id) {
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
        bsp_draw_seg((u16)(ss->first_seg + i));
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

