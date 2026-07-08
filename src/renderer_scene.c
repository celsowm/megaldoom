#include "renderer_internal.h"
#include "generated_assets.h"
#include "generated_billboard_assets.h"
#include "generated_hud_assets.h"

// Flat billboard-texture descriptor. Storing the pixels as a plain const u8* (a
// [rows][cols] array decays cleanly) lets one draw loop sample sprites of any size:
// the enemy is 24x48, every other billboard stays 16x16. Index as pixels[y*w + x].
typedef struct {
    const u8 *pixels;
    u8 w;
    u8 h;
} BillboardTex;

static BillboardTex get_billboard_texture(u8 visual_id, u8 frame) {
    switch (visual_id) {
        case BILLBOARD_VISUAL_DUMMY_DAMAGED:
        case BILLBOARD_VISUAL_DUMMY: {
            const u8 f = (frame < FREEDOOM_BILLBOARD_ENEMY_FRAME_COUNT) ? frame : 0;
            return (BillboardTex){(const u8 *)FREEDOOM_BILLBOARD_ENEMY_FRAMES[f],
                                  FREEDOOM_BILLBOARD_ENEMY_W, FREEDOOM_BILLBOARD_ENEMY_H};
        }
        case BILLBOARD_VISUAL_DECOR_DAMAGED:
        case BILLBOARD_VISUAL_DECOR:
            return (BillboardTex){(const u8 *)FREEDOOM_BILLBOARD_DECOR_TEXTURE, 16, 16};
        case BILLBOARD_VISUAL_KEY:
            return (BillboardTex){(const u8 *)FREEDOOM_BILLBOARD_KEY_TEXTURE, 16, 16};
        case BILLBOARD_VISUAL_BONUS:
        default:
            return (BillboardTex){(const u8 *)FREEDOOM_BILLBOARD_BONUS_TEXTURE, 16, 16};
    }
}

static u8 remap_damaged_billboard_texel(u8 texel) {
    if (texel == 0) {
        return 0;
    }

    return (texel & 1) ? 12 : 13;
}

// Damaged-hit flash for the enemy: shift toward red so a shot reads clearly. The
// healthy enemy is drawn with its real converted colors (no remap) so it looks
// like an actual Freedoom soldier instead of a flat yellow blob.
static u8 remap_damaged_dummy_texel(u8 texel) {
    if (texel == 0) {
        return 0;
    }

    return (texel & 1) ? 12 : 2;
}

// Per-colour remap that darkens each wall texel by one step. Index 10 (brown) must
// darken to 8 (dark brown), NOT 9 (blue) — that typo once turned every shaded brown
// wall sky-blue. This is the single darkening step; the fog LUTs below apply it 0..N
// times per column, so shading on the fly keeps a pre-baked [RAY_TEXTURE_COUNT]
// [WALL_TEX_DIM][WALL_TEX_DIM] copy (~9 KB) out of the MD's 64 KB work RAM.
static const u8 WALL_SHADE_MAP[16] = {0, 6, 2, 2, 3, 4, 5, 6, 8, 8, 8, 10, 12, 12, 14, 14};

// Distance fog: walls are darkened in discrete steps the farther they are, giving
// the scene depth instead of a flat full-bright look. Level 0 is the identity map
// (nearest); each higher level applies WALL_SHADE_MAP once more, reusing that same
// hand-verified darkening ramp (so colour 10 brown still darkens to 8, never to
// blue). The LUTs are built once in renderer_scene_init from WALL_SHADE_MAP, then
// selected per column by depth in build_column_strip. N/S ("shade") walls just add
// one extra level, replacing the old two-map branch.
#define SHADE_LEVELS 4
// depth (world units) >> FOG_SHIFT picks the base fog level. Tuned so mid-room
// walls sit around level 1-2 and distant walls saturate at the darkest level.
#define FOG_SHIFT 9
static u8 g_shade_luts[SHADE_LEVELS][16];

static void build_shade_luts(void) {
    for (u16 c = 0; c < 16; c++) {
        g_shade_luts[0][c] = (u8)c; // level 0 == identity (WALL_IDENT_MAP)
    }
    for (u16 level = 1; level < SHADE_LEVELS; level++) {
        for (u16 c = 0; c < 16; c++) {
            g_shade_luts[level][c] = WALL_SHADE_MAP[g_shade_luts[level - 1][c] & 0x0F];
        }
    }
}

// Wall texture lookup indexed by texture_id, replacing the per-pixel 8-way branch.
// Order matches the original sample_wall_texture chain (id 0 == default wall).
static const u8 (*const WALL_TEX[RAY_TEXTURE_COUNT])[WALL_TEX_DIM] = {
    FREEDOOM_WALL_TEXTURE,        // 0
    FREEDOOM_DOOR_TEXTURE,        // 1
    FREEDOOM_LOCKED_DOOR_TEXTURE, // 2
    FREEDOOM_SWITCH_TEXTURE,      // 3
    FREEDOOM_WALL_BROWN_TEXTURE,  // 4
    FREEDOOM_WALL_GRAY_TEXTURE,   // 5
    FREEDOOM_WALL_METAL_TEXTURE,  // 6
    FREEDOOM_WALL_BRICK_TEXTURE,  // 7
    FREEDOOM_WALL_TECH_TEXTURE,   // 8
};

// Pixel-replication table for the active stride (guarded so the unused one isn't
// compiled): REP4[c] == c*0x1111 spreads a colour across 4px (stride 4); REP2[c] ==
// c*0x11 spreads it across 2px (stride 2, four cast columns per 8px tile).
#if RAY_COL_STRIDE == 4
static const u32 REP4[16] = {
    0x0000, 0x1111, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666, 0x7777,
    0x8888, 0x9999, 0xAAAA, 0xBBBB, 0xCCCC, 0xDDDD, 0xEEEE, 0xFFFF,
};
#else /* RAY_COL_STRIDE == 2 */
static const u32 REP2[16] = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
};
#endif

// Precomposed weapon overlay: the weapon bitmap is static per variant (idle /
// fire), so instead of re-testing ~72x54 pixels every redraw we bake, once at
// init, one (dst, clear_mask, value) op per affected tile-row. Drawing then
// becomes a handful of hundred u32 read-modify-writes: dst = (dst & ~clear) | val.
// Result is byte-identical to the old per-pixel loop (each nibble is written by
// exactly one weapon pixel; transparent pixels leave clear/val bits at 0).
// Stored as a struct-of-arrays to keep .bss tiny (MD has only 64KB RAM shared by
// heap + stack): a u16 flat index into g_view_tiles[][] plus the packed weapon
// nibbles. clear_mask is NOT stored - it is derived at draw time (see below),
// which is exact because weapon texels are 4bpp palette indices 1..15, so every
// opaque pixel leaves a nonzero nibble in value.
//
// Theoretical max = affected tile span (10 x 8 tiles) * 8 rows; assets currently
// use 331 (idle) / 277 (fire). The builder guards against overflow.
#define WEAPON_MAX_ROW_OPS 384
static u16 g_weapon_dst[2][WEAPON_MAX_ROW_OPS];   // flat u32 index into g_view_tiles
static u32 g_weapon_value[2][WEAPON_MAX_ROW_OPS]; // weapon nibbles at their shifts
static u16 g_weapon_op_count[2];

static void build_weapon_overlay_ops(void) {
    const u16 bottom_y = FREEDOOM_WEAPON_DRAW_Y + FREEDOOM_WEAPON_DRAW_H;
    const u16 right_x = FREEDOOM_WEAPON_DRAW_X + FREEDOOM_WEAPON_DRAW_W;
    const u16 tx_start = (u16)(FREEDOOM_WEAPON_DRAW_X >> 3);
    const u16 tx_end = (u16)((right_x - 1) >> 3);

    for (u16 v = 0; v < 2; v++) {
        const u8 (*weapon)[FREEDOOM_WEAPON_W] = v ? FREEDOOM_WEAPON_FIRE : FREEDOOM_WEAPON_IDLE;
        u16 count = 0;

        for (u16 y = FREEDOOM_WEAPON_DRAW_Y; y < bottom_y; y++) {
            const u16 tile_y = (u16)(y >> 3);
            const u16 row_y = (u16)(y & 7);

            for (u16 tile_x = tx_start; tile_x <= tx_end; tile_x++) {
                const u16 x0 = (u16)(tile_x * 8);
                const u16 x_begin = (x0 > FREEDOOM_WEAPON_DRAW_X) ? x0 : (u16)FREEDOOM_WEAPON_DRAW_X;
                const u16 x_stop = ((x0 + 8) < right_x) ? (u16)(x0 + 8) : right_x;
                u32 clear_mask = 0;
                u32 value = 0;

                for (u16 x = x_begin; x < x_stop; x++) {
                    const u8 texel = weapon[y][x];
                    if (texel != 0) {
                        const u16 shift = (u16)((7 - (x & 7)) * 4);
                        clear_mask |= (u32)0x0F << shift;
                        value |= (u32)(texel & 0x0F) << shift;
                    }
                }

                if ((clear_mask != 0) && (count < WEAPON_MAX_ROW_OPS)) {
                    const u16 tile_index = (u16)((tile_y * VIEW_TILE_W) + tile_x);
                    g_weapon_dst[v][count] = (u16)((tile_index * 8u) + row_y);
                    g_weapon_value[v][count] = value;
                    count++;
                }
            }
        }

        g_weapon_op_count[v] = count;
    }
}

void renderer_scene_init(void) {
    build_weapon_overlay_ops();
    build_shade_luts();
}

// Build a full vertical color strip for one ray column, resolving the texture
// pointer and shade flag once (instead of per pixel). The wall is centered, so
// everything above it is sky (y < VIEW_PIXEL_H/2) and below it is floor.
static void build_column_strip(const RayColumn *column, u8 *strip) {
    const u16 wall_h = column->height;
    const u16 top = (u16)((VIEW_PIXEL_H - wall_h) / 2);
    const u16 bottom = (u16)(top + wall_h);
    const u8 tid = (u8)((column->texture_id < RAY_TEXTURE_COUNT) ? column->texture_id : 0);
    const u8 (*tex)[WALL_TEX_DIM] = WALL_TEX[tid];
    // Distance fog + side shading fold into one LUT selection per column: the fog
    // level grows with depth, and N/S ("shade") walls add one extra darkening step.
    // g_shade_luts[0] is the identity, so near front walls are unshaded; every level
    // maps 0 -> 0, preserving transparency, and the inner loop stays branch-free.
    u16 fog_level = (u16)(column->depth >> FOG_SHIFT) + (column->shade ? 1u : 0u);
    if (fog_level > (SHADE_LEVELS - 1)) {
        fog_level = SHADE_LEVELS - 1;
    }
    const u8 *shade_map = g_shade_luts[fog_level];
    const u8 *ty_table = g_wall_tex_y_by_height[wall_h];
    const u8 tex_x = (u8)(column->tex_x & WALL_TEX_DIM_MASK);
    u16 y = 0;

    for (; y < top; y++) {
        strip[y] = 9; // sky
    }

    for (; y < bottom; y++) {
        strip[y] = shade_map[tex[ty_table[y - top]][tex_x] & 0x0F];
    }

    for (; y < VIEW_PIXEL_H; y++) {
        strip[y] = 14; // floor
    }
}

#if (RAY_COL_STRIDE != 4) && (RAY_COL_STRIDE != 2)
#error "build_raycast_tilemap only implements the RAY_COL_STRIDE == 4 and == 2 packers"
#endif

static u8 remap_billboard_texel(u8 visual_id, u8 texel) {
    if (visual_id == BILLBOARD_VISUAL_DECOR_DAMAGED) {
        return remap_damaged_billboard_texel(texel);
    }
    if (visual_id == BILLBOARD_VISUAL_DUMMY_DAMAGED) {
        return remap_damaged_dummy_texel(texel);
    }
    // Healthy enemy (BILLBOARD_VISUAL_DUMMY) and all others: real colors, unchanged.
    return texel;
}

#if RAY_COL_STRIDE == 4
static void build_raycast_tilemap(const RayColumn *columns) {
    u8 strip_a[VIEW_PIXEL_H];
    u8 strip_b[VIEW_PIXEL_H];

    // Each 8px-wide tile column maps to two cast columns (px 0 and 4), each
    // replicated 4x. Build both strips once per tile column, then pack the 15
    // vertical tiles from them.
    for (u16 tile_x = 0; tile_x < VIEW_TILE_W; tile_x++) {
        const u16 base_col = (u16)(tile_x * 8);
        build_column_strip(&columns[base_col], strip_a);
        build_column_strip(&columns[base_col + 4], strip_b);

        for (u16 tile_y = 0; tile_y < VIEW_TILE_H; tile_y++) {
            const u16 tile_index = (u16)((tile_y * VIEW_TILE_W) + tile_x);
            u16 pixel_y = (u16)(tile_y * 8);

            for (u16 row = 0; row < 8; row++, pixel_y++) {
                g_view_tiles[tile_index][row] =
                    (REP4[strip_a[pixel_y]] << 16) | REP4[strip_b[pixel_y]];
            }
        }
    }
}
#else /* RAY_COL_STRIDE == 2 */
static void build_raycast_tilemap(const RayColumn *columns) {
    u8 strip_a[VIEW_PIXEL_H];
    u8 strip_b[VIEW_PIXEL_H];
    u8 strip_c[VIEW_PIXEL_H];
    u8 strip_d[VIEW_PIXEL_H];

    // Each 8px-wide tile column maps to four cast columns (px 0, 2, 4, 6), each
    // replicated 2x -> twice the horizontal detail of the stride-4 packer at the
    // same tile count / DMA cost. Pack MSB-first: px0,1 in the top nibbles.
    for (u16 tile_x = 0; tile_x < VIEW_TILE_W; tile_x++) {
        const u16 base_col = (u16)(tile_x * 8);
        build_column_strip(&columns[base_col], strip_a);
        build_column_strip(&columns[base_col + 2], strip_b);
        build_column_strip(&columns[base_col + 4], strip_c);
        build_column_strip(&columns[base_col + 6], strip_d);

        for (u16 tile_y = 0; tile_y < VIEW_TILE_H; tile_y++) {
            const u16 tile_index = (u16)((tile_y * VIEW_TILE_W) + tile_x);
            u16 pixel_y = (u16)(tile_y * 8);

            for (u16 row = 0; row < 8; row++, pixel_y++) {
                g_view_tiles[tile_index][row] =
                    (REP2[strip_a[pixel_y]] << 24) | (REP2[strip_b[pixel_y]] << 16) |
                    (REP2[strip_c[pixel_y]] << 8) | REP2[strip_d[pixel_y]];
            }
        }
    }
}
#endif

// Draw the one-column billboard spans on top of the packed wall tiles. This is
// O(visible billboard pixels): cheap for scattered sprites (turning) yet bounded
// for a fullscreen enemy. Per span the tile_x and nibble shift are constant, and
// we walk tile rows so the tile-row base address is computed once per 8 rows
// (one 68000 multiply per tile, not per pixel — that per-pixel multiply was what
// made the old set_view_column_color path spike).
static void draw_billboard_spans(const RayColumn *columns, const BillboardSpan *spans, u16 span_count) {
    for (u16 i = 0; i < span_count; i++) {
        const BillboardSpan *span = &spans[i];

        if ((span->column < 0) || (span->column >= RAY_VIEW_COLS)) {
            continue;
        }
        if (span->depth >= columns[span->column].depth) {
            continue;
        }

        const s16 height = (s16)(span->bottom - span->top + 1);
        const BillboardTex tex = get_billboard_texture(span->visual_id, span->frame);
        // span->tex_x is a 0-255 normalized horizontal fraction (projection is
        // sprite-size-agnostic); scale it back to this sprite's real width here.
        const u8 tex_x = (u8)(((u16)span->tex_x * tex.w) >> 8);
        // All billboards use the exact Bresenham DDA below (floor(rel_y*tex.h/height),
        // advanced by add/compare per pixel, no per-pixel divide). The shared wall
        // sampling table now bakes the 32-texel wall height, so it can no longer be
        // borrowed for the 16-tall sprites; the DDA is byte-identical to what the
        // 16-tall table path produced.

        // Collapse the per-pixel colour remap into a 16-entry LUT resolved once per
        // span, so the inner loop does one table lookup instead of a branch. Every
        // remap maps 0 -> 0, so lut[0] == 0 preserves transparency.
        u8 lut[16];
        for (u16 c = 0; c < 16; c++) {
            lut[c] = remap_billboard_texel(span->visual_id, (u8)c);
        }

        const u16 col = (u16)span->column;
        const u16 tile_x = (u16)(col >> 3);
        const u16 shift = (u16)((7 - (col & 7)) * 4);
        const u32 keep_mask = ~((u32)0x0F << shift);

        s16 y0 = span->top;
        s16 y1 = span->bottom;
        if (y0 < 0) {
            y0 = 0;
        }
        if (y1 >= VIEW_PIXEL_H) {
            y1 = VIEW_PIXEL_H - 1;
        }

        // DDA state: exact floor(rel_y * tex.h / height), advanced by add/compare per
        // pixel (no divide). Seeded from the first visible row (rel_y_start) when the
        // span is clipped at the top.
        const s16 rel_y_start = (s16)((s16)y0 - span->top);
        u16 tex_y;
        u16 err;
        {
            const s32 num = (s32)rel_y_start * tex.h;
            tex_y = (u16)(num / height);
            err = (u16)(num % height);
        }

        u16 y = (u16)y0;
        const u16 y_end = (u16)(y1 + 1);
        while (y < y_end) {
            const u16 tile_y = (u16)(y >> 3);
            u32 *tile = g_view_tiles[(tile_y * VIEW_TILE_W) + tile_x]; // multiply once per tile
            const u16 next_tile_y = (u16)((tile_y + 1) * 8);
            const u16 stop = (next_tile_y < y_end) ? next_tile_y : y_end;

            for (; y < stop; y++) {
                const u16 ty = tex_y;
                err = (u16)(err + tex.h);
                while (err >= (u16)height) {
                    err = (u16)(err - (u16)height);
                    tex_y++;
                }

                const u8 texel = lut[tex.pixels[(ty * tex.w) + tex_x] & 0x0F];

                if (texel != 0) {
                    const u16 row_y = (u16)(y & 7);
                    tile[row_y] = (tile[row_y] & keep_mask) | ((u32)texel << shift);
                }
            }
        }
    }
}

// Draw the static weapon overlay from its precomposed per-tile-row ops (built
// once in build_weapon_overlay_ops). Each op is a single u32 read-modify-write,
// so an idle frame costs a few hundred RMWs instead of ~72x54 per-pixel tests.
static void draw_weapon_overlay(bool flash) {
    const u16 v = flash ? 1 : 0;
    const u16 *dst_idx = g_weapon_dst[v];
    const u32 *values = g_weapon_value[v];
    const u16 count = g_weapon_op_count[v];
    u32 *base = &g_view_tiles[0][0];

    for (u16 i = 0; i < count; i++) {
        const u32 val = values[i];
        // Rebuild clear_mask: set every nibble of val that has any bit -> 0x0F.
        // t marks nonzero nibbles (low bit each); (t<<4)-t expands 0x1 -> 0xF per
        // nibble with no cross-nibble carry (0xF < 0x10). Exact for 4bpp texels.
        const u32 t = (val | (val >> 1) | (val >> 2) | (val >> 3)) & 0x11111111u;
        const u32 clear_mask = (t << 4) - t;
        u32 *dst = base + dst_idx[i];
        *dst = (*dst & ~clear_mask) | val;
    }
}

#define DAMAGE_BORDER_PX 8

static void draw_damage_overlay(void) {
    for (u16 x = 0; x < RAY_VIEW_COLS; x++) {
        for (u16 t = 0; t < 3; t++) {
            set_view_column_color(x, t, 2);
            set_view_column_color(x, (VIEW_PIXEL_H - 1 - t), 2);
        }
    }

    for (u16 y = 0; y < VIEW_PIXEL_H; y++) {
        for (u16 t = 0; t < DAMAGE_BORDER_PX; t++) {
            set_view_column_color(t, y, 2);
            set_view_column_color((RAY_VIEW_COLS - 1 - t), y, 2);
        }
    }
}

static void draw_low_health_overlay(void) {
    for (u16 x = 16; x <= 32; x++) {
        set_view_column_color(x, 1, 11);
        set_view_column_color(x, 2, 11);
        set_view_column_color(x, (VIEW_PIXEL_H - 3), 11);
        set_view_column_color(x, (VIEW_PIXEL_H - 2), 11);
    }

    for (u16 x = (RAY_VIEW_COLS - 33); x <= (RAY_VIEW_COLS - 17); x++) {
        set_view_column_color(x, 1, 11);
        set_view_column_color(x, 2, 11);
        set_view_column_color(x, (VIEW_PIXEL_H - 3), 11);
        set_view_column_color(x, (VIEW_PIXEL_H - 2), 11);
    }
}

static void upload_view_tilemap(void) {
    // The tilemap itself is static and was uploaded once at init; only the tile
    // pixel data changes per frame, so just DMA the tiles.
    VDP_loadTileData((const u32 *)g_view_tiles, VIEW_TILE_BASE, VIEW_TILE_COUNT, DMA);
}

static void clear_compass_tilemap(void) {
    for (u16 i = 0; i < (COMPASS_W * COMPASS_H); i++) {
        g_compass_tilemap[i] = TILE_ATTR_FULL(PAL0, FALSE, FALSE, FALSE, PAIR_TILE_BASE);
    }
}

static void set_compass_tile(s16 x, s16 y, u8 color) {
    if ((x < 0) || (y < 0) || (x >= COMPASS_W) || (y >= COMPASS_H)) {
        return;
    }

    g_compass_tilemap[(y * COMPASS_W) + x] =
        TILE_ATTR_FULL(PAL0, FALSE, FALSE, FALSE, PAIR_TILE_BASE + ((color & 0x0F) << 4) + (color & 0x0F));
}

static void build_compass_tilemap(u16 angle) {
    const s16 vx = fx_cos(angle);
    const s16 vy = fx_sin(angle);
    const s16 dot_x = (s16)(2 + ((vx * 2) >> FX_SHIFT));
    const s16 dot_y = (s16)(2 + ((vy * 2) >> FX_SHIFT));

    clear_compass_tilemap();
    set_compass_tile(2, 0, 7);
    set_compass_tile(0, 2, 7);
    set_compass_tile(2, 2, 11);
    set_compass_tile(4, 2, 7);
    set_compass_tile(2, 4, 7);
    set_compass_tile(dot_x, dot_y, 13);
}

static void upload_compass_tilemap(void) {
    VDP_setTileMapDataRect(BG_B,
                           g_compass_tilemap,
                           COMPASS_X,
                           COMPASS_Y,
                           COMPASS_W,
                           COMPASS_H,
                           COMPASS_W,
                           CPU);
}

void renderer_render_scene(const RayColumn *columns,
                           const PlayerState *player,
                           bool weapon_flash,
                           bool damage_flash,
                           bool low_health_warning) {
    BillboardSpan spans[BILLBOARD_MAX_SPANS];
    const u16 span_count = billboard_project_scene(player, spans, BILLBOARD_MAX_SPANS);

    build_raycast_tilemap(columns);
    draw_billboard_spans(columns, spans, span_count);
    draw_weapon_overlay(weapon_flash);
    if (damage_flash) {
        draw_damage_overlay();
    } else if (low_health_warning) {
        draw_low_health_overlay();
    }
    build_compass_tilemap(player->angle);
}

// Push the frame built by renderer_render_scene to VRAM. Call this right after a
// VDP_waitVSync so the ~9.6KB view-tile DMA runs at the fast vblank rate instead
// of stalling the CPU mid active-display (its old call site).
void renderer_upload_scene(void) {
    upload_view_tilemap();
    upload_compass_tilemap();
}
