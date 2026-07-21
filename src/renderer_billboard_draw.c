#include "renderer_pack_internal.h"
#include "renderer_perf.h"
#include "generated_billboard_assets.h"

// Flat billboard-texture descriptor. Storing the pixels as a plain const u8* (a
// [rows][cols] array decays cleanly) lets one draw loop sample sprites of any size:
// world sprites and enemies are 24x48. Index as pixels[y*w + x].
typedef struct {
    const u8 *pixels;
    u8 w;
    u8 h;
} BillboardTex;

// Explicit ROM pointer tables avoid GCC's generic 32-bit frame*stride helper
// for non-power-of-two sprite sheets. Selection is one indexed pointer load.
static const u8 *const ENEMY_FRAME_PIXELS[FREEDOOM_BILLBOARD_ENEMY_FRAME_COUNT] = {
    (const u8 *)FREEDOOM_BILLBOARD_ENEMY_FRAMES[0], (const u8 *)FREEDOOM_BILLBOARD_ENEMY_FRAMES[1],
    (const u8 *)FREEDOOM_BILLBOARD_ENEMY_FRAMES[2], (const u8 *)FREEDOOM_BILLBOARD_ENEMY_FRAMES[3],
    (const u8 *)FREEDOOM_BILLBOARD_ENEMY_FRAMES[4], (const u8 *)FREEDOOM_BILLBOARD_ENEMY_FRAMES[5],
    (const u8 *)FREEDOOM_BILLBOARD_ENEMY_FRAMES[6], (const u8 *)FREEDOOM_BILLBOARD_ENEMY_FRAMES[7],
    (const u8 *)FREEDOOM_BILLBOARD_ENEMY_FRAMES[8], (const u8 *)FREEDOOM_BILLBOARD_ENEMY_FRAMES[9],
};
static const u8 *const BARREL_FRAME_PIXELS[FREEDOOM_BILLBOARD_BARREL_EXPLOSION_FRAME_COUNT] = {
    (const u8 *)FREEDOOM_BILLBOARD_BARREL_EXPLOSION_FRAMES[0],
    (const u8 *)FREEDOOM_BILLBOARD_BARREL_EXPLOSION_FRAMES[1],
    (const u8 *)FREEDOOM_BILLBOARD_BARREL_EXPLOSION_FRAMES[2],
    (const u8 *)FREEDOOM_BILLBOARD_BARREL_EXPLOSION_FRAMES[3],
    (const u8 *)FREEDOOM_BILLBOARD_BARREL_EXPLOSION_FRAMES[4],
};
static const u8 *const PUFF_FRAME_PIXELS[FREEDOOM_BILLBOARD_PUFF_FRAME_COUNT] = {
    (const u8 *)FREEDOOM_BILLBOARD_PUFF_FRAMES[0], (const u8 *)FREEDOOM_BILLBOARD_PUFF_FRAMES[1],
    (const u8 *)FREEDOOM_BILLBOARD_PUFF_FRAMES[2], (const u8 *)FREEDOOM_BILLBOARD_PUFF_FRAMES[3],
};
static const u8 *const BLOOD_FRAME_PIXELS[FREEDOOM_BILLBOARD_BLOOD_FRAME_COUNT] = {
    (const u8 *)FREEDOOM_BILLBOARD_BLOOD_FRAMES[0], (const u8 *)FREEDOOM_BILLBOARD_BLOOD_FRAMES[1],
    (const u8 *)FREEDOOM_BILLBOARD_BLOOD_FRAMES[2],
};

#define PUFF_WALL_DEPTH_TOLERANCE 64

static bool billboard_depth_visible(const ProjectedBillboard *object,
                                    u16 wall_depth) {
    if (object->depth < wall_depth) return TRUE;
    // PUFF represents a hit on this wall plane. Ray depths vary across the
    // four-pixel rendered block, so allow only this transient a small tolerance
    // around the plane instead of letting the block hide its own impact.
    return (bool)((object->visual_id == BILLBOARD_VISUAL_PUFF) &&
                  ((u16)(object->depth - wall_depth) <= PUFF_WALL_DEPTH_TOLERANCE));
}

static BillboardTex get_billboard_texture(u8 visual_id, u8 frame) {
    switch (visual_id) {
        case BILLBOARD_VISUAL_DUMMY_DAMAGED:
        case BILLBOARD_VISUAL_DUMMY: {
            const u8 f = (frame < FREEDOOM_BILLBOARD_ENEMY_FRAME_COUNT) ? frame : 0;
            return (BillboardTex){ENEMY_FRAME_PIXELS[f],
                                  FREEDOOM_BILLBOARD_ENEMY_W, FREEDOOM_BILLBOARD_ENEMY_H};
        }
        case BILLBOARD_VISUAL_BARREL_EXPLODING: {
            const u8 f = (frame < FREEDOOM_BILLBOARD_BARREL_EXPLOSION_FRAME_COUNT) ? frame : 0;
            return (BillboardTex){BARREL_FRAME_PIXELS[f],
                                  FREEDOOM_BILLBOARD_BARREL_EXPLOSION_W,
                                  FREEDOOM_BILLBOARD_BARREL_EXPLOSION_H};
        }
        case BILLBOARD_VISUAL_PUFF: {
            const u8 f = (frame < FREEDOOM_BILLBOARD_PUFF_FRAME_COUNT) ? frame : 0;
            return (BillboardTex){PUFF_FRAME_PIXELS[f],
                                  FREEDOOM_BILLBOARD_PUFF_W, FREEDOOM_BILLBOARD_PUFF_H};
        }
        case BILLBOARD_VISUAL_BLOOD: {
            const u8 f = (frame < FREEDOOM_BILLBOARD_BLOOD_FRAME_COUNT) ? frame : 0;
            return (BillboardTex){BLOOD_FRAME_PIXELS[f],
                                  FREEDOOM_BILLBOARD_BLOOD_W, FREEDOOM_BILLBOARD_BLOOD_H};
        }
        default:
            if (visual_id < FREEDOOM_BILLBOARD_WORLD_TEXTURE_COUNT) {
                return (BillboardTex){(const u8 *)FREEDOOM_BILLBOARD_WORLD_TEXTURES[visual_id],
                                      FREEDOOM_BILLBOARD_WORLD_W, FREEDOOM_BILLBOARD_WORLD_H};
            }
            return (BillboardTex){(const u8 *)FREEDOOM_BILLBOARD_WORLD_TEXTURES[BILLBOARD_VISUAL_BONUS],
                                  FREEDOOM_BILLBOARD_WORLD_W, FREEDOOM_BILLBOARD_WORLD_H};
    }
}

// Exact 32/16=32 unsigned division using two DIVU.W steps (base-65536 long
// division). Produces the same integer quotient as the '/' operator, so it can
// replace the slow compiler-emitted 32-bit unsigned-division helper for
// quotients that exceed 16 bits (e.g. Q16 texture stepping for tall sprites).
static u32 divu32_16_exact(u32 numerator, u16 denominator) {
    const u16 high = (u16)(numerator >> 16);

    // Quotient fits in 16 bits: one DIVU.W is exact.
    if (high < denominator) {
        return divu(numerator, denominator);
    }

    const u16 quotient_high = divu(high, denominator);
    const u16 remainder_high =
        (u16)(high - ((u32)quotient_high * denominator));

    const u32 low_numerator =
        ((u32)remainder_high << 16) | (numerator & 0xFFFFu);
    const u16 quotient_low = divu(low_numerator, denominator);

    return ((u32)quotient_high << 16) | quotient_low;
}

static u32 scene_mulu_word(u16 left, u16 right) {
    u32 result = left;
    __asm__ volatile (
        "mulu.w %1,%0"
        : "+d" (result)
        : "d" (right)
        : "cc");
    return result;
}

static u32 scene_mul_u32_u16(u32 left, u16 right) {
    return scene_mulu_word((u16)left, right) +
           (scene_mulu_word((u16)(left >> 16), right) << 16);
}

// Generated pickup posts are an exact ROM copy of each source column's opaque
// spans. Only sparse collectible textures take this path; dense sprites remain
// on the row/tile packer where an extra post lookup would cost more than the
// transparent texel test it replaces.
static inline bool pickup_post_contains(u8 visual_id, u8 tex_x, u8 tex_y) {
    const u16 begin = FREEDOOM_BILLBOARD_PICKUP_POST_OFFSETS[visual_id][tex_x];
    const u16 end = FREEDOOM_BILLBOARD_PICKUP_POST_OFFSETS[visual_id][tex_x + 1];

    for (u16 post = begin; post < end; post++) {
        const u8 top = FREEDOOM_BILLBOARD_PICKUP_POSTS[post][0];
        const u8 length = FREEDOOM_BILLBOARD_PICKUP_POSTS[post][1];
        if (tex_y < top) return FALSE;
        if ((u8)(tex_y - top) < length) return TRUE;
    }
    return FALSE;
}

// Draw projected billboards object-by-object in painter order. Texture and depth
// decisions remain pixel-exact, but pixels sharing one 8-pixel tile row are
// accumulated into a mask/value pair and committed with one RAM RMW. The old
// column-first path rewrote the same packed u32 once for every opaque pixel.
void draw_projected_billboards(const RayColumn *columns,
                               const ProjectedBillboard *objects,
                               u16 object_count) {
#if DEBUG_PERF
    const bool measure_pickup_posts = (bool)(
        renderer_perf_get_deep_phase() == RENDERER_PERF_DEEP_PICKUP_POSTS);
#endif
    for (u16 i = 0; i < object_count; i++) {
        const ProjectedBillboard *object = &objects[i];
        const s16 height = (s16)(object->bottom - object->top + 1);
        const s16 width = (s16)(object->right - object->left + 1);
        const BillboardTex tex = get_billboard_texture(object->visual_id, object->frame);
        const u8 *lut = MEGALDOOM_BILLBOARD_REMAP[
            (object->visual_id < 6) ? object->visual_id : BILLBOARD_VISUAL_BONUS];
        const bool use_pickup_posts = (bool)(
            object->visual_id < FREEDOOM_BILLBOARD_PICKUP_TEXTURE_COUNT &&
            FREEDOOM_BILLBOARD_PICKUP_USE_POSTS[object->visual_id]);
        // 0xFF marks a clipped/wall-hidden screen column. Generated atlas X
        // coordinates are far below 255, so the sentinel cannot alias a texel.
        u8 tex_x_by_screen_col[RAY_VIEW_COLS];
        s16 x0 = object->left;
        s16 x1 = object->right;
        s16 y0 = object->top;
        s16 y1 = object->bottom;
        u32 tex_x_step;
        const u8 atlas_x_last = (u8)(object->atlas_x + object->atlas_w - 1);
        u16 first_tile_x;
        u16 last_tile_x;
        bool has_door_overlay = FALSE;

        if ((height <= 0) || (width <= 0)) {
            continue;
        }
        // Sample only the occupied atlas rectangle. Transparent letterbox padding
        // must not alter the WAD patch's projected size or origin.
        tex_x_step = divu((u32)object->atlas_w << 8, (u16)width);

        if (x0 < 0) {
            x0 = 0;
        }
        if (x1 >= RAY_VIEW_COLS) {
            x1 = RAY_VIEW_COLS - 1;
        }
        if (y0 < 0) {
            y0 = 0;
        }
        if (y1 >= VIEW_PIXEL_H) {
            y1 = VIEW_PIXEL_H - 1;
        }
        if ((x0 > x1) || (y0 > y1)) {
            continue;
        }
#if DEBUG_PERF
        const u32 pickup_post_start = (measure_pickup_posts && use_pickup_posts) ?
                                          getSubTick() : 0;
#endif
        first_tile_x = (u16)(x0 >> 3);
        last_tile_x = (u16)(x1 >> 3);

        // Resolve horizontal texture coordinates and whole-column wall depth
        // once. Door slabs remain a per-pixel vertical test below.
        {
            u32 tex_x_acc = ((u32)object->atlas_x << 8) +
                scene_mulu_word((u16)(x0 - object->left), (u16)tex_x_step);
            for (s16 col = x0; col <= x1; col++) {
                u8 tex_x = (u8)(tex_x_acc >> 8);
                const u16 wall_col = (u16)(col & ~(RAY_COL_STRIDE - 1));
                tex_x_acc += tex_x_step;

                if (!billboard_depth_visible(object, columns[wall_col].depth)) {
                    tex_x_by_screen_col[col] = 0xFF;
                    continue;
                }
                if (tex_x > atlas_x_last) tex_x = atlas_x_last;
                tex_x_by_screen_col[col] = tex_x;
            }
        }

        // Most views have no moving door slab across a sprite. Detect that once
        // per object so the hot pixel loop can short-circuit the full vertical
        // door/depth test instead of repeating it for every opaque texel.
        for (u16 wall_col = (u16)(x0 & ~(RAY_COL_STRIDE - 1));
             wall_col <= (u16)x1;
             wall_col = (u16)(wall_col + RAY_COL_STRIDE)) {
            const RayDoorOverlay *door = &columns[wall_col].door;
            if (door->height != 0 && door->depth < columns[wall_col].depth &&
                object->depth >= door->depth) {
                has_door_overlay = TRUE;
                break;
            }
        }

        // Approximate floor(rel_y * tex.h / height) with one setup divide and a
        // fixed-point DDA instead of a 68k divide for every visible sprite row.
        // tex_y_step = (crop.h<<16)/height is Q16; for a 48px enemy the numerator
        // is 3,145,728 and small projected heights make the quotient exceed 16
        // bits, so a single DIVU.W cannot hold it. The two-stage divider produces
        // the exact same quotient as '/'.
        const u32 tex_y_step = divu32_16_exact((u32)object->atlas_h << 16,
                                               (u16)height);
        u32 tex_y_acc = ((u32)object->atlas_y << 16) +
                        scene_mul_u32_u16(tex_y_step,
                                         (u16)(y0 - object->top));
        const u8 atlas_y_last = (u8)(object->atlas_y + object->atlas_h - 1);
        for (s16 y = y0; y <= y1; y++) {
            u8 tex_y = (u8)(tex_y_acc >> 16);
            const u16 tile_y = (u16)(y >> 3);
            const u16 row_y = (u16)(y & 7);
            if (tex_y > atlas_y_last) tex_y = atlas_y_last;
            tex_y_acc += tex_y_step;
            const u8 *tex_row = &tex.pixels[scene_mulu_word(tex_y, tex.w)];

            for (u16 tile_x = first_tile_x; tile_x <= last_tile_x; tile_x++) {
                const s16 tile_left = (s16)(tile_x * 8);
                const s16 tile_right = (s16)(tile_left + 7);
                const s16 col_begin = (x0 > tile_left) ? x0 : tile_left;
                const s16 col_end = (x1 < tile_right) ? x1 : tile_right;
                u32 clear_mask = 0;
                u32 value = 0;

                // Raster one wall sample (two screen pixels) per iteration.
                // Both authored texels are still sampled independently, so the
                // packed framebuffer is byte-identical to the scalar reference.
                for (s16 pair_col = (s16)(col_begin & ~(RAY_COL_STRIDE - 1));
                     pair_col <= col_end; pair_col += RAY_COL_STRIDE) {
                    const u16 wall_col = (u16)pair_col;
                    u32 pair_mask = 0;
                    u32 pair_value = 0;

                    for (u16 pixel = 0; pixel < RAY_COL_STRIDE; pixel++) {
                        const s16 col = (s16)(pair_col + pixel);
                        if (col < col_begin || col > col_end) continue;
                        const u8 tex_x = tex_x_by_screen_col[col];
                        if (tex_x == 0xFF) continue;
                        if (use_pickup_posts &&
                            !pickup_post_contains(object->visual_id, tex_x, tex_y)) {
                            continue;
                        }
                        const u8 texel = lut[tex_row[tex_x] & 0x0F];
                        if (texel != 0) {
                            const u16 shift = (u16)((7 - (col & 7)) * 4);
                            pair_mask |= (u32)0x0F << shift;
                            pair_value |= (u32)texel << shift;
                        }
                    }
                    if (pair_mask != 0 && (!has_door_overlay ||
                            !door_overlay_blocks_pixel(&columns[wall_col],
                                                       object->depth, (u16)y))) {
                        clear_mask |= pair_mask;
                        value |= pair_value;
                    }
                }

                if (clear_mask != 0) {
                    const u16 tile_index = view_tile_index(tile_x, tile_y);
                    u32 *dst;
                    renderer_mark_overlay_tile(tile_index);
                    dst = &g_view_tiles[tile_index][row_y];
                    *dst = (*dst & ~clear_mask) | value;
                }
            }
        }
#if DEBUG_PERF
        if (measure_pickup_posts && use_pickup_posts) {
            renderer_perf_record_deep(RENDERER_PERF_DEEP_PICKUP_POSTS,
                                      getSubTick() - pickup_post_start, 1);
        }
#endif
    }
}
