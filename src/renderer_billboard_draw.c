#include "renderer_pack_internal.h"
#include "renderer_perf.h"
#include "generated_billboard_assets.h"
#include "debug_checkpoint.h"

#if CADENCE_STAGE_PROBE
#define BB_INC(name) (g_cadence_bb_##name++)
#else
#define BB_INC(name) ((void)0)
#endif

// Differential harness: build with -DBILLBOARD_RASTER_VERIFY=1 to run the
// pre-2026-08-03 reference rasterizer alongside the byte-wise one below and
// byte-compare the results, object by object. Off by default -- it costs a
// second full raster every frame plus a tile-row copy, and it is what proved
// the rewrite byte-exact (0 mismatches on all five routes, 2026-08-03).
//   EXTRA_FLAGS="-DDEBUG_BLASTEM_CHECKPOINT=1 -DBILLBOARD_RASTER_VERIFY=1"
// then read `bb VERIFY mismatch` from tools/decode-cadence.py.
#ifndef BILLBOARD_RASTER_VERIFY
#define BILLBOARD_RASTER_VERIFY 0
#endif

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
//
// Takes the already-selected per-visual offsets row rather than the visual id:
// FREEDOOM_BILLBOARD_PICKUP_POST_OFFSETS is 2D, so indexing it by visual id
// inside the pixel loop repeated a row multiply that is invariant per object.
static inline bool pickup_post_contains(const u16 *offsets, u8 tex_x, u8 tex_y) {
    const u16 begin = offsets[tex_x];
    const u16 end = offsets[tex_x + 1];

    for (u16 post = begin; post < end; post++) {
        const u8 top = FREEDOOM_BILLBOARD_PICKUP_POSTS[post][0];
        const u8 length = FREEDOOM_BILLBOARD_PICKUP_POSTS[post][1];
        if (tex_y < top) return FALSE;
        if ((u8)(tex_y - top) < length) return TRUE;
    }
    return FALSE;
}

// Everything the byte loop needs that is constant for one object. Passed as one
// pointer so the loop itself keeps its live set inside the 68000 register file:
// the previous single-scope implementation had ~25 live values in one function
// and GCC spilled almost all of them, reloading pointers from the stack on
// every pixel (measured ~545 cycles per pixel slot).
typedef struct {
    const RayColumn *columns;
    const u8 *lut;
    const u8 *tex_x_by_col;
    // NULL unless this sprite's opaque spans are described by generated posts.
    const u16 *post_offsets;
    u16 depth;
    u16 first_byte;
    u16 last_byte;
    bool has_door_overlay;
} BillboardRasterJob;

// One packed tile is 8 rows of u32.
#define VIEW_TILE_BYTES ((s16)sizeof(g_view_tiles[0]))

// Rasterize one sprite row into the packed tile buffer.
//
// A packed tile row is a u32 holding 8 nibbles, pixel `col` living at bit
// (7 - (col & 7)) * 4 -- so on this big-endian target byte (col & 7) >> 1 of
// that u32 holds exactly two adjacent screen pixels, even column in the high
// nibble. Walking bytes instead of accumulating the whole 32-bit tile word is
// what makes this cheap: the old code built the word with `(u32)texel << shift`
// for a shift that reaches 28, and a variable LSL.L costs 8 + 2n cycles on the
// 68000, so every opaque texel paid up to 128 cycles in shifts alone. Byte
// nibble placement is a constant 4-bit shift or none at all.
//
// Out-of-range and wall-occluded columns are pre-marked 0xFF in tex_x_by_col by
// the caller, including the at most two columns that the first/last byte adds
// beyond [x0, x1], so this loop needs no per-pixel bounds test.
static void raster_sprite_row(const BillboardRasterJob *job,
                              const u8 *tex_row, u8 tex_y, u16 y) {
    const u8 *sc = &job->tex_x_by_col[job->first_byte << 1];
    const u8 *lut = job->lut;
    const u16 *post_offsets = job->post_offsets;
    u16 tile_index = view_tile_index((u16)(job->first_byte >> 2), (u16)(y >> 3));
    u8 *dst = (u8 *)&g_view_tiles[tile_index][y & 7] + (job->first_byte & 3);
    u16 lane = (u16)(job->first_byte & 3);
    bool tile_marked = FALSE;

    for (u16 b = job->first_byte; b <= job->last_byte; b++) {
        u8 mask = 0;
        u8 value = 0;
        u8 tex_x;

        BB_INC(bytes);
        tex_x = sc[0];
        if (tex_x != 0xFF &&
            (post_offsets == NULL || pickup_post_contains(post_offsets, tex_x, tex_y))) {
            const u8 texel = lut[tex_row[tex_x] & 0x0F];
            if (texel != 0) {
                BB_INC(opaque);
                mask = 0xF0;
                value = (u8)(texel << 4);
            }
        }
        tex_x = sc[1];
        if (tex_x != 0xFF &&
            (post_offsets == NULL || pickup_post_contains(post_offsets, tex_x, tex_y))) {
            const u8 texel = lut[tex_row[tex_x] & 0x0F];
            if (texel != 0) {
                BB_INC(opaque);
                mask |= 0x0F;
                value |= texel;
            }
        }
        sc += 2;

        // A moving door slab in front of this sprite hides it per pixel row.
        // Both screen pixels of a byte share one sampled wall column (and at
        // stride 4 both bytes of a group do), so this stays a per-byte test.
        if (mask != 0 && (!job->has_door_overlay ||
                !door_overlay_blocks_pixel(&job->columns[(u16)((b << 1) & ~(RAY_COL_STRIDE - 1))],
                                           job->depth, y))) {
            // Must snapshot the tile before the first write to it: the overlay
            // restore path replays that snapshot to erase the sprite.
            if (!tile_marked) {
                renderer_mark_overlay_tile(tile_index);
                tile_marked = TRUE;
            }
            BB_INC(commits);
            *dst = (u8)((*dst & (u8)~mask) | value);
        }

        dst++;
        if (++lane == 4) {
            lane = 0;
            // Adjacent tile COLUMNS are VIEW_TILE_H apart, not 1: view_tile_index
            // is column-major (tile_x * VIEW_TILE_H + tile_y) so that a changed
            // column uploads as one DMA run. dst has already stepped one byte
            // past lane 3, so it needs the rest of that same jump.
            tile_index = (u16)(tile_index + VIEW_TILE_H);
            dst += (s16)(VIEW_TILE_BYTES * VIEW_TILE_H) - 4;
            tile_marked = FALSE;
        }
    }
}

// Draw projected billboards object-by-object in painter order, pixel-exact in
// texture and depth decisions.
static void draw_billboards_bytewise(const RayColumn *columns,
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
        const bool use_pickup_posts = (bool)(
            object->visual_id < FREEDOOM_BILLBOARD_PICKUP_TEXTURE_COUNT &&
            FREEDOOM_BILLBOARD_PICKUP_USE_POSTS[object->visual_id]);
        // 0xFF marks a clipped/wall-hidden screen column. Generated atlas X
        // coordinates are far below 255, so the sentinel cannot alias a texel.
        u8 tex_x_by_screen_col[RAY_VIEW_COLS];
        BillboardRasterJob job;
        s16 x0 = object->left;
        s16 x1 = object->right;
        s16 y0 = object->top;
        s16 y1 = object->bottom;
        u32 tex_x_step;
        const u8 atlas_x_last = (u8)(object->atlas_x + object->atlas_w - 1);

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
        BB_INC(objects);
#if DEBUG_PERF
        const u32 pickup_post_start = (measure_pickup_posts && use_pickup_posts) ?
                                          getSubTick() : 0;
#endif

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
        // The byte loop covers whole 2-pixel bytes, so it reaches one column
        // before x0 when x0 is odd and one past x1 when x1 is even. Marking
        // those skipped is what lets the loop drop its per-pixel bounds test.
        // Both indices stay inside the array: RAY_VIEW_COLS is even, so an odd
        // x0 is >= 1 and an even x1 is <= RAY_VIEW_COLS - 2.
        if (x0 & 1) {
            tex_x_by_screen_col[x0 - 1] = 0xFF;
        }
        if ((x1 & 1) == 0) {
            tex_x_by_screen_col[x1 + 1] = 0xFF;
        }

        job.columns = columns;
        job.lut = MEGALDOOM_BILLBOARD_REMAP[
            (object->visual_id < 6) ? object->visual_id : BILLBOARD_VISUAL_BONUS];
        job.tex_x_by_col = tex_x_by_screen_col;
        job.post_offsets = use_pickup_posts ?
            FREEDOOM_BILLBOARD_PICKUP_POST_OFFSETS[object->visual_id] : NULL;
        job.depth = object->depth;
        job.first_byte = (u16)(x0 >> 1);
        job.last_byte = (u16)(x1 >> 1);
        job.has_door_overlay = FALSE;

        // Most views have no moving door slab across a sprite. Detect that once
        // per object so the row loop can short-circuit the full vertical
        // door/depth test instead of repeating it for every byte.
        for (u16 wall_col = (u16)(x0 & ~(RAY_COL_STRIDE - 1));
             wall_col <= (u16)x1;
             wall_col = (u16)(wall_col + RAY_COL_STRIDE)) {
            const RayDoorOverlay *door = &columns[wall_col].door;
            if (door->height != 0 && door->depth < columns[wall_col].depth &&
                object->depth >= door->depth) {
                job.has_door_overlay = TRUE;
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
            if (tex_y > atlas_y_last) tex_y = atlas_y_last;
            tex_y_acc += tex_y_step;
            BB_INC(rows);
            raster_sprite_row(&job,
                              &tex.pixels[scene_mulu_word(tex_y, tex.w)],
                              tex_y, (u16)y);
        }
#if DEBUG_PERF
        if (measure_pickup_posts && use_pickup_posts) {
            renderer_perf_record_deep(RENDERER_PERF_DEEP_PICKUP_POSTS,
                                      getSubTick() - pickup_post_start, 1);
        }
#endif
    }
}

#if BILLBOARD_RASTER_VERIFY
// Pre-2026-08-03 rasterizer, retained verbatim in structure as the differential
// reference: accumulate a 32-bit tile word across a tile_x/pair/pixel loop nest
// with variable shifts, then commit it. Writes into a caller-supplied buffer and
// never marks overlay tiles, so running it alongside the shipping path cannot
// perturb the real frame or the overlay snapshot set.
static void draw_billboards_reference(const RayColumn *columns,
                                      const ProjectedBillboard *objects,
                                      u16 object_count,
                                      u32 band[][8], u16 band_y) {
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
        const u16 *post_offsets = use_pickup_posts ?
            FREEDOOM_BILLBOARD_PICKUP_POST_OFFSETS[object->visual_id] : NULL;
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

        if ((height <= 0) || (width <= 0)) continue;
        tex_x_step = divu((u32)object->atlas_w << 8, (u16)width);
        if (x0 < 0) x0 = 0;
        if (x1 >= RAY_VIEW_COLS) x1 = RAY_VIEW_COLS - 1;
        if (y0 < 0) y0 = 0;
        if (y1 >= VIEW_PIXEL_H) y1 = VIEW_PIXEL_H - 1;
        if ((x0 > x1) || (y0 > y1)) continue;
        first_tile_x = (u16)(x0 >> 3);
        last_tile_x = (u16)(x1 >> 3);
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
        const u32 tex_y_step = divu32_16_exact((u32)object->atlas_h << 16, (u16)height);
        u32 tex_y_acc = ((u32)object->atlas_y << 16) +
                        scene_mul_u32_u16(tex_y_step, (u16)(y0 - object->top));
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
                        if (post_offsets != NULL &&
                            !pickup_post_contains(post_offsets, tex_x, tex_y)) {
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

                if (clear_mask != 0 && tile_y == band_y) {
                    u32 *dst = &band[tile_x][row_y];
                    *dst = (*dst & ~clear_mask) | value;
                }
            }
        }
    }
}

// One tile row of scratch, not the whole view. A full VIEW_TILE_COUNT shadow
// buffer is 9600 bytes and left only 13494 bytes of work RAM free, which is
// under the ~13.7KB where SGDK panics at boot with "not enough memory to reset
// VDP". Verifying a single rotating tile row is still an exact comparison --
// the reference simply drops commits outside the band -- and 15 bands cycle in
// 15 frames, so a route of a few hundred frames covers every tile many times.
static u32 s_verify_band[VIEW_TILE_W][8];
static u16 s_verify_band_y;

void draw_projected_billboards(const RayColumn *columns,
                               const ProjectedBillboard *objects,
                               u16 object_count) {
    const u16 band_y = s_verify_band_y;

    // Compare per object, not per frame: a whole-frame compare only says the
    // final buffers differ, and every later object then draws over a base the
    // two paths already disagree about.
    for (u16 i = 0; i < object_count; i++) {
        for (u16 tile = 0; tile < VIEW_TILE_W; tile++) {
            for (u16 row = 0; row < 8; row++) {
                s_verify_band[tile][row] = g_view_tiles[view_tile_index(tile, band_y)][row];
            }
        }
        draw_billboards_bytewise(columns, &objects[i], 1);
        draw_billboards_reference(columns, &objects[i], 1, s_verify_band, band_y);
        for (u16 tile = 0; tile < VIEW_TILE_W; tile++) {
            for (u16 row = 0; row < 8; row++) {
                if (s_verify_band[tile][row] != g_view_tiles[view_tile_index(tile, band_y)][row]) {
                    g_cadence_bb_mismatch++;
                }
            }
        }
    }
    s_verify_band_y = (u16)((band_y + 1) % VIEW_TILE_H);
}

#else
void draw_projected_billboards(const RayColumn *columns,
                               const ProjectedBillboard *objects,
                               u16 object_count) {
    draw_billboards_bytewise(columns, objects, object_count);
}
#endif
