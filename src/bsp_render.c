#include "bsp_render.h"
#include "bsp_map.h"
#include "fixed_math.h"

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

static s32 abs_s32(s32 v) {
    return (v < 0) ? -v : v;
}

// Source Doom texture width (px) per texture_id; order matches WALL_TEX[] in
// renderer_scene.c and the converter's wall list:
//   0 STONE 1 DOOR3 2 BIGDOOR2 3 SW1COMP 4 BROWN1 5 GRAY7 6 METAL1 7 STONE2 8 TEKWALL4
static const u16 WALL_TEX_SRC_WIDTH[RAY_TEXTURE_COUNT] = {
    256, 64, 128, 64, 128, 256, 64, 128, 128};

// Horizontal repeat shift = log2(source_width / WALL_TEX_DIM). Both operands are
// powers of two, so this is an exact integer log2 of their ratio. Assumes the
// source is at least WALL_TEX_DIM wide (true for every wall texture: min 64).
static u8 wall_tex_ushift(u8 tid) {
    u16 ratio = (u16)(WALL_TEX_SRC_WIDTH[tid] / WALL_TEX_DIM);
    u8 shift = 0;
    while (ratio > 1) {
        ratio = (u16)(ratio >> 1);
        shift++;
    }
    return shift;
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
    s32 uA = 0;
    s32 uB = wall_len;

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

    const u8 tid = seg->texture_id;
    const u8 shade = (seg->ny != 0) ? 1 : 0; // N/S walls use the shaded copy
    // Per-texture horizontal repeat: the WALL_TEX_DIM-wide texture must span one full
    // period of the source Doom texture (Doom maps 1 texel == 1 world unit), so it
    // repeats every source_width world units instead of a fixed 256. The shift is
    // log2(source_width / WALL_TEX_DIM), derived here from the source widths so it
    // tracks WALL_TEX_DIM automatically instead of being a hand-baked table.
    const u8 ushift = wall_tex_ushift((tid < RAY_TEXTURE_COUNT) ? tid : 0);

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
    for (s32 x = x0; x <= x1; x++) {
        if (g_col_solid[x]) {
            continue;
        }

        const s32 sfix = (((x - xL) * inv_span) >> FX_SHIFT); // 0..256 across span
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
            col->tex_x = (u8)((u_col >> ushift) & WALL_TEX_DIM_MASK);
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
        render_node(n->front); // near side first
        render_node(n->back);
    } else {
        render_node(n->back);
        render_node(n->front);
    }
}

void bsp_init(void) {
    // Nothing to precompute; the collision/LOS broad-phase computes seg AABBs
    // inline from the vertex table (no per-seg RAM on the 64KB MD).
}

void bsp_cast_frame(const PlayerState *player, RayColumn *columns) {
    g_fwx = fx_cos(player->angle);
    g_fwy = fx_sin(player->angle);
    g_rx = fx_cos((u16)(player->angle + ANGLE_90));
    g_ry = fx_sin((u16)(player->angle + ANGLE_90));
    g_px = player->x;
    g_py = player->y;
    g_columns = columns;

    // Clear occlusion and seed every column with a far/empty default so columns
    // no wall covers still render (as distant, mostly sky/floor).
    g_solid_count = 0;
    for (u16 c = 0; c < RAY_VIEW_COLS; c++) {
        g_col_solid[c] = FALSE;
        columns[c].height = 1;
        columns[c].depth = 0x7FFF;
        columns[c].tex_x = 0;
        columns[c].texture_id = 0;
        columns[c].shade = 0;
    }

    render_node(bsp_root_node);
}
