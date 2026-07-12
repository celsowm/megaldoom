#include "bsp_render.h"
#include "bsp_map.h"
#include "fixed_math.h"
#include "generated_assets.h"

#if BSP_SECTOR_RENDERER

#define SECTOR_NEAR 16
#define SCENE_FAR 0xFFFFu

static u8 g_scene_color[RAY_SAMPLE_COLS][RAY_VIEW_ROWS];
static u16 g_scene_depth[RAY_SAMPLE_COLS][RAY_VIEW_ROWS];
static s16 g_ceiling_clip[RAY_SAMPLE_COLS];
static s16 g_floor_clip[RAY_SAMPLE_COLS];

const u8 *bsp_sector_scene_color(void) { return &g_scene_color[0][0]; }
const u16 *bsp_sector_scene_depth(void) { return &g_scene_depth[0][0]; }
u16 bsp_sector_depth_at(u16 x, u16 y) {
    return (x < RAY_SAMPLE_COLS && y < RAY_VIEW_ROWS) ? g_scene_depth[x][y] : SCENE_FAR;
}

static s16 project_world_z(s16 world_z, s16 view_z, s32 depth) {
    if (depth < 1) depth = 1;
    return (s16)(RAY_VIEW_CENTER_Y - (((s32)(world_z - view_z) * RAY_PROJ_Y) / depth));
}

static void put_pixel(u16 sx, s16 y, u16 depth, u8 color) {
    if (y < 0 || y >= RAY_VIEW_ROWS || sx >= RAY_SAMPLE_COLS) return;
    if (depth < g_scene_depth[sx][y]) {
        g_scene_depth[sx][y] = depth;
        g_scene_color[sx][y] = color & 15;
    }
}

static void draw_span(u16 sx, s16 top, s16 bottom, u16 depth, u8 texture,
                      u8 tex_x, s16 world_top, s16 world_bottom) {
    if (top < 0) top = 0;
    if (bottom >= RAY_VIEW_ROWS) bottom = RAY_VIEW_ROWS - 1;
    if (top > bottom) return;
    if (texture == 0xFF) return;
    if (texture >= FREEDOOM_WALL_TEXTURE_COUNT) texture = MEGALDOOM_TEX_FALLBACK;
    const s16 height = (bottom > top) ? (bottom - top + 1) : 1;
    (void)world_top; (void)world_bottom;
    for (s16 y = top; y <= bottom; y++) {
        const u8 ty = (u8)(((s32)(y - top) * WALL_TEX_DIM / height) & WALL_TEX_DIM_MASK);
        put_pixel(sx, y, depth, FREEDOOM_WALL_TEXTURES[texture][ty][tex_x]);
    }
}

static void draw_render_seg(const PlayerState *player, const BspRenderSeg *seg,
                            s16 fwx, s16 fwy, s16 rx, s16 ry) {
    const BspVertex *a = &bsp_vertices[seg->v1];
    const BspVertex *b = &bsp_vertices[seg->v2];
    if (((player->x - a->x) * seg->nx + (player->y - a->y) * seg->ny) <= 0) return;

    s32 relx = (s32)a->x - player->x, rely = (s32)a->y - player->y;
    s32 da = (relx * fwx + rely * fwy) >> FX_SHIFT;
    s32 la = (relx * rx + rely * ry) >> FX_SHIFT;
    relx = (s32)b->x - player->x; rely = (s32)b->y - player->y;
    s32 db = (relx * fwx + rely * fwy) >> FX_SHIFT;
    s32 lb = (relx * rx + rely * ry) >> FX_SHIFT;
    if (da < SECTOR_NEAR || db < SECTOR_NEAR) return;

    s32 xa = RAY_VIEW_CENTER_X + (la * RAY_PROJ_X) / da;
    s32 xb = RAY_VIEW_CENTER_X + (lb * RAY_PROJ_X) / db;
    if (xa == xb) return;
    if (xa > xb) { s32 t=xa; xa=xb; xb=t; t=da; da=db; db=t; }
    s32 left = xa < 0 ? 0 : xa;
    s32 right = xb >= RAY_VIEW_COLS ? RAY_VIEW_COLS - 1 : xb - 1;
    if (left > right) return;

    const BspSectorState *front = bsp_get_sector_state(seg->front_sector);
    const BspSectorState *back = bsp_get_sector_state(seg->back_sector);
    if (!front) return;
    const s32 span = xb - xa;
    for (u16 sx = (u16)((left + RAY_COL_STRIDE - 1) / RAY_COL_STRIDE);
         sx <= (u16)(right / RAY_COL_STRIDE); sx++) {
        const s32 screen_x = sx * RAY_COL_STRIDE;
        const s32 frac = ((screen_x - xa) << 8) / span;
        s32 depth = da + (((db - da) * frac) >> 8);
        if (depth < 1) depth = 1;
        const s16 fc = project_world_z(front->ceiling_height, player->view_z, depth);
        const s16 ff = project_world_z(front->floor_height, player->view_z, depth);
        const u8 tx = (u8)(((seg->tex_u_offset + ((screen_x - xa) * WALL_TEX_DIM / span))) & WALL_TEX_DIM_MASK);
        if (!back) {
            draw_span(sx, fc, ff - 1, (u16)depth, seg->middle_texture, tx,
                      front->ceiling_height, front->floor_height);
            g_ceiling_clip[sx] = RAY_VIEW_ROWS - 1;
            g_floor_clip[sx] = 0;
            continue;
        }
        const s16 bc = project_world_z(back->ceiling_height, player->view_z, depth);
        const s16 bf = project_world_z(back->floor_height, player->view_z, depth);
        const u16 surface_depth = (u16)((depth > 1) ? depth - 1 : 1);
        if (back->ceiling_height != front->ceiling_height) {
            const s16 top = (fc < bc) ? fc : bc;
            const s16 bottom = (fc > bc) ? fc : bc;
            draw_span(sx, top, bottom - 1, surface_depth, seg->upper_texture, tx,
                      front->ceiling_height, back->ceiling_height);
        }
        if (back->floor_height != front->floor_height) {
            const s16 top = (ff < bf) ? ff : bf;
            const s16 bottom = (ff > bf) ? ff : bf;
            draw_span(sx, top, bottom - 1, surface_depth, seg->lower_texture, tx,
                      front->floor_height, back->floor_height);
        }
    }
}

static void render_planes(const PlayerState *player, s16 fwx, s16 fwy, s16 rx, s16 ry) {
    const BspSectorState *player_sector = bsp_get_sector_state(player->sector_id);
    const s16 player_floor = player_sector ? player_sector->floor_height : 0;
    for (u16 sx = 0; sx < RAY_SAMPLE_COLS; sx++) {
        const s32 lateral = (s32)(sx * RAY_COL_STRIDE) - RAY_VIEW_CENTER_X;
        for (u16 y = 0; y < RAY_VIEW_ROWS; y++) {
            if (y == RAY_VIEW_CENTER_Y) continue;
            u16 sector_id = player->sector_id;
            s32 depth = 0;
            for (u16 iteration = 0; iteration < 4; iteration++) {
                const BspSectorState *state = bsp_get_sector_state(sector_id);
                if (!state) break;
                if (y > RAY_VIEW_CENTER_Y) {
                    const s32 delta = player->view_z - state->floor_height;
                    if (delta <= 0) break;
                    depth = (delta * RAY_PROJ_Y) / (y - RAY_VIEW_CENTER_Y);
                } else {
                    const s32 delta = state->ceiling_height - player->view_z;
                    if (delta <= 0) break;
                    depth = (delta * RAY_PROJ_Y) / (RAY_VIEW_CENTER_Y - y);
                }
                if (depth < 1 || depth > 0xFFFE) break;
                const s32 side = (depth * lateral) / RAY_PROJ_X;
                const s32 wx = player->x + (((s32)fwx * depth + (s32)rx * side) >> FX_SHIFT);
                const s32 wy = player->y + (((s32)fwy * depth + (s32)ry * side) >> FX_SHIFT);
                const u16 resolved_sector = bsp_find_sector(wx, wy);
                if (resolved_sector == sector_id) break;
                sector_id = resolved_sector;
            }
            if (sector_id < bsp_sector_count && depth > 0 && depth < 0xFFFF) {
                g_scene_depth[sx][y] = (u16)depth;
                u8 color = (y > RAY_VIEW_CENTER_Y)
                    ? bsp_sectors[sector_id].floor_color
                    : bsp_sectors[sector_id].ceiling_color;
                if (y > RAY_VIEW_CENTER_Y) {
                    const BspSectorState *state = bsp_get_sector_state(sector_id);
                    s16 delta = state ? state->floor_height - player_floor : 0;
                    if (delta < 0) delta = (s16)-delta;
                    u16 shade_steps = (u16)(delta / 8);
                    if (shade_steps > 2) shade_steps = 2;
                    while (shade_steps-- > 0) color = FREEDOOM_WORLD_SHADE_MAP[color & 15];
                }
                g_scene_color[sx][y] = color;
            }
        }
    }
}

void bsp_sector_cast_frame(const PlayerState *player) {
    for (u16 x = 0; x < RAY_SAMPLE_COLS; x++) {
        g_ceiling_clip[x] = -1;
        g_floor_clip[x] = RAY_VIEW_ROWS;
        for (u16 y = 0; y < RAY_VIEW_ROWS; y++) {
            g_scene_depth[x][y] = SCENE_FAR;
            g_scene_color[x][y] = 0;
        }
    }
    const s16 fwx = fx_cos(player->angle), fwy = fx_sin(player->angle);
    const s16 rx = (s16)-fwy, ry = fwx;
    render_planes(player, fwx, fwy, rx, ry);
    for (u16 i = 0; i < bsp_render_seg_count; i++)
        draw_render_seg(player, &bsp_render_segs[i], fwx, fwy, rx, ry);
}

#endif
