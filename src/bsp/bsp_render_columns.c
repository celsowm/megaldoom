/* BSP wall-column projection and raster payload generation. */
#include "bsp_render_internal.h"

void bsp_seed_column_default(RayColumn *col) {
    col->height = 1;
    col->projected_height = 1;
    col->depth = 0x7FFF;
    col->tex_x = 0;
    col->tex_y = 0;
    col->texture_id = MEGALDOOM_TEX_FALLBACK;
    col->shade = 0;
    col->flags = 0;
}

void bsp_draw_seg(u16 seg_index) {
    if (bsp_seg_is_open(seg_index)) {
        return;
    }

    BSP_DBG_INC(segments_tested);

    const BspSeg *seg = &bsp_segs[seg_index];
    const u16 door_lift = bsp_seg_door_lift(seg_index);
    const bool moving_door = (bool)(seg->type == BSP_SEG_DOOR && door_lift > 0);
    const bool plain_door = (bool)(seg->type == BSP_SEG_DOOR &&
        (seg->flags & BSP_SEG_FLAG_PLAIN_DOOR));
    // A window writes the same near overlay a moving door does, but leaves the
    // column OPEN so the front-to-back walk keeps filling what is behind it.
    const bool window = (bool)(seg->type == BSP_SEG_WINDOW);
    const bool overlay = (bool)(moving_door || window);
    // A sky wall is an ordinary solid wall whose real WAD height is shorter
    // than the engine's 128-unit slab. Its upper gap is sky; the remaining
    // textured span is aligned to the floor. See sky_wall_sector() in
    // tools/doom_map.py for how one is recognised.
    const bool sky_wall = (bool)(seg->type == BSP_SEG_SKY_WALL);
    const BspVertex *a = &bsp_vertices[seg->v1];

    // Backface / one-sided cull: draw only when the camera is on the seg's
    // front side (the side its normal points toward).
    const s32 facing = bsp_render_mul(g_px - a->x, seg->nx) +
                       bsp_render_mul(g_py - a->y, seg->ny);
    if (facing <= 0) {
        return;
    }

    // Transform both endpoints into view space (depth = forward, lat = right).
    // Shared endpoints hit the generation cache after their first segment.
    s32 depthA;
    s32 latA;
    s32 depthB;
    s32 latB;
    bsp_transform_vertex(seg->v1, &depthA, &latA);
    bsp_transform_vertex(seg->v2, &depthB, &latB);

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
        const s32 t = bsp_perspective_divide(((s32)BSP_NEAR - depthA) << FX_SHIFT,
                                         depthB - depthA);
        latA += bsp_render_mul(latB - latA, t) >> FX_SHIFT;
        uA += bsp_native_mul_long_unsigned(uB - uA, (u16)t) >> FX_SHIFT;
        depthA = BSP_NEAR;
    } else if (depthB < BSP_NEAR) {
        const s32 t = bsp_perspective_divide(((s32)BSP_NEAR - depthB) << FX_SHIFT,
                                         depthA - depthB);
        latB += bsp_render_mul(latA - latB, t) >> FX_SHIFT;
        uB += bsp_native_mul_long_unsigned(uA - uB, (u16)t) >> FX_SHIFT;
        depthB = BSP_NEAR;
    }

    // Project to screen x.
    s32 xa = RAY_VIEW_CENTER_X + bsp_perspective_divide(bsp_render_mul(latA, RAY_PROJ_X), depthA);
    s32 xb = RAY_VIEW_CENTER_X + bsp_perspective_divide(bsp_render_mul(latB, RAY_PROJ_X), depthB);
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
    const s32 invzL = bsp_reciprocal_depth(depthL);
    const s32 invzR = bsp_reciprocal_depth(depthR);
    const s32 uzL = bsp_native_mul_long_unsigned(uL, (u16)invzL);
    const s32 uzR = bsp_native_mul_long_unsigned(uR, (u16)invzR);
    // Reciprocal of the span, Q8, computed once per seg so the inner loop's
    // horizontal fraction is a multiply instead of a per-column divide. Matches
    // the old ((x-xL)<<8)/span to within the reciprocal's truncation.
    const s32 inv_span = bsp_reciprocal_span(span);

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
    u16 sample = bsp_find_next_open(first_sample);
    while (sample <= last_sample) {
        const s32 x = (s32)sample * RAY_COL_STRIDE;
        // x - xL fits u16: x <= 159 and xL >= -(s16 quotient + margin), so the
        // difference is in [0, ~33008]. inv_span <= 65536/2 fits u16 (the
        // span == 1 case never reaches the multiply). One MULU.W therefore
        // produces the exact same product as the 32x16 helper it replaces.
        const s32 sfix = (span == 1) ? 0 :
            (s32)(bsp_native_mulu_word((u16)(x - xL), (u16)inv_span) >> FX_SHIFT);
        RayColumn *col = &g_columns[x];

        const s32 invz = invzL + (bsp_render_mul(invzR - invzL, sfix) >> FX_SHIFT);
        if (invz <= 0) {
            sample = bsp_find_next_open((u16)(sample + 1));
            continue;
        }

        // BSP_INV_SCALE/invz via a precomputed table: bsp_draw_seg's near-plane
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

        const s32 uz = uzL + (bsp_native_mul_long_unsigned(uzR - uzL, (u16)sfix) >> FX_SHIFT);
        const s32 u_col = bsp_perspective_divide(uz, invz);

        // height = RAY_PROJ_Y*RAY_WORLD_WALL_HEIGHT / depth_col, but depth_col =
        // INV_SCALE/invz, so fold to a multiply+shift and skip a divide.
        // Keep the real projected height for the vertical texture lookup. The
        // visible slab is clipped to the viewport, but using the clipped 120px
        // height as the texture scale remaps a whole door texture into the
        // screen when the player is pressed against it (depth 16 can project
        // a 128-unit wall to 640px).
        s32 projected_height =
            bsp_render_mul(RAY_PROJ_Y * RAY_WORLD_WALL_HEIGHT, invz) >> 14;
        if (projected_height < 1) {
            projected_height = 1;
        }
        const u16 texture_projected_height =
            (projected_height > RAY_MAX_PROJECTED_WALL_HEIGHT) ?
                RAY_MAX_PROJECTED_WALL_HEIGHT : (u16)projected_height;
        u16 height = (projected_height > RAY_VIEW_ROWS) ?
            RAY_VIEW_ROWS : (u16)projected_height;
        if (sky_wall) {
            // Project the whole 128-unit reference slab, then discard only its
            // Q8 upper sky band. Clamping the two absolute edges (instead of
            // clamping the shortened height) keeps a near wall anchored to the
            // floor when its top and/or bottom extend beyond the viewport.
            const s32 slab_top = (RAY_VIEW_ROWS - projected_height) / 2;
            s32 wall_top = slab_top +
                ((projected_height * bsp_seg_sky_gap_top(seg)) >> 8);
            s32 wall_bottom = slab_top + projected_height;
            if (wall_top < 0) wall_top = 0;
            if (wall_top > RAY_VIEW_ROWS) wall_top = RAY_VIEW_ROWS;
            if (wall_bottom < 0) wall_bottom = 0;
            if (wall_bottom > RAY_VIEW_ROWS) wall_bottom = RAY_VIEW_ROWS;
            height = (wall_bottom > wall_top) ?
                (u16)(wall_bottom - wall_top) : 0;
        }

        // u_col is a DIVS.W quotient (fits s16 by the same map-bounds contract
        // bsp_perspective_divide relies on) and is non-negative: uz interpolates
        // between two non-negative endpoint products, so a single MULU.W is
        // exact. Max product 32767 * 65535 still fits u32/s32.
        const s32 scaled_u = (s32)(bsp_native_mulu_word((u16)u_col, u_scale_q12) >> 12);
        if (overlay) {
            RayDoorOverlay *near = &col->door;
            if (near->height != 0 && depth_col >= near->depth) {
                sample = bsp_find_next_open((u16)(sample + 1));
                continue;
            }
            near->height = (u8)height;
            near->depth = (u16)depth_col;
            // lift == 0 is the window discriminator; a moving door is always
            // 1..255 here because bsp_draw_seg returns early on a fully open
            // door and a closed one never takes this branch.
            near->lift = window ? 0 : (u8)door_lift;
            // Resolve the seg's Q8 band against the slab HERE, once per sampled
            // column, and store absolute viewport rows. Both consumers used to
            // redo this multiply -- and the billboard clip redid it per byte per
            // sprite row. The slab top is the same centring the pack stage
            // applies (describe_textured_column: (VIEW_PIXEL_H - height) / 2),
            // and height <= RAY_VIEW_ROWS, so both rows fit the byte that
            // RayDoorOverlay's 10-byte budget allows.
            const u16 slab_top = (u16)((RAY_VIEW_ROWS - height) / 2);
            near->band_top = window ? (u8)(slab_top +
                (((u16)height * bsp_seg_window_band_top(seg)) >> 8)) :
                (plain_door ? RAY_OVERLAY_FLAG_PLAIN_DOOR : 0);
            near->band_bottom = window ? (u8)(slab_top +
                (((u16)height * bsp_seg_window_band_bottom(seg)) >> 8)) : 0;
            near->tex_x = (u8)(scaled_u & WALL_TEX_WIDTH_MASK);
            near->tex_y = seg->tex_v_offset;
            near->texture_id = tid;
            near->shade = shade;
            // This branch deliberately does NOT close the sample the way the
            // wall branch below does: the column stays open so a farther wall
            // (or the ceiling/floor seed) supplies what shows through the gap.
            // That is the whole see-through mechanism, and
            // tools/test-door-animation.py checks this branch for the absence
            // of that call -- so do not name it here either.
        } else {
            col->height = (u16)height;
            col->projected_height = texture_projected_height;
            col->depth = (u16)depth_col;
            col->tex_x = (u8)(scaled_u & WALL_TEX_WIDTH_MASK);
            // The ordinary vertical table addresses the complete 128-unit
            // slab. Move its phase back by the omitted world-space sky gap so
            // texture row zero begins at the low wall's real top.
            col->tex_y = sky_wall ?
                (u8)(seg->tex_v_offset - (bsp_seg_sky_gap_top(seg) >> 1)) :
                seg->tex_v_offset;
            col->texture_id = tid;
            col->shade = shade;
            col->flags = sky_wall ? RAY_COLUMN_FLAG_FLOOR_ALIGNED :
                ((seg->type == BSP_SEG_DOOR && !plain_door) ?
                    RAY_COLUMN_FLAG_DOOR : 0);
            bsp_mark_sample_solid(sample);
        }
        drew_any = TRUE;
#if CADENCE_STAGE_PROBE
        g_cadence_samples++;
#endif
        sample = overlay ? bsp_find_next_open((u16)(sample + 1)) :
                           bsp_find_next_open(sample);
    }
#if CADENCE_STAGE_PROBE && CADENCE_DRAWSEG_SPLIT
    g_cadence_sample_subticks += getSubTick() - sample_loop_start;
#endif
    if (drew_any) {
        bsp_automap_mark_seg(seg_index);
        BSP_DBG_INC(segments_drawn);
    }
}

// Fill only the samples left open by the front-to-back wall pass. The normal
// E1M1 view closes all samples; this remains the correct sky/floor fallback.
void bsp_seed_unclaimed_columns(RayColumn *columns) {
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
                bsp_seed_column_default(&columns[(u16)((base + bit) * RAY_COL_STRIDE)]);
            }
        }
    }
}
