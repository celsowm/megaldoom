#include "bsp_render.h"
#include "bsp_map.h"
#include "fixed_math.h"
#include "generated_assets.h"

// draw_seg only computes the height/texture fields at columns on a RAY_COL_STRIDE
// boundary, matching the columns build_raycast_tilemap actually samples. That
// relies on the stride being a power of two (the (x & (STRIDE-1)) test) and on
// the renderer sampling at multiples of it (enforced there by RAY_COL_STRIDE==4).
#if (RAY_COL_STRIDE & (RAY_COL_STRIDE - 1)) != 0
#error "draw_seg's strided-column optimization requires RAY_COL_STRIDE to be a power of two"
#endif

// --- Projection constants ---------------------------------------------------
// View is RAY_VIEW_COLS (160) px wide; center column is 80. PROJ is tuned so the
// horizontal field of view matches the raycaster's 48 degrees:
//   at the screen edge, |lateral/depth| = tan(24deg) ~= 0.445 maps to +/-80,
//   so PROJ = 80 / 0.445 ~= 180.
#define BSP_VIEW_CENTER_X (RAY_VIEW_COLS / 2)
#define BSP_PROJ 180
#define BSP_VIEW_PIXEL_H 120 // matches raycaster's RAY_VIEW_PIXEL_H
#define BSP_NEAR 16          // near clip plane, in world units
#define BSP_INV_SCALE 16384  // fixed-point scale for 1/depth interpolation (1<<14)

// Per-frame view basis + camera, set at the top of bsp_cast_frame().
static s16 g_fwx, g_fwy; // forward axis (cos, sin), Q8
static s16 g_rx, g_ry;   // right axis, Q8
static s32 g_px, g_py;   // camera position, world units

static bool g_col_solid[RAY_VIEW_COLS];
static u16 g_solid_count;
static RayColumn *g_columns;

static void render_node(u16 child);

static s32 abs_s32(s32 v) {
    return (v < 0) ? -v : v;
}

// Draw one one-sided wall segment into the RayColumn buffer, respecting the
// solid-column occlusion buffer. Uniform full-height wall. Open doors are
// skipped (rendered as a passable gap).
static void draw_seg(u16 seg_index) {
    if (bsp_seg_is_open(seg_index)) {
        return;
    }

    const BspSeg *seg = &bsp_segs[seg_index];
    const BspVertex *a = &bsp_vertices[seg->v1];
    const BspVertex *b = &bsp_vertices[seg->v2];

    // Backface / one-sided cull: draw only when the camera is on the seg's
    // front side (the side its normal points toward).
    const s32 facing = (g_px - a->x) * seg->nx + (g_py - a->y) * seg->ny;
    if (facing <= 0) {
        return;
    }

    // Transform both endpoints into view space (depth = forward, lat = right).
    s32 relx = (s32)a->x - g_px;
    s32 rely = (s32)a->y - g_py;
    s32 depthA = (relx * g_fwx + rely * g_fwy) >> FX_SHIFT;
    s32 latA = (relx * g_rx + rely * g_ry) >> FX_SHIFT;

    relx = (s32)b->x - g_px;
    rely = (s32)b->y - g_py;
    s32 depthB = (relx * g_fwx + rely * g_fwy) >> FX_SHIFT;
    s32 latB = (relx * g_rx + rely * g_ry) >> FX_SHIFT;

    // Texture coordinate along the wall (world units), repeating every 256 px
    // like the raycaster. u goes 0 -> wall_length from v1 -> v2.
    const s32 wall_len = abs_s32((s32)b->x - a->x) + abs_s32((s32)b->y - a->y);
    s32 uA = seg->tex_u_offset;
    s32 uB = (s32)seg->tex_u_offset + wall_len;

    // Near-plane clipping.
    if (depthA < BSP_NEAR && depthB < BSP_NEAR) {
        return;
    }
    if (depthA < BSP_NEAR) {
        const s32 t = (((s32)BSP_NEAR - depthA) << FX_SHIFT) / (depthB - depthA);
        latA += ((latB - latA) * t) >> FX_SHIFT;
        uA += ((uB - uA) * t) >> FX_SHIFT;
        depthA = BSP_NEAR;
    } else if (depthB < BSP_NEAR) {
        const s32 t = (((s32)BSP_NEAR - depthB) << FX_SHIFT) / (depthA - depthB);
        latB += ((latA - latB) * t) >> FX_SHIFT;
        uB += ((uA - uB) * t) >> FX_SHIFT;
        depthB = BSP_NEAR;
    }

    // Project to screen x.
    s32 xa = BSP_VIEW_CENTER_X + (latA * BSP_PROJ) / depthA;
    s32 xb = BSP_VIEW_CENTER_X + (latB * BSP_PROJ) / depthB;
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
    const s32 invzL = BSP_INV_SCALE / depthL;
    const s32 invzR = BSP_INV_SCALE / depthR;
    const s32 uzL = uL * invzL;
    const s32 uzR = uR * invzR;
    // Reciprocal of the span, Q8, computed once per seg so the inner loop's
    // horizontal fraction is a multiply instead of a per-column divide. Matches
    // the old ((x-xL)<<8)/span to within the reciprocal's truncation.
    const s32 inv_span = ((s32)FX_ONE << FX_SHIFT) / span;

    const u8 tid = (seg->texture_id < FREEDOOM_WALL_TEXTURE_COUNT) ?
                       seg->texture_id : MEGALDOOM_TEX_FALLBACK;
    const u8 shade = (seg->ny != 0) ? 1 : 0; // N/S walls use the shaded copy
    // Per-texture horizontal repeat: the WALL_TEX_DIM-wide texture must span one full
    // period of the source Doom texture (Doom maps 1 texel == 1 world unit), so it
    // repeats every source_width world units instead of a fixed 256. The shift is
    // log2(source_width / WALL_TEX_DIM), derived here from the source widths so it
    // tracks WALL_TEX_DIM automatically instead of being a hand-baked table.
    const s8 ushift = FREEDOOM_WALL_TEXTURE_USHIFT[tid];

    s32 x0 = xL;
    s32 x1 = xR - 1;
    if (x0 < 0) x0 = 0;
    if (x1 > RAY_VIEW_COLS - 1) x1 = RAY_VIEW_COLS - 1;

    // depth carried to the in-between (non-sampled) columns for billboard occlusion.
    // Only wall columns on a RAY_COL_STRIDE boundary are ever read for height/texture
    // by build_raycast_tilemap; the columns between them contribute nothing to the
    // wall image, so their depth just needs to be close enough (<= STRIDE-1 px off)
    // for billboard-vs-wall occlusion. Carrying the last boundary depth avoids a
    // per-column divide entirely on those columns.
    s32 carry_depth = 0x7FFF;
    s32 sfix_acc = (x0 - xL) * inv_span;
    for (s32 x = x0; x <= x1; x++) {
        const s32 sfix = (sfix_acc >> FX_SHIFT); // 0..256 across span
        sfix_acc += inv_span;

        if (g_col_solid[x]) {
            continue;
        }

        const s32 invz = invzL + (((invzR - invzL) * sfix) >> FX_SHIFT);
        if (invz <= 0) {
            continue;
        }

        RayColumn *col = &g_columns[x];

        if ((x & (RAY_COL_STRIDE - 1)) == 0) {
            s32 depth_col = BSP_INV_SCALE / invz;
            if (depth_col < 1) {
                depth_col = 1;
            }

            const s32 uz = uzL + (((uzR - uzL) * sfix) >> FX_SHIFT);
            const s32 u_col = uz / invz;

            // height = BSP_VIEW_PIXEL_H*FX_ONE / depth_col, but depth_col = INV_SCALE/invz,
            // so fold to a multiply+shift and skip a divide. invz > 0 here (guarded), and
            // BSP_VIEW_PIXEL_H*FX_ONE*invz (<= ~31M) fits in s32.
            s32 height = ((s32)BSP_VIEW_PIXEL_H * FX_ONE * invz) >> 14; // BSP_INV_SCALE == 1<<14
            if (height < 1) {
                height = 1;
            } else if (height > BSP_VIEW_PIXEL_H) {
                height = BSP_VIEW_PIXEL_H;
            }

            col->height = (u16)height;
            col->depth = (u16)depth_col;
            const s32 scaled_u = (ushift >= 0) ? (u_col >> ushift) :
                                                   (u_col * ((s32)1 << -ushift));
            col->tex_x = (u8)(scaled_u & WALL_TEX_DIM_MASK);
            col->tex_y = seg->tex_v_offset;
            col->texture_id = tid;
            col->shade = shade;
            carry_depth = depth_col;
        } else {
            col->depth = (u16)carry_depth;
        }

        g_col_solid[x] = TRUE;
        g_solid_count++;
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

    const s16 xs[4] = {box->min_x, box->max_x, box->max_x, box->min_x};
    const s16 ys[4] = {box->min_y, box->min_y, box->max_y, box->max_y};
    s32 depths[4];
    s32 laterals[4];
    s32 min_depth = 0x7FFFFFFF;
    s32 max_depth = -0x7FFFFFFF;

    for (u16 i = 0; i < 4; i++) {
        const s32 relx = (s32)xs[i] - g_px;
        const s32 rely = (s32)ys[i] - g_py;
        const s32 depth = (relx * g_fwx + rely * g_fwy) >> FX_SHIFT;
        const s32 lateral = (relx * g_rx + rely * g_ry) >> FX_SHIFT;
        depths[i] = depth;
        laterals[i] = lateral;
        if (depth < min_depth) min_depth = depth;
        if (depth > max_depth) max_depth = depth;
    }

    if (max_depth < BSP_NEAR) {
        return FALSE;
    }

    // A box crossing the near plane can cover arbitrarily wide screen ranges;
    // visiting it is the conservative choice.
    if (min_depth < BSP_NEAR) {
        *left = 0;
        *right = RAY_VIEW_COLS - 1;
        return TRUE;
    }

    s32 min_screen = 0x7FFFFFFF;
    s32 max_screen = -0x7FFFFFFF;
    for (u16 i = 0; i < 4; i++) {
        const s32 screen = BSP_VIEW_CENTER_X + (laterals[i] * BSP_PROJ) / depths[i];
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

    if (!project_box_range(box, &left, &right)) {
        return;
    }

    // Full-height solid walls make horizontal coverage sufficient: when every
    // column in the child's conservative range is already filled front-to-back,
    // no geometry in that child can change the frame.
    for (s16 x = left; x <= right; x++) {
        if (!g_col_solid[x]) {
            render_node(child);
            return;
        }
    }
}

static void render_node(u16 child) {
    if (g_solid_count >= RAY_VIEW_COLS) {
        return; // whole view already filled front-to-back
    }

    if (BSP_CHILD_IS_SUBSECTOR(child)) {
        const BspSubsector *ss = &bsp_subsectors[BSP_CHILD_INDEX(child)];
        for (u16 i = 0; i < ss->seg_count; i++) {
            draw_seg((u16)(ss->first_seg + i));
        }
        return;
    }

    const BspNode *n = &bsp_nodes[child];
    // Which side of the partition line is the camera on?
    const s32 cross = (g_px - n->px) * n->dy - (g_py - n->py) * n->dx;
    if (cross >= 0) {
        render_boxed_child(n->front, &n->front_box); // near side first
        render_boxed_child(n->back, &n->back_box);
    } else {
        render_boxed_child(n->back, &n->back_box);
        render_boxed_child(n->front, &n->front_box);
    }
}

void bsp_init(void) {
    // Nothing to precompute; the collision/LOS broad-phase computes seg AABBs
    // inline from the vertex table (no per-seg RAM on the 64KB MD).
}

static u16 find_point_subsector(s32 px, s32 py) {
    u16 child = bsp_root_node;
    while (!BSP_CHILD_IS_SUBSECTOR(child)) {
        const BspNode *node = &bsp_nodes[BSP_CHILD_INDEX(child)];
        const s32 cross = (px - node->px) * node->dy - (py - node->py) * node->dx;
        child = (cross >= 0) ? node->front : node->back;
    }
    return BSP_CHILD_INDEX(child);
}

void bsp_cast_frame(const PlayerState *player, RayColumn *columns, RaySceneColors *scene_colors) {
    g_fwx = fx_cos(player->angle);
    g_fwy = fx_sin(player->angle);
    g_rx = fx_cos((u16)(player->angle + ANGLE_90));
    g_ry = fx_sin((u16)(player->angle + ANGLE_90));
    g_px = player->x;
    g_py = player->y;
    g_columns = columns;

    const BspSubsector *camera_subsector = &bsp_subsectors[find_point_subsector(g_px, g_py)];
    const BspSectorVisual *visual = &bsp_sector_visuals[camera_subsector->sector_id];
    scene_colors->ceiling_color = visual->ceiling_color;
    scene_colors->floor_color = visual->floor_color;

    // Clear occlusion and seed every column with a far/empty default so columns
    // no wall covers still render (as distant, mostly sky/floor).
    g_solid_count = 0;
    for (u16 c = 0; c < RAY_VIEW_COLS; c++) {
        g_col_solid[c] = FALSE;
        columns[c].height = 1;
        columns[c].depth = 0x7FFF;
        columns[c].tex_x = 0;
        columns[c].tex_y = 0;
        columns[c].texture_id = MEGALDOOM_TEX_FALLBACK;
        columns[c].shade = 0;
    }

    render_node(bsp_root_node);
}
