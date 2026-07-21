#include "renderer_pack_internal.h"

#if RENDERER_SPARSE_FB
// Phase 3, Task 3: sparse uploader. Reuses the dynamic union already produced by
// sparse_classify_frame (build->dynamic_rows[x] is a 15-bit per-column mask of
// dynamic tiles). For each column we walk its bits, and for every maximal run of
// set bits [first_y, last_y] we DMA that contiguous source range with the
// existing load_view_tile_run and record it in build->runs[]. No new mask arrays;
// no reimplemented DMA. Dead code while RENDERER_SPARSE_FB == 0.
//
// IMPORTANT: load_view_tile_run couples source index == destination VRAM index
// (it writes inactive_bank_base + src_first). So a dynamic tile keeps its
// column-major VRAM position; we do NOT compact the destination. slot_allocation[]
// and run->dest_slot therefore record the SAME column-major index the DMA wrote,
// so sparse_build_tilemap (Phase 4) points each dynamic cell at the right tile.
// The DMA-byte saving comes from skipping static (ceiling/floor) tiles, not from
// compression.
void sparse_queue_dynamic_runs(const SparseFrameBuild *build,
                               u16 inactive_bank_base) {
    // Cast away const to record the run list / slot allocation into the build.
    SparseFrameBuild *out = (SparseFrameBuild *)build;
    out->dynamic_run_count = 0;
    out->dynamic_tile_count = 0;

    for (u16 x = 0; x < VIEW_TILE_W; x++) {
        const u16 rows = build->dynamic_rows[x];
        u16 y = 0;
        while (y < VIEW_TILE_H) {
            if ((rows & (u16)(1u << y)) == 0) {
                y++;
                continue;
            }
            const u16 first_y = y;
            while ((y < VIEW_TILE_H) && ((rows & (u16)(1u << y)) != 0)) {
                y++;
            }
            const u16 run_len = (u16)(y - first_y);
            const u16 src_first = (u16)(x * VIEW_TILE_H + first_y);

            load_view_tile_run(inactive_bank_base, src_first, run_len);

            SparseTileRun *run = &out->runs[out->dynamic_run_count];
            run->source_y = x;
            run->tile_count = run_len;
            // Destination == source column-major position (see note above).
            run->dest_slot = src_first;
            for (u16 k = 0; k < run_len; k++) {
                out->slot_allocation[src_first + k] = (u16)(src_first + k);
            }
            out->dynamic_tile_count = (u16)(out->dynamic_tile_count + run_len);
            out->dynamic_run_count++;
        }
    }
}
#endif

// Phase 4, Task 4: classify the frame from the column descriptors (NOT the
// packer's mask arrays — the packer is untouched). A tile (x,y) is STATIC
// (ceiling or floor, served from the resident atlas, zero per-frame DMA) iff
// its 8px band is entirely above every wall top (pure ceiling) or entirely
// below every wall bottom (pure floor). Any other band is DYNAMIC (wall/door/
// overlay — needs per-frame VRAM). This is the exact same three-way split
// sparse_build_tilemap uses, so dynamic_rows[x] matches the oracle's dyn count
// and the mixed tilemap's static/dynamic layout. Dead code at flag 0.
#if RENDERER_SPARSE_FB
void sparse_classify_frame(const RayColumn *columns,
                           const RaySceneColors *scene_colors,
                           SparseFrameBuild *build) {
    (void)scene_colors;
    build->dynamic_tile_count = 0;
    build->dynamic_run_count = 0;
    for (u16 x = 0; x < VIEW_TILE_W; x++) {
        const u16 base_col = (u16)(x * 8);
        const WallColumnDescriptor descriptors[4] = {
            describe_wall_column(&columns[base_col]),
            describe_wall_column(&columns[base_col + 2]),
            describe_wall_column(&columns[base_col + 4]),
            describe_wall_column(&columns[base_col + 6])
        };
        u16 dyn = 0;
        for (u16 y = 0; y < VIEW_TILE_H; y++) {
            const u16 pixel_y = (u16)(y * 8);
            const bool pure_ceiling =
                ((pixel_y + 7) < descriptors[0].top) &&
                ((pixel_y + 7) < descriptors[1].top) &&
                ((pixel_y + 7) < descriptors[2].top) &&
                ((pixel_y + 7) < descriptors[3].top);
            const bool pure_floor =
                (pixel_y >= descriptors[0].bottom) &&
                (pixel_y >= descriptors[1].bottom) &&
                (pixel_y >= descriptors[2].bottom) &&
                (pixel_y >= descriptors[3].bottom);
            if (!pure_ceiling && !pure_floor) {
                dyn |= (u16)(1u << y);
            }
        }
        build->dynamic_mask[x] = dyn;
        build->dynamic_rows[x] = dyn;
        // Count set bits for the budget gate.
        u16 bits = dyn;
        while (bits) {
            build->dynamic_tile_count++;
            bits &= (u16)(bits - 1);
        }
    }
}
#else
// Flag-0 stub: never called (the sparse path is dead at flag 0), kept so the
// symbol resolves if anything references it; producing an all-dynamic build
// would be equivalent to the legacy full upload.
void sparse_classify_frame(const RayColumn *columns,
                           const RaySceneColors *scene_colors,
                           SparseFrameBuild *build) {
    (void)columns; (void)scene_colors; (void)build;
}
#endif

#if RENDERER_SPARSE_FB
// Phase 4, Task 4: build the mixed-tilemap screen layout from a SparseFrameBuild.
//
// The screen tilemap stays in screen order (row-major: y*VIEW_TILE_W + x) like
// the legacy build_view_bank_tilemaps. For each screen cell:
//   * pure-ceiling cell  -> the resident ceiling atlas tile for the player's
//     current sector (STATIC_CEILING_ATLAS_BASE + g_sector_ceiling_tile[sector]);
//   * pure-floor cell    -> the single ROM-constant floor tile (STATIC_FLOOR_TILE_BASE);
//   * dynamic cell       -> the dynamic VRAM tile the sparse upload filled this
//     frame in the INACTIVE bank: VIEW_TILE_BASE + bank*VIEW_TILE_COUNT +
//     slot_allocation[col_major_index] (slot_allocation holds the column-major
//     source index the DMA wrote, per the Phase 3 review fix).
//
// Classification reuses the EXACT same three branches as build_bsp_tilemap
// (pixel_y+7 < all tops => ceiling; pixel_y >= all bottoms => floor; else
// dynamic) so the mixed tilemap's static/dynamic split matches the packer's
// output with zero pixel drift. With RENDERER_SPARSE_FB == 0 this is dead code
// that is never called; the legacy full-upload path is untouched.
void sparse_build_tilemap(const RayColumn *columns,
                          const RaySceneColors *scene_colors,
                          u16 sector,
                          u16 bank,
                          u16 *screen_tilemap,
                          const SparseFrameBuild *build) {
    (void)scene_colors;
    const u8 ceil_tile = g_sector_ceiling_tile[(sector < FREEDOOM_SECTOR_VISUAL_COUNT) ?
                                                  sector : 0];
    const u16 ceil_vram = (u16)(STATIC_CEILING_ATLAS_BASE + ceil_tile);
    const u16 floor_vram = STATIC_FLOOR_TILE_BASE;
    const u16 dyn_bank_base = (u16)(VIEW_TILE_BASE + (bank * VIEW_TILE_COUNT));
    for (u16 tile_x = 0; tile_x < VIEW_TILE_W; tile_x++) {
        const u16 base_col = (u16)(tile_x * 8);
        const WallColumnDescriptor descriptors[4] = {
            describe_wall_column(&columns[base_col]),
            describe_wall_column(&columns[base_col + 2]),
            describe_wall_column(&columns[base_col + 4]),
            describe_wall_column(&columns[base_col + 6])
        };
        for (u16 tile_y = 0; tile_y < VIEW_TILE_H; tile_y++) {
            const u16 screen_index = (u16)((tile_y * VIEW_TILE_W) + tile_x);
            const u16 pixel_y = (u16)(tile_y * 8);

            if (((pixel_y + 7) < descriptors[0].top) &&
                ((pixel_y + 7) < descriptors[1].top) &&
                ((pixel_y + 7) < descriptors[2].top) &&
                ((pixel_y + 7) < descriptors[3].top)) {
                screen_tilemap[screen_index] = TILE_ATTR_FULL(
                    PAL3, FALSE, FALSE, FALSE, ceil_vram);
                continue;
            }

            if ((pixel_y >= descriptors[0].bottom) &&
                (pixel_y >= descriptors[1].bottom) &&
                (pixel_y >= descriptors[2].bottom) &&
                (pixel_y >= descriptors[3].bottom)) {
                screen_tilemap[screen_index] = TILE_ATTR_FULL(
                    PAL3, FALSE, FALSE, FALSE, floor_vram);
                continue;
            }

            // Dynamic: point at the inactive bank's VRAM tile the sparse upload
            // filled (column-major source index == dest, per Phase 3 review fix).
            const u16 tile_index = (u16)(tile_x * VIEW_TILE_H + tile_y);
            const u16 slot = build->slot_allocation[tile_index];
            screen_tilemap[screen_index] = TILE_ATTR_FULL(
                PAL3, FALSE, FALSE, FALSE, (u16)(dyn_bank_base + slot));
        }
    }
}
#endif
