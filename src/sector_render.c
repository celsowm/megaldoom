#include <string.h>
#include "bsp_render.h"
#include "bsp_map.h"
#include "bsp_traverse.h"
#include "fixed_math.h"
#include "generated_assets.h"

#if BSP_SECTOR_RENDERER

#define SECTOR_NEAR 16
#define SCENE_FAR 0xFFFFu
#define PLANE_DEPTH 0xFFFEu
#define INTERP_SHIFT 8
#define MAX_PLANE_VERTICES 10
#define MAX_VISIBLE_SUBSECTORS 256
#define MAX_SUBSECTOR_SEGS 16
#define SECTOR_CLOSED_WORD_COUNT ((RAY_SAMPLE_COLS + 31) / 32)

static u8 g_scene_color[RAY_SAMPLE_COLS][RAY_VIEW_ROWS];
static u16 g_scene_depth[RAY_SAMPLE_COLS][RAY_VIEW_ROWS];
static s16 g_ceiling_clip[RAY_SAMPLE_COLS];
static s16 g_floor_clip[RAY_SAMPLE_COLS];
static bool g_column_closed[RAY_SAMPLE_COLS];
static u32 g_closed_words[SECTOR_CLOSED_WORD_COUNT];
static u16 g_closed_count;
static u16 g_visible_subsectors[MAX_VISIBLE_SUBSECTORS];
static u16 g_visible_subsector_count;
static s16 g_plane_left[RAY_VIEW_ROWS];
static s16 g_plane_right[RAY_VIEW_ROWS];

typedef struct {
    const PlayerState *player;
    s16 fwx;
    s16 fwy;
    s16 rx;
    s16 ry;
} SectorRenderContext;

const u8 *bsp_sector_scene_color(void) { return &g_scene_color[0][0]; }
const u16 *bsp_sector_scene_depth(void) { return &g_scene_depth[0][0]; }
u16 bsp_sector_depth_at(u16 x, u16 y) {
    return (x < RAY_SAMPLE_COLS && y < RAY_VIEW_ROWS) ? g_scene_depth[x][y] : SCENE_FAR;
}

// The 68000 has native DIVS.W (32-bit dividend / 16-bit divisor). C expressions
// with an s32 divisor otherwise call the much slower compiler 32/32 helper.
// Use the native instruction whenever both divisor and quotient fit, retaining
// the exact C fallback for unusual coordinates.
static s32 fast_signed_divide(s32 numerator, s32 denominator) {
    if (denominator != 0 && denominator >= -32768L && denominator <= 32767L) {
        const s16 divisor = (s16)denominator;
        s32 min_numerator;
        s32 max_numerator;

        if (divisor > 0) {
            min_numerator = -((s32)divisor * 32768L);
            max_numerator = (s32)divisor * 32767L;
        } else {
            min_numerator = (s32)divisor * 32767L;
            max_numerator = -((s32)divisor * 32768L);
        }

        if (numerator >= min_numerator && numerator <= max_numerator) {
            return (s32)divs(numerator, divisor);
        }
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

    // Exact replacement for ((row * WALL_TEX_DIM) / height), but with no
    // division in the per-pixel loop. The error accumulator produces the same
    // floor() sequence as the old expression for every span height.
    const u16 height = (u16)(bottom - top + 1);
    const u8 *texture_pixels = &FREEDOOM_WALL_TEXTURES[texture][0][0];
    u16 tex_y = 0;
    u16 tex_error = 0;

    for (s16 y = top; y <= bottom; y++) {
        if (depth < g_scene_depth[sx][y]) {
            g_scene_depth[sx][y] = depth;
            g_scene_color[sx][y] =
                texture_pixels[((tex_y & WALL_TEX_DIM_MASK) * WALL_TEX_DIM) + tex_x] & 15;
        }

        tex_error = (u16)(tex_error + WALL_TEX_DIM);
        while (tex_error >= height) {
            tex_error = (u16)(tex_error - height);
            tex_y++;
        }
    }
}

static void draw_solid_span(u16 sx, s16 top, s16 bottom, u16 depth, u8 color) {
    const s16 clip_top = (s16)(g_ceiling_clip[sx] + 1);
    const s16 clip_bottom = (s16)(g_floor_clip[sx] - 1);
    if (top < clip_top) top = clip_top;
    if (bottom > clip_bottom) bottom = clip_bottom;
    if (top < 0) top = 0;
    if (bottom >= RAY_VIEW_ROWS) bottom = RAY_VIEW_ROWS - 1;
    for (s16 y = top; y <= bottom; y++) {
        if (depth < g_scene_depth[sx][y]) {
            g_scene_depth[sx][y] = depth;
            g_scene_color[sx][y] = color & 15;
        }
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

static void draw_floor_subsector(const SectorRenderContext *context,
                                 u16 subsector_id) {
    const BspRenderSubsector *subsector = &bsp_render_subsectors[subsector_id];
    if (subsector->seg_count < 3 || subsector->sector_id >= bsp_sector_count) return;
    const BspSectorState *state = bsp_get_sector_state(subsector->sector_id);
    if (!state || state->floor_height >= context->player->view_z) return;

    s32 input_lat[MAX_PLANE_VERTICES];
    s32 input_depth[MAX_PLANE_VERTICES];
    s32 clipped_lat[MAX_PLANE_VERTICES];
    s32 clipped_depth[MAX_PLANE_VERTICES];
    u16 input_count = subsector->seg_count;
    if (input_count > MAX_PLANE_VERTICES - 2) input_count = MAX_PLANE_VERTICES - 2;
    for (u16 i = 0; i < input_count; i++) {
        const BspRenderSeg *seg = &bsp_render_segs[subsector->first_seg + i];
        const BspVertex *vertex = &bsp_vertices[seg->v1];
        const s32 dx = (s32)vertex->x - context->player->x;
        const s32 dy = (s32)vertex->y - context->player->y;
        input_depth[i] = (dx * context->fwx + dy * context->fwy) >> FX_SHIFT;
        input_lat[i] = (dx * context->rx + dy * context->ry) >> FX_SHIFT;
    }

    u16 clipped_count = 0;
    u16 previous = (u16)(input_count - 1);
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

    s16 screen_x[MAX_PLANE_VERTICES];
    s16 screen_y[MAX_PLANE_VERTICES];
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

    const u8 color = bsp_sectors[subsector->sector_id].floor_color & 15;
    for (u16 y = RAY_VIEW_CENTER_Y; y < RAY_VIEW_ROWS; y++) {
        s16 left = g_plane_left[y];
        s16 right = g_plane_right[y];
        if (left > right || right < 0 || left >= RAY_VIEW_COLS) continue;
        if (left < 0) left = 0;
        if (right >= RAY_VIEW_COLS) right = RAY_VIEW_COLS - 1;
        const u16 first = (u16)((left + RAY_COL_STRIDE - 1) / RAY_COL_STRIDE);
        const u16 last = (u16)(right / RAY_COL_STRIDE);
        for (u16 sx = first; sx <= last; sx++) {
            if (y > g_ceiling_clip[sx] && y < g_floor_clip[sx] &&
                g_scene_depth[sx][y] == SCENE_FAR) {
                g_scene_color[sx][y] = color;
                g_scene_depth[sx][y] = PLANE_DEPTH;
            }
        }
    }
}

static void draw_render_seg(const PlayerState *player, const BspRenderSeg *seg,
                            s16 fwx, s16 fwy, s16 rx, s16 ry) {
    const BspVertex *a = &bsp_vertices[seg->v1];
    const BspVertex *b = &bsp_vertices[seg->v2];
    if (((player->x - a->x) * seg->nx + (player->y - a->y) * seg->ny) <= 0) return;

    s32 relx = (s32)a->x - player->x;
    s32 rely = (s32)a->y - player->y;
    s32 da = (relx * fwx + rely * fwy) >> FX_SHIFT;
    s32 la = (relx * rx + rely * ry) >> FX_SHIFT;
    relx = (s32)b->x - player->x;
    rely = (s32)b->y - player->y;
    s32 db = (relx * fwx + rely * fwy) >> FX_SHIFT;
    s32 lb = (relx * rx + rely * ry) >> FX_SHIFT;
    s32 ua = seg->tex_u_offset;
    s32 ub = ua + abs32((s32)b->x - a->x) + abs32((s32)b->y - a->y);

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

    s32 xa = RAY_VIEW_CENTER_X + fast_signed_divide(la * RAY_PROJ_X, da);
    s32 xb = RAY_VIEW_CENTER_X + fast_signed_divide(lb * RAY_PROJ_X, db);
    if (xa == xb) return;
    if (xa > xb) {
        s32 temp = xa; xa = xb; xb = temp;
        temp = da; da = db; db = temp;
        temp = ua; ua = ub; ub = temp;
    }
    const s32 left = xa < 0 ? 0 : xa;
    const s32 right = xb >= RAY_VIEW_COLS ? RAY_VIEW_COLS - 1 : xb - 1;
    if (left > right) return;

    const BspSectorState *front = bsp_get_sector_state(seg->front_sector);
    const BspSectorState *back = bsp_get_sector_state(seg->back_sector);
    if (!front) return;
    const s32 span = xb - xa;
    const u16 first_sample = (u16)((left + RAY_COL_STRIDE - 1) / RAY_COL_STRIDE);
    const u16 last_sample = (u16)(right / RAY_COL_STRIDE);
    const s32 first_x = (s32)first_sample * RAY_COL_STRIDE;
    const s16 fca = project_world_z(front->ceiling_height, player->view_z, da);
    const s16 fcb = project_world_z(front->ceiling_height, player->view_z, db);
    const s16 ffa = project_world_z(front->floor_height, player->view_z, da);
    const s16 ffb = project_world_z(front->floor_height, player->view_z, db);
    const s16 bca = back ? project_world_z(back->ceiling_height, player->view_z, da) : 0;
    const s16 bcb = back ? project_world_z(back->ceiling_height, player->view_z, db) : 0;
    const s16 bfa = back ? project_world_z(back->floor_height, player->view_z, da) : 0;
    const s16 bfb = back ? project_world_z(back->floor_height, player->view_z, db) : 0;

    s32 depth_q = interpolate_at(da, db, xa, span, first_x);
    s32 u_q = interpolate_at(ua, ub, xa, span, first_x);
    s32 fc_q = interpolate_at(fca, fcb, xa, span, first_x);
    s32 ff_q = interpolate_at(ffa, ffb, xa, span, first_x);
    s32 bc_q = interpolate_at(bca, bcb, xa, span, first_x);
    s32 bf_q = interpolate_at(bfa, bfb, xa, span, first_x);
    const s32 depth_step = interpolation_step(da, db, span);
    const s32 u_step = interpolation_step(ua, ub, span);
    const s32 fc_step = interpolation_step(fca, fcb, span);
    const s32 ff_step = interpolation_step(ffa, ffb, span);
    const s32 bc_step = interpolation_step(bca, bcb, span);
    const s32 bf_step = interpolation_step(bfa, bfb, span);

    for (u16 sx = first_sample; sx <= last_sample; sx++) {
        if (!g_column_closed[sx]) {
            s32 depth = depth_q >> INTERP_SHIFT;
            const s16 fc = (s16)(fc_q >> INTERP_SHIFT);
            const s16 ff = (s16)(ff_q >> INTERP_SHIFT);
            if (depth < 1) depth = 1;
            u16 wall_depth;
            if (depth > (s32)SCENE_FAR) wall_depth = SCENE_FAR;
            else wall_depth = (u16)depth;
            const u8 tx = (u8)((u_q >> INTERP_SHIFT) & WALL_TEX_DIM_MASK);
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
                        const u8 color = FREEDOOM_WORLD_SHADE_MAP[
                            bsp_sectors[seg->back_sector].floor_color & 15];
                        draw_solid_span(sx, bf, (s16)(ff - 1), surface_depth,
                                        color);
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
}

static s32 render_seg_near_depth(const SectorRenderContext *context,
                                 const BspRenderSeg *seg) {
    const BspVertex *a = &bsp_vertices[seg->v1];
    const BspVertex *b = &bsp_vertices[seg->v2];
    const s32 adx = (s32)a->x - context->player->x;
    const s32 ady = (s32)a->y - context->player->y;
    const s32 bdx = (s32)b->x - context->player->x;
    const s32 bdy = (s32)b->y - context->player->y;
    const s32 da = (adx * context->fwx + ady * context->fwy) >> FX_SHIFT;
    const s32 db = (bdx * context->fwx + bdy * context->fwy) >> FX_SHIFT;
    return da < db ? da : db;
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
        u16 count = 0;
        for (u16 i = subsector->first_seg; i < end; i++) {
            const s32 depth = render_seg_near_depth(context, &bsp_render_segs[i]);
            u16 insert = count;
            while (insert > 0 && depths[insert - 1] > depth) {
                depths[insert] = depths[insert - 1];
                order[insert] = order[insert - 1];
                insert--;
            }
            depths[insert] = depth;
            order[insert] = i;
            count++;
        }
        for (u16 i = 0; i < count; i++) {
            draw_render_seg(context->player, &bsp_render_segs[order[i]],
                            context->fwx, context->fwy, context->rx, context->ry);
        }
    } else {
        // Generated maps currently top out at eight segs per SSECTOR. Keep a
        // correctness fallback for hand-authored maps instead of overflowing
        // the small stack-local ordering buffers.
        for (u16 i = subsector->first_seg; i < end; i++) {
            draw_render_seg(context->player, &bsp_render_segs[i], context->fwx,
                            context->fwy, context->rx, context->ry);
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

    // Scene depth is always SCENE_FAR at frame start, so a bulk fill replaces
    // 4,800 scalar u16 assignments. Build one flat colour column and replicate
    // it across the sampled view with the library's optimized memcpy.
    memset(g_scene_depth, 0xFF, sizeof(g_scene_depth));
    memset(g_scene_color[0], sector->ceiling_color, RAY_VIEW_CENTER_Y);
    memset(&g_scene_color[0][RAY_VIEW_CENTER_Y], sector->floor_color,
           RAY_VIEW_ROWS - RAY_VIEW_CENTER_Y);
    for (u16 x = 1; x < RAY_SAMPLE_COLS; x++) {
        memcpy(g_scene_color[x], g_scene_color[0], RAY_VIEW_ROWS);
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
    clear_scene_flat(player);
    context.player = player;
    context.fwx = fx_cos(player->angle);
    context.fwy = fx_sin(player->angle);
    context.rx = (s16)-context.fwy;
    context.ry = context.fwx;
    bsp_traverse_front_to_back(player, draw_render_subsector, sector_range_closed,
                               sector_all_closed, &context);
    for (u16 i = 0; i < g_visible_subsector_count; i++) {
        draw_floor_subsector(&context, g_visible_subsectors[i]);
    }
}

#endif
