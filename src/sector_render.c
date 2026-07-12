#include <genesis.h>
#include <string.h>
#include "bsp_render.h"
#include "bsp_map.h"
#include "bsp_traverse.h"
#include "fixed_math.h"
#include "generated_assets.h"
#include "generated_renderer_assets.h"
#include "renderer_internal.h"

#if BSP_SECTOR_RENDERER

#define SECTOR_NEAR 16
#define SCENE_FAR 0xFFFFu
#define PLANE_DEPTH 0xFFFEu
#define INTERP_SHIFT 8
#define MAX_PLANE_VERTICES 10
#define MAX_VISIBLE_SUBSECTORS 256
#define MAX_SUBSECTOR_SEGS 16
#define SECTOR_CLOSED_WORD_COUNT ((RAY_SAMPLE_COLS + 31) / 32)
#define SECTOR_VERTEX_CACHE_CAP 32

static u16 g_scene_depth[RAY_SAMPLE_COLS][RAY_VIEW_ROWS];
static u8 g_depth_block_generation[RAY_SAMPLE_COLS][VIEW_TILE_H];
static u8 g_depth_generation;
static const u16 g_far_depth_block[8] = {
    SCENE_FAR, SCENE_FAR, SCENE_FAR, SCENE_FAR,
    SCENE_FAR, SCENE_FAR, SCENE_FAR, SCENE_FAR
};
static s16 g_ceiling_clip[RAY_SAMPLE_COLS];
static s16 g_floor_clip[RAY_SAMPLE_COLS];
static bool g_column_closed[RAY_SAMPLE_COLS];
static u32 g_closed_words[SECTOR_CLOSED_WORD_COUNT];
static u16 g_closed_count;
static u16 g_visible_subsectors[MAX_VISIBLE_SUBSECTORS];
static u16 g_visible_subsector_count;
static s16 g_plane_left[RAY_VIEW_ROWS];
static s16 g_plane_right[RAY_VIEW_ROWS];
static s32 g_vertex_depth[SECTOR_VERTEX_CACHE_CAP];
static s32 g_vertex_lateral[SECTOR_VERTEX_CACHE_CAP];
static u16 g_vertex_id[SECTOR_VERTEX_CACHE_CAP];
static u8 g_vertex_generation[SECTOR_VERTEX_CACHE_CAP];
static u8 g_transform_generation;
static const u16 SECTOR_REP4[16] = {
    0x0000, 0x1111, 0x2222, 0x3333,
    0x4444, 0x5555, 0x6666, 0x7777,
    0x8888, 0x9999, 0xAAAA, 0xBBBB,
    0xCCCC, 0xDDDD, 0xEEEE, 0xFFFF
};
#if DEBUG_PERF
static u32 g_debug_flat_subticks;
static u32 g_debug_wall_subticks;
static u32 g_debug_floor_subticks;
static u32 g_debug_transform_subticks;
static u32 g_debug_setup_subticks;
static u32 g_debug_raster_subticks;

u32 bsp_sector_get_debug_flat_subticks(void) { return g_debug_flat_subticks; }
u32 bsp_sector_get_debug_wall_subticks(void) { return g_debug_wall_subticks; }
u32 bsp_sector_get_debug_floor_subticks(void) { return g_debug_floor_subticks; }
u32 bsp_sector_get_debug_transform_subticks(void) { return g_debug_transform_subticks; }
u32 bsp_sector_get_debug_setup_subticks(void) { return g_debug_setup_subticks; }
u32 bsp_sector_get_debug_raster_subticks(void) { return g_debug_raster_subticks; }
#endif

typedef struct {
    const PlayerState *player;
    s16 fwx;
    s16 fwy;
    s16 rx;
    s16 ry;
    s16 base_floor_height;
    u8 base_floor_color;
} SectorRenderContext;

typedef struct {
    s32 da;
    s32 la;
    s32 db;
    s32 lb;
} RenderSegView;

static void transform_vertex(const SectorRenderContext *context, u16 vertex_id,
                             s32 *depth, s32 *lateral);

const u16 *bsp_sector_depth_block(u16 sample_x, u16 tile_y) {
    if (sample_x >= RAY_SAMPLE_COLS || tile_y >= VIEW_TILE_H ||
        g_depth_block_generation[sample_x][tile_y] != g_depth_generation) {
        return g_far_depth_block;
    }
    return &g_scene_depth[sample_x][tile_y * 8];
}

u16 bsp_sector_depth_at(u16 x, u16 y) {
    const u16 *block;
    if (x >= RAY_SAMPLE_COLS || y >= RAY_VIEW_ROWS) return SCENE_FAR;
    block = bsp_sector_depth_block(x, (u16)(y >> 3));
    return block[y & 7];
}

static u16 *ensure_depth_block(u16 sample_x, u16 tile_y) {
    u16 *block = &g_scene_depth[sample_x][tile_y * 8];
    if (g_depth_block_generation[sample_x][tile_y] != g_depth_generation) {
        u32 *wide = (u32 *)block;
        wide[0] = 0xFFFFFFFFu;
        wide[1] = 0xFFFFFFFFu;
        wide[2] = 0xFFFFFFFFu;
        wide[3] = 0xFFFFFFFFu;
        g_depth_block_generation[sample_x][tile_y] = g_depth_generation;
    }
    return block;
}

static u16 *base_sample_rows(u16 sample_x, u16 tile_y) {
    u32 *tile = g_view_tiles[(tile_y * VIEW_TILE_W) + (sample_x >> 1)];
    return ((u16 *)tile) + (sample_x & 1);
}

// Exact unsigned 32/16=32 division using two hardware DIVU.W operations. The
// high-word quotient and the remainder-fed low-word quotient each fit u16.
static u32 sector_divu32_16_exact(u32 numerator, u16 denominator) {
    const u16 high = (u16)(numerator >> 16);
    const u16 quotient_high = divu(high, denominator);
    const u16 remainder_high =
        (u16)(high - ((u32)quotient_high * denominator));
    const u32 low_numerator =
        ((u32)remainder_high << 16) | (numerator & 0xFFFFu);
    const u16 quotient_low = divu(low_numerator, denominator);
    return ((u32)quotient_high << 16) | quotient_low;
}

// The 68000 has no native signed 32-bit quotient. Normalize signs and use the
// exact two-stage 32/16 divider whenever the divisor fits u16; retain the C
// fallback only for the exceptional wider-divisor and INT_MIN/-1 cases.
static s32 fast_signed_divide(s32 numerator, s32 denominator) {
    if (denominator != 0 && denominator >= -32767L && denominator <= 32767L &&
        !((numerator == (-2147483647L - 1L)) && (denominator < 0))) {
        s32 normalized_numerator = numerator;
        s32 normalized_denominator = denominator;
        if (normalized_denominator < 0) {
            normalized_numerator = -normalized_numerator;
            normalized_denominator = -normalized_denominator;
        }
        {
            const s32 magnitude_limit = normalized_denominator << 15;
            if (normalized_numerator >= -magnitude_limit &&
                normalized_numerator <= magnitude_limit - normalized_denominator) {
                return (s32)divs(normalized_numerator,
                                 (s16)normalized_denominator);
            }
        }
    }

    if (denominator != 0 && denominator >= -65535L && denominator <= 65535L &&
        !((numerator == (-2147483647L - 1L)) && (denominator == -1))) {
        if (numerator == (-2147483647L - 1L) && denominator == 1) {
            return numerator;
        }
        const bool negative = (bool)((numerator < 0) != (denominator < 0));
        const u32 numerator_magnitude = (numerator < 0) ?
            (u32)(-(numerator + 1)) + 1u : (u32)numerator;
        const u16 denominator_magnitude = (u16)((denominator < 0) ?
            -denominator : denominator);
        const u32 quotient =
            sector_divu32_16_exact(numerator_magnitude, denominator_magnitude);
        return negative ? -(s32)quotient : (s32)quotient;
    }

    return numerator / denominator;
}

static s16 project_world_z(s16 world_z, s16 view_z, s32 depth) {
    if (depth < 1) depth = 1;
    return (s16)(RAY_VIEW_CENTER_Y -
        fast_signed_divide((s32)(world_z - view_z) * RAY_PROJ_Y, depth));
}

static s32 abs32(s32 value) { return value < 0 ? -value : value; }

static void draw_span(u16 sx, s16 top, s16 bottom, u16 depth, u8 texture,
                      u8 tex_x) {
    const s16 clip_top = (s16)(g_ceiling_clip[sx] + 1);
    const s16 clip_bottom = (s16)(g_floor_clip[sx] - 1);
    if (top < clip_top) top = clip_top;
    if (bottom > clip_bottom) bottom = clip_bottom;
    if (top < 0) top = 0;
    if (bottom >= RAY_VIEW_ROWS) bottom = RAY_VIEW_ROWS - 1;
    if (top > bottom || texture == 0xFF) return;
    if (texture >= FREEDOOM_WALL_TEXTURE_COUNT) texture = MEGALDOOM_TEX_FALLBACK;

    {
        const u16 height = (u16)(bottom - top + 1);
        const u8 *texture_pixels = &FREEDOOM_WALL_TEXTURES[texture][0][0];
        const u8 *tex_y_rows = MEGALDOOM_WALL_TEX_Y_BY_HEIGHT[height];
        s16 y = top;
        u16 relative_y = 0;

        while (y <= bottom) {
            const u16 tile_y = (u16)(y >> 3);
            const s16 block_bottom = (s16)(((tile_y + 1) << 3) - 1);
            const s16 stop = block_bottom < bottom ? block_bottom : bottom;
            const u16 start_row = (u16)(y & 7);
            u16 *depth_row = ensure_depth_block(sx, tile_y) + start_row;
            u16 *color_row = base_sample_rows(sx, tile_y) + (start_row * 2);
            const u8 *tex_y_row = tex_y_rows + relative_y;
            u16 count = (u16)(stop - y + 1);

            while (count-- > 0) {
                const u8 tex_y = *tex_y_row++ & WALL_TEX_DIM_MASK;
                if (depth < *depth_row) {
                    const u8 color =
                        texture_pixels[(tex_y * WALL_TEX_DIM) + tex_x] & 15;
                    *depth_row = depth;
                    *color_row = SECTOR_REP4[color];
                }
                depth_row++;
                color_row += 2;
            }
            relative_y = (u16)(relative_y + stop - y + 1);
            y = (s16)(stop + 1);
        }
    }
}

static void draw_solid_span(u16 sx, s16 top, s16 bottom, u16 depth, u8 color) {
    const u16 packed_color = SECTOR_REP4[color & 15];
    const s16 clip_top = (s16)(g_ceiling_clip[sx] + 1);
    const s16 clip_bottom = (s16)(g_floor_clip[sx] - 1);
    if (top < clip_top) top = clip_top;
    if (bottom > clip_bottom) bottom = clip_bottom;
    if (top < 0) top = 0;
    if (bottom >= RAY_VIEW_ROWS) bottom = RAY_VIEW_ROWS - 1;
    while (top <= bottom) {
        const u16 tile_y = (u16)(top >> 3);
        const s16 block_bottom = (s16)(((tile_y + 1) << 3) - 1);
        const s16 stop = block_bottom < bottom ? block_bottom : bottom;
        const u16 start_row = (u16)(top & 7);
        u16 *depth_row = ensure_depth_block(sx, tile_y) + start_row;
        u16 *color_row = base_sample_rows(sx, tile_y) + (start_row * 2);
        u16 count = (u16)(stop - top + 1);
        while (count-- > 0) {
            if (depth < *depth_row) {
                *depth_row = depth;
                *color_row = packed_color;
            }
            depth_row++;
            color_row += 2;
        }
        top = (s16)(stop + 1);
    }
}

static void close_column(u16 sx) {
    if (g_column_closed[sx]) return;
    g_column_closed[sx] = TRUE;
    g_closed_words[sx >> 5] |= (u32)1u << (sx & 31);
    g_ceiling_clip[sx] = RAY_VIEW_ROWS - 1;
    g_floor_clip[sx] = 0;
    g_closed_count++;
}

static s32 interpolate_at(s32 a, s32 b, s32 xa, s32 span, s32 screen_x) {
    return (a << INTERP_SHIFT) +
        fast_signed_divide(((b - a) << INTERP_SHIFT) * (screen_x - xa), span);
}

static s32 interpolation_step(s32 a, s32 b, s32 span) {
    return fast_signed_divide(
        ((b - a) << INTERP_SHIFT) * RAY_COL_STRIDE, span);
}

static s16 clamp_screen_x(s32 x) {
    if (x < -512) return -512;
    if (x > 512) return 512;
    return (s16)x;
}

static void add_plane_edge(s16 x0, s16 y0, s16 x1, s16 y1) {
    if (y0 == y1) return;
    if (y0 > y1) {
        s16 temp = x0; x0 = x1; x1 = temp;
        temp = y0; y0 = y1; y1 = temp;
    }
    if (y1 < RAY_VIEW_CENTER_Y || y0 >= RAY_VIEW_ROWS) return;
    {
        const s32 dy = (s32)y1 - y0;
        const s32 step = fast_signed_divide(
            ((s32)x1 - x0) << INTERP_SHIFT, dy);
        s16 start = y0 < RAY_VIEW_CENTER_Y ? RAY_VIEW_CENTER_Y : y0;
        s16 end = y1 >= RAY_VIEW_ROWS ? RAY_VIEW_ROWS - 1 : y1;
        s32 xq = ((s32)x0 << INTERP_SHIFT) + step * (start - y0);
        for (s16 y = start; y <= end; y++) {
            const s16 x = (s16)(xq >> INTERP_SHIFT);
            if (x < g_plane_left[y]) g_plane_left[y] = x;
            if (x > g_plane_right[y]) g_plane_right[y] = x;
            xq += step;
        }
    }
}

static void draw_floor_subsector(const SectorRenderContext *context,
                                 u16 subsector_id) {
    const BspRenderSubsector *subsector = &bsp_render_subsectors[subsector_id];
    const BspSectorState *state;
    u8 color;

    if (subsector->seg_count < 3 || subsector->sector_id >= bsp_sector_count) return;
    state = bsp_get_sector_state(subsector->sector_id);
    if (!state || state->floor_height >= context->player->view_z) return;

    color = bsp_sectors[subsector->sector_id].floor_color & 15;
    // clear_scene_flat already painted this exact floor plane across the lower
    // half. Re-rasterizing same-height/same-colour subsectors only changes FAR
    // to PLANE_DEPTH; all real object depths are nearer, so the visible result
    // and billboard visibility are identical.
    if (state->floor_height == context->base_floor_height &&
        color == context->base_floor_color) {
        return;
    }

    {
        s32 input_lat[MAX_PLANE_VERTICES];
        s32 input_depth[MAX_PLANE_VERTICES];
        s32 clipped_lat[MAX_PLANE_VERTICES];
        s32 clipped_depth[MAX_PLANE_VERTICES];
        u16 input_count = subsector->seg_count;
        u16 clipped_count = 0;
        u16 previous;
        s16 screen_x[MAX_PLANE_VERTICES];
        s16 screen_y[MAX_PLANE_VERTICES];

        if (input_count > MAX_PLANE_VERTICES - 2) input_count = MAX_PLANE_VERTICES - 2;
        for (u16 i = 0; i < input_count; i++) {
            const BspRenderSeg *seg = &bsp_render_segs[subsector->first_seg + i];
            transform_vertex(context, seg->v1, &input_depth[i], &input_lat[i]);
        }

        previous = (u16)(input_count - 1);
        for (u16 current = 0; current < input_count; current++) {
            const bool previous_inside = input_depth[previous] >= SECTOR_NEAR;
            const bool current_inside = input_depth[current] >= SECTOR_NEAR;
            if (previous_inside != current_inside) {
                const s32 denominator = input_depth[current] - input_depth[previous];
                const s32 t = fast_signed_divide(
                    ((s32)SECTOR_NEAR - input_depth[previous]) << INTERP_SHIFT,
                    denominator);
                clipped_lat[clipped_count] = input_lat[previous] +
                    (((input_lat[current] - input_lat[previous]) * t) >> INTERP_SHIFT);
                clipped_depth[clipped_count++] = SECTOR_NEAR;
            }
            if (current_inside && clipped_count < MAX_PLANE_VERTICES) {
                clipped_lat[clipped_count] = input_lat[current];
                clipped_depth[clipped_count++] = input_depth[current];
            }
            previous = current;
        }
        if (clipped_count < 3) return;

        for (u16 i = 0; i < clipped_count; i++) {
            screen_x[i] = clamp_screen_x(RAY_VIEW_CENTER_X +
                fast_signed_divide(clipped_lat[i] * RAY_PROJ_X, clipped_depth[i]));
            screen_y[i] = project_world_z(state->floor_height,
                                          context->player->view_z,
                                          clipped_depth[i]);
        }
        for (u16 y = RAY_VIEW_CENTER_Y; y < RAY_VIEW_ROWS; y++) {
            g_plane_left[y] = 32767;
            g_plane_right[y] = -32768;
        }
        previous = (u16)(clipped_count - 1);
        for (u16 current = 0; current < clipped_count; current++) {
            add_plane_edge(screen_x[previous], screen_y[previous],
                           screen_x[current], screen_y[current]);
            previous = current;
        }

        for (u16 y = RAY_VIEW_CENTER_Y; y < RAY_VIEW_ROWS; y++) {
            s16 left = g_plane_left[y];
            s16 right = g_plane_right[y];
            if (left > right || right < 0 || left >= RAY_VIEW_COLS) continue;
            if (left < 0) left = 0;
            if (right >= RAY_VIEW_COLS) right = RAY_VIEW_COLS - 1;
            {
                const u16 first = (u16)((left + RAY_COL_STRIDE - 1) / RAY_COL_STRIDE);
                const u16 last = (u16)(right / RAY_COL_STRIDE);
                for (u16 sx = first; sx <= last; sx++) {
                    if (y > g_ceiling_clip[sx] && y < g_floor_clip[sx]) {
                        const u16 tile_y = (u16)(y >> 3);
                        const u16 row = (u16)(y & 7);
                        u16 *depth_rows = ensure_depth_block(sx, tile_y);
                        if (depth_rows[row] == SCENE_FAR) {
                            u16 *color_rows = base_sample_rows(sx, tile_y);
                            color_rows[row * 2] = SECTOR_REP4[color];
                            depth_rows[row] = PLANE_DEPTH;
                        }
                    }
                }
            }
        }
    }
}

static void transform_vertex(const SectorRenderContext *context, u16 vertex_id,
                             s32 *depth, s32 *lateral) {
    const BspVertex *vertex = &bsp_vertices[vertex_id];
    const u16 cache_slot = (u16)(vertex_id & (SECTOR_VERTEX_CACHE_CAP - 1));

    if (vertex_id < bsp_vertex_count &&
        g_vertex_generation[cache_slot] == g_transform_generation &&
        g_vertex_id[cache_slot] == vertex_id) {
        *depth = g_vertex_depth[cache_slot];
        *lateral = g_vertex_lateral[cache_slot];
        return;
    }

    {
        const s32 relx = (s32)vertex->x - context->player->x;
        const s32 rely = (s32)vertex->y - context->player->y;
        *depth = (relx * context->fwx + rely * context->fwy) >> FX_SHIFT;
        *lateral = (relx * context->rx + rely * context->ry) >> FX_SHIFT;
    }

    if (vertex_id < bsp_vertex_count) {
        g_vertex_depth[cache_slot] = *depth;
        g_vertex_lateral[cache_slot] = *lateral;
        g_vertex_id[cache_slot] = vertex_id;
        g_vertex_generation[cache_slot] = g_transform_generation;
    }
}

static void transform_render_seg(const SectorRenderContext *context,
                                 const BspRenderSeg *seg,
                                 RenderSegView *view) {
    transform_vertex(context, seg->v1, &view->da, &view->la);
    transform_vertex(context, seg->v2, &view->db, &view->lb);
}

static void draw_render_seg(const PlayerState *player, const BspRenderSeg *seg,
                            const RenderSegView *view) {
    const BspVertex *a = &bsp_vertices[seg->v1];
    const BspVertex *b = &bsp_vertices[seg->v2];
    s32 da = view->da;
    s32 la = view->la;
    s32 db = view->db;
    s32 lb = view->lb;
    s32 ua;
    s32 ub;

    if (((player->x - a->x) * seg->nx + (player->y - a->y) * seg->ny) <= 0) return;

    ua = seg->tex_u_offset;
    ub = ua + abs32((s32)b->x - a->x) + abs32((s32)b->y - a->y);

    if (da < SECTOR_NEAR && db < SECTOR_NEAR) return;
    if (da < SECTOR_NEAR) {
        const s32 t = fast_signed_divide(
            ((s32)SECTOR_NEAR - da) << INTERP_SHIFT, db - da);
        la += ((lb - la) * t) >> INTERP_SHIFT;
        ua += ((ub - ua) * t) >> INTERP_SHIFT;
        da = SECTOR_NEAR;
    } else if (db < SECTOR_NEAR) {
        const s32 t = fast_signed_divide(
            ((s32)SECTOR_NEAR - db) << INTERP_SHIFT, da - db);
        lb += ((la - lb) * t) >> INTERP_SHIFT;
        ub += ((ua - ub) * t) >> INTERP_SHIFT;
        db = SECTOR_NEAR;
    }

#if DEBUG_PERF
    const u32 setup_start = getSubTick();
#endif
    {
        s32 xa = RAY_VIEW_CENTER_X + fast_signed_divide(la * RAY_PROJ_X, da);
        s32 xb = RAY_VIEW_CENTER_X + fast_signed_divide(lb * RAY_PROJ_X, db);
        if (xa == xb) return;
        if (xa > xb) {
            s32 temp = xa; xa = xb; xb = temp;
            temp = da; da = db; db = temp;
            temp = ua; ua = ub; ub = temp;
        }

        {
            const s32 left = xa < 0 ? 0 : xa;
            const s32 right = xb >= RAY_VIEW_COLS ? RAY_VIEW_COLS - 1 : xb - 1;
            const BspSectorState *front;
            const BspSectorState *back;
            s32 span;
            u16 first_sample;
            u16 last_sample;
            s32 first_x;
            s16 fca, fcb, ffa, ffb, bca, bcb, bfa, bfb;
            s32 depth_q, u_q, fc_q, ff_q, bc_q, bf_q;
            s32 depth_step, u_step, fc_step, ff_step, bc_step, bf_step;

            if (left > right) return;

            front = bsp_get_sector_state(seg->front_sector);
            back = bsp_get_sector_state(seg->back_sector);
            if (!front) return;
            span = xb - xa;
            first_sample = (u16)((left + RAY_COL_STRIDE - 1) / RAY_COL_STRIDE);
            last_sample = (u16)(right / RAY_COL_STRIDE);
            first_x = (s32)first_sample * RAY_COL_STRIDE;
            fca = project_world_z(front->ceiling_height, player->view_z, da);
            fcb = project_world_z(front->ceiling_height, player->view_z, db);
            ffa = project_world_z(front->floor_height, player->view_z, da);
            ffb = project_world_z(front->floor_height, player->view_z, db);
            bca = back ? ((back->ceiling_height == front->ceiling_height) ? fca :
                project_world_z(back->ceiling_height, player->view_z, da)) : 0;
            bcb = back ? ((back->ceiling_height == front->ceiling_height) ? fcb :
                project_world_z(back->ceiling_height, player->view_z, db)) : 0;
            bfa = back ? ((back->floor_height == front->floor_height) ? ffa :
                project_world_z(back->floor_height, player->view_z, da)) : 0;
            bfb = back ? ((back->floor_height == front->floor_height) ? ffb :
                project_world_z(back->floor_height, player->view_z, db)) : 0;

            depth_q = interpolate_at(da, db, xa, span, first_x);
            u_q = interpolate_at(ua, ub, xa, span, first_x);
            fc_q = interpolate_at(fca, fcb, xa, span, first_x);
            ff_q = interpolate_at(ffa, ffb, xa, span, first_x);
            bc_q = interpolate_at(bca, bcb, xa, span, first_x);
            bf_q = interpolate_at(bfa, bfb, xa, span, first_x);
            depth_step = interpolation_step(da, db, span);
            u_step = interpolation_step(ua, ub, span);
            fc_step = interpolation_step(fca, fcb, span);
            ff_step = interpolation_step(ffa, ffb, span);
            bc_step = interpolation_step(bca, bcb, span);
            bf_step = interpolation_step(bfa, bfb, span);

#if DEBUG_PERF
            const u32 raster_start = getSubTick();
            g_debug_setup_subticks += raster_start - setup_start;
#endif
            for (u16 sx = first_sample; sx <= last_sample; sx++) {
                if (!g_column_closed[sx]) {
                    s32 depth = depth_q >> INTERP_SHIFT;
                    const s16 fc = (s16)(fc_q >> INTERP_SHIFT);
                    const s16 ff = (s16)(ff_q >> INTERP_SHIFT);
                    u16 wall_depth;
                    const u8 tx = (u8)((u_q >> INTERP_SHIFT) & WALL_TEX_DIM_MASK);

                    if (depth < 1) depth = 1;
                    if (depth > (s32)SCENE_FAR) wall_depth = SCENE_FAR;
                    else wall_depth = (u16)depth;

                    if (!back) {
                        draw_span(sx, fc, (s16)(ff - 1), wall_depth,
                                  seg->middle_texture, tx);
                        close_column(sx);
                    } else {
                        const s16 bc = (s16)(bc_q >> INTERP_SHIFT);
                        const s16 bf = (s16)(bf_q >> INTERP_SHIFT);
                        const u16 surface_depth = wall_depth > 1 ? (u16)(wall_depth - 1) : 1;
                        if (back->ceiling_height < front->ceiling_height) {
                            draw_span(sx, fc, (s16)(bc - 1), surface_depth,
                                      seg->upper_texture, tx);
                            if (bc - 1 > g_ceiling_clip[sx]) {
                                g_ceiling_clip[sx] = (s16)(bc - 1);
                            }
                        }
                        if (back->floor_height > front->floor_height) {
                            if ((seg->side_flags & BSP_RENDER_FLAT_RISER) != 0) {
                                const u8 shade_color = FREEDOOM_WORLD_SHADE_MAP[
                                    bsp_sectors[seg->back_sector].floor_color & 15];
                                draw_solid_span(sx, bf, (s16)(ff - 1), surface_depth,
                                                shade_color);
                            } else {
                                draw_span(sx, bf, (s16)(ff - 1), surface_depth,
                                          seg->lower_texture, tx);
                            }
                            if (bf < g_floor_clip[sx]) g_floor_clip[sx] = bf;
                        }
                        if (g_ceiling_clip[sx] + 1 >= g_floor_clip[sx]) {
                            close_column(sx);
                        }
                    }
                }
                depth_q += depth_step;
                u_q += u_step;
                fc_q += fc_step;
                ff_q += ff_step;
                bc_q += bc_step;
                bf_q += bf_step;
            }
#if DEBUG_PERF
            g_debug_raster_subticks += getSubTick() - raster_start;
#endif
        }
    }
}

static void draw_render_subsector(u16 subsector_id, void *opaque) {
    SectorRenderContext *context = (SectorRenderContext *)opaque;
    const BspRenderSubsector *subsector = &bsp_render_subsectors[subsector_id];
    const u16 end = (u16)(subsector->first_seg + subsector->seg_count);

    if (g_visible_subsector_count < MAX_VISIBLE_SUBSECTORS) {
        g_visible_subsectors[g_visible_subsector_count++] = subsector_id;
    }

    if (subsector->seg_count <= MAX_SUBSECTOR_SEGS) {
        u16 order[MAX_SUBSECTOR_SEGS];
        s32 depths[MAX_SUBSECTOR_SEGS];
        RenderSegView views[MAX_SUBSECTOR_SEGS];
        u16 count = 0;
#if DEBUG_PERF
        const u32 transform_start = getSubTick();
#endif

        for (u16 i = subsector->first_seg; i < end; i++) {
            u16 insert;
            transform_render_seg(context, &bsp_render_segs[i], &views[count]);
            depths[count] = views[count].da < views[count].db ?
                            views[count].da : views[count].db;
            insert = count;
            while (insert > 0 && depths[order[insert - 1]] > depths[count]) {
                order[insert] = order[insert - 1];
                insert--;
            }
            order[insert] = count;
            count++;
        }
#if DEBUG_PERF
        g_debug_transform_subticks += getSubTick() - transform_start;
#endif

        for (u16 i = 0; i < count; i++) {
            const u16 local_index = order[i];
            draw_render_seg(context->player,
                            &bsp_render_segs[subsector->first_seg + local_index],
                            &views[local_index]);
        }
    } else {
        // Generated maps currently top out at eight segs per SSECTOR. Keep a
        // correctness fallback for hand-authored maps instead of overflowing
        // the small stack-local ordering buffers.
        for (u16 i = subsector->first_seg; i < end; i++) {
            RenderSegView view;
            transform_render_seg(context, &bsp_render_segs[i], &view);
            draw_render_seg(context->player, &bsp_render_segs[i], &view);
        }
    }
}

static bool sector_range_closed(u16 left_sample, u16 right_sample, void *context) {
    u16 word = (u16)(left_sample >> 5);
    const u16 last_word = (u16)(right_sample >> 5);
    (void)context;

    while (word <= last_word) {
        const u16 word_left = (u16)(word << 5);
        const u16 word_right = (u16)(word_left + 31);
        const u16 range_left = left_sample > word_left ? left_sample : word_left;
        const u16 range_right = right_sample < word_right ? right_sample : word_right;
        const u16 start_bit = (u16)(range_left & 31);
        const u16 end_bit = (u16)(range_right & 31);
        u32 mask = 0xFFFFFFFFu << start_bit;

        if (end_bit < 31) {
            mask &= (u32)((1u << (end_bit + 1)) - 1u);
        }
        if ((g_closed_words[word] & mask) != mask) {
            return FALSE;
        }
        word++;
    }

    return TRUE;
}

static bool sector_all_closed(void *context) {
    (void)context;
    return g_closed_count >= RAY_SAMPLE_COLS;
}

static void clear_scene_flat(const PlayerState *player) {
    const u16 sector_id = player->sector_id < bsp_sector_count ? player->sector_id : 0;
    const BspSector *sector = &bsp_sectors[sector_id];

    // Build the flat background directly in the packed view-tile layout. The
    // sector renderer writes the same left/right 4px sample halves as the old
    // colour-buffer packer, so no second full-frame conversion is needed.
    {
        const u16 ceiling_half = SECTOR_REP4[sector->ceiling_color & 15];
        const u16 floor_half = SECTOR_REP4[sector->floor_color & 15];
        const u32 ceiling_row = ((u32)ceiling_half << 16) | ceiling_half;
        const u32 floor_row = ((u32)floor_half << 16) | floor_half;
        const u16 horizon_tile_y = (u16)(RAY_VIEW_CENTER_Y >> 3);
        const u16 horizon_row = (u16)(RAY_VIEW_CENTER_Y & 7);
        const u16 ceiling_words = (u16)(horizon_tile_y * VIEW_TILE_W * 8);

        if (ceiling_words > 0) {
            memsetU32(&g_view_tiles[0][0], ceiling_row, ceiling_words);
        }
        if (horizon_row != 0) {
            for (u16 tile_x = 0; tile_x < VIEW_TILE_W; tile_x++) {
                u32 *tile = g_view_tiles[(horizon_tile_y * VIEW_TILE_W) + tile_x];
                memsetU32(tile, ceiling_row, horizon_row);
                memsetU32(tile + horizon_row, floor_row, (u16)(8 - horizon_row));
            }
        }
        {
            const u16 first_floor_tile = (u16)(horizon_tile_y + (horizon_row != 0));
            const u16 floor_words =
                (u16)((VIEW_TILE_H - first_floor_tile) * VIEW_TILE_W * 8);
            if (floor_words > 0) {
                memsetU32(&g_view_tiles[first_floor_tile * VIEW_TILE_W][0],
                          floor_row, floor_words);
            }
        }
    }

    g_depth_generation++;
    if (g_depth_generation == 0) {
        memset(g_depth_block_generation, 0, sizeof(g_depth_block_generation));
        g_depth_generation = 1;
    }
    g_transform_generation++;
    if (g_transform_generation == 0) {
        memset(g_vertex_generation, 0, sizeof(g_vertex_generation));
        g_transform_generation = 1;
    }

    memset(g_column_closed, 0, sizeof(g_column_closed));
    memset(g_closed_words, 0, sizeof(g_closed_words));
    for (u16 x = 0; x < RAY_SAMPLE_COLS; x++) {
        g_ceiling_clip[x] = -1;
        g_floor_clip[x] = RAY_VIEW_ROWS;
    }
    g_closed_count = 0;
    g_visible_subsector_count = 0;
}

void bsp_sector_cast_frame(const PlayerState *player) {
    SectorRenderContext context;
    const u16 sector_id = player->sector_id < bsp_sector_count ? player->sector_id : 0;
    const BspSectorState *base_state = bsp_get_sector_state(sector_id);
#if DEBUG_PERF
    u32 stage_start = getSubTick();
    g_debug_transform_subticks = 0;
    g_debug_setup_subticks = 0;
    g_debug_raster_subticks = 0;
#endif

    clear_scene_flat(player);
#if DEBUG_PERF
    g_debug_flat_subticks = getSubTick() - stage_start;
#endif
    context.player = player;
    context.fwx = fx_cos(player->angle);
    context.fwy = fx_sin(player->angle);
    context.rx = (s16)-context.fwy;
    context.ry = context.fwx;
    context.base_floor_height = base_state ? base_state->floor_height :
                                            bsp_sectors[sector_id].floor_height;
    context.base_floor_color = bsp_sectors[sector_id].floor_color & 15;

#if DEBUG_PERF
    stage_start = getSubTick();
#endif
    bsp_traverse_front_to_back(player, draw_render_subsector, sector_range_closed,
                               sector_all_closed, &context);
#if DEBUG_PERF
    g_debug_wall_subticks = getSubTick() - stage_start;
    stage_start = getSubTick();
#endif
    for (u16 i = 0; i < g_visible_subsector_count; i++) {
        draw_floor_subsector(&context, g_visible_subsectors[i]);
    }
#if DEBUG_PERF
    g_debug_floor_subticks = getSubTick() - stage_start;
#endif
}

#endif
