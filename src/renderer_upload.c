#include "renderer_pack_internal.h"
#include "renderer_perf.h"

static bool g_upload_requires_bank_swap = FALSE;

typedef struct {
    bool pending;
    bool full;
    bool swap;
    u16 bank;
    u16 cursor;
} ViewUploadState;

static ViewUploadState g_view_upload;

#if RENDERER_SPARSE_FB
// Mixed-tilemap scratch buffer for the sparse upload path. Only written to via
// the screen_tilemap pointer parameter of sparse_build_tilemap(); read here
// when committing the mixed tilemap to the BG_B plane. Dead storage while
// RENDERER_SPARSE_FB == 0.
static u16 g_sparse_screen_tilemap[VIEW_TILE_W * VIEW_TILE_H];
#endif

void upload_state_init(void) {
    g_upload_requires_bank_swap = FALSE;
    g_view_upload = (ViewUploadState){FALSE, FALSE, FALSE, 0, 0};
}

void upload_state_invalidate(void) {
    g_upload_requires_bank_swap = FALSE;
}

void upload_request_bank_swap(void) {
    g_upload_requires_bank_swap = TRUE;
}

void clear_all_view_banks_dirty(void) {
    for (u16 bank = 0; bank < VIEW_BANK_COUNT; bank++) {
        for (u16 word = 0; word < VIEW_DIRTY_WORD_COUNT; word++) {
            g_view_bank_dirty_bits[bank][word] = 0;
        }
        g_view_bank_dirty_count[bank] = 0;
    }
}

static bool view_bank_tile_is_dirty(u16 bank, u16 tile) {
    const u16 word = (u16)(tile >> 5);
    const u32 mask = (u32)1u << (tile & 31);
    return (bool)((g_view_bank_dirty_bits[bank][word] & mask) != 0);
}

static void clear_view_bank_dirty_bits(u16 bank) {
    for (u16 word = 0; word < VIEW_DIRTY_WORD_COUNT; word++) {
        g_view_bank_dirty_bits[bank][word] = 0;
    }
    g_view_bank_dirty_count[bank] = 0;
}

static u16 count_view_bank_dirty_runs(u16 bank) {
    u16 runs = 0;
    bool in_run = FALSE;

    for (u16 tile = 0; tile < VIEW_TILE_COUNT; tile++) {
        const bool dirty = view_bank_tile_is_dirty(bank, tile);
        if (dirty && !in_run) {
            runs++;
        }
        in_run = dirty;
    }

    return runs;
}

void load_view_tile_run(u16 vram_base, u16 first, u16 count) {
#if DEBUG_PERF
    const u32 issue_start = getSubTick();
#endif
    VDP_loadTileData((const u32 *)&g_view_tiles[first][0],
                     (u16)(vram_base + first),
                     count,
                     DMA);
#if DEBUG_PERF
    renderer_perf_record_upload_run(count, getSubTick() - issue_start);
#endif
}

#if DEBUG_PERF
static void dbg_wait_dma(void) {
    const u32 t = getSubTick();
    VDP_waitDMACompletion();
    renderer_perf_record_dma_wait(getSubTick() - t);
}
#else
#define dbg_wait_dma() VDP_waitDMACompletion()
#endif

static void clear_view_bank_dirty_range(u16 bank, u16 first, u16 end) {
    for (u16 tile = first; tile < end; tile++) {
        const u16 word = (u16)(tile >> 5);
        const u32 mask = (u32)1u << (tile & 31);
        if ((g_view_bank_dirty_bits[bank][word] & mask) != 0) {
            g_view_bank_dirty_bits[bank][word] &= ~mask;
            g_view_bank_dirty_count[bank]--;
        }
    }
}

static bool choose_full_view_upload(u16 bank) {
    const u16 dirty_count = g_view_bank_dirty_count[bank];
    return (bool)((dirty_count >= VIEW_DIRTY_FULL_THRESHOLD) ||
                  (count_view_bank_dirty_runs(bank) > VIEW_DIRTY_MAX_RUNS));
}

void renderer_queue_scene_upload(const RayColumn *columns,
                                 const RaySceneColors *scene_colors) {
    const bool swap = g_upload_requires_bank_swap;
    const u16 bank = swap ? (u16)(g_view_vram_bank ^ 1) : g_view_vram_bank;

#if RENDERER_SPARSE_FB
    // Phase 4, Task 4: sparse upload path. Only when a bank swap is pending do
    // we classify the frame (from the column descriptors — packer untouched),
    // and if the dynamic tile count fits in one vblank, DMA just the dynamic
    // runs into the inactive bank, build the mixed tilemap (static atlas tiles
    // + dynamic bank tiles) and commit it straight to the BG_B plane, then hand
    // off to the EXISTING deferred pump so the bank swap happens only AFTER the
    // DMAs land (dbg_wait_dma), exactly like the legacy path. Otherwise we fall
    // through to the legacy full-base upload below.
    if (swap) {
        static SparseFrameBuild s_build;
        sparse_classify_frame(columns, scene_colors, &s_build);
        if (s_build.dynamic_tile_count <= SPARSE_ONE_VBLANK_BUDGET) {
            const u16 inactive_bank_base =
                (u16)(VIEW_TILE_BASE + (bank * VIEW_TILE_COUNT));
            // DMA just the dynamic runs into the inactive bank's column-major
            // tile positions (deferred — they land during the next vblank wait
            // in upload_view_tilemap_step's dbg_wait_dma()).
            sparse_queue_dynamic_runs(&s_build, inactive_bank_base);
            // Build the mixed tilemap (ceiling atlas / floor / dynamic bank
            // tiles) into the buffer. The actual plane commit is DEFERRED to
            // finish_view_upload()'s swap branch, which runs AFTER dbg_wait_dma()
            // — so the reveal happens only once the dynamic DMAs have landed,
            // exactly like the legacy path (no sub-frame flash even if this
            // function is ever called earlier in the active display).
            sparse_build_tilemap(columns, scene_colors, scene_colors->sector,
                                 bank, g_sparse_screen_tilemap, &s_build);
            g_sparse_tilemap_committed = TRUE;
            // Hand off to the EXISTING deferred pump; the swap (in
            // finish_view_upload, after dbg_wait_dma) commits the mixed tilemap
            // to the plane and flips the bank. Mark upload complete, non-full.
            g_upload_requires_bank_swap = FALSE;
            g_view_upload.pending = TRUE;
            g_view_upload.full = FALSE;
            g_view_upload.swap = TRUE;
            g_view_upload.bank = bank;
            g_view_upload.cursor = VIEW_TILE_COUNT;
            return;
        }
    }
#endif

    g_upload_requires_bank_swap = FALSE;
    g_view_upload.pending = (bool)(g_view_bank_dirty_count[bank] != 0);
    g_view_upload.full = swap ? TRUE : choose_full_view_upload(bank);
    g_view_upload.swap = swap;
    g_view_upload.bank = bank;
    g_view_upload.cursor = 0;
#if DEBUG_PERF
    renderer_perf_begin_upload(g_view_bank_dirty_count[bank],
                               g_view_upload.full, swap);
#endif
}

bool renderer_scene_upload_pending(void) {
    return g_view_upload.pending;
}

static void finish_view_upload(void) {
    g_view_upload.pending = FALSE;
    if (g_view_upload.full) {
        clear_view_bank_dirty_bits(g_view_upload.bank);
    }
    if (g_view_upload.swap) {
#if RENDERER_SPARSE_FB
        if (g_sparse_tilemap_committed) {
            // DMA has landed (this runs after upload_view_tilemap_step's
            // dbg_wait_dma). Commit the mixed tilemap to the plane now, then
            // flip the bank select. This restores the strict legacy invariant:
            // the reveal happens only AFTER the dynamic tiles are in VRAM.
            VDP_setTileMapDataRect(BG_B, g_sparse_screen_tilemap,
                                   VIEW_TILEMAP_X, VIEW_TILEMAP_Y,
                                   VIEW_TILE_W, VIEW_TILE_H, VIEW_TILE_W, CPU);
            g_sparse_tilemap_committed = FALSE;
            g_view_vram_bank = (u16)(g_view_upload.bank & 1);
        } else {
            renderer_set_view_vram_bank(g_view_upload.bank);
        }
#else
        renderer_set_view_vram_bank(g_view_upload.bank);
#endif
    }
}

static void upload_view_tilemap_step(void) {
    u16 budget = VIEW_DMA_TILES_PER_VBLANK;
    const u16 start_cursor = g_view_upload.cursor;
    const u16 vram_base =
        (u16)(VIEW_TILE_BASE + (g_view_upload.bank * VIEW_TILE_COUNT));

    if (!g_view_upload.pending) return;

    if (g_view_upload.full) {
        const u16 remaining = (u16)(VIEW_TILE_COUNT - g_view_upload.cursor);
        const u16 count = (remaining < budget) ? remaining : budget;
        load_view_tile_run(vram_base, g_view_upload.cursor, count);
        g_view_upload.cursor = (u16)(g_view_upload.cursor + count);
    } else {
        while ((g_view_upload.cursor < VIEW_TILE_COUNT) && (budget > 0)) {
            while ((g_view_upload.cursor < VIEW_TILE_COUNT) &&
                   !view_bank_tile_is_dirty(g_view_upload.bank, g_view_upload.cursor)) {
                g_view_upload.cursor++;
            }
            if (g_view_upload.cursor >= VIEW_TILE_COUNT) break;

            const u16 first = g_view_upload.cursor;
            while ((g_view_upload.cursor < VIEW_TILE_COUNT) &&
                   view_bank_tile_is_dirty(g_view_upload.bank, g_view_upload.cursor) &&
                   ((u16)(g_view_upload.cursor - first) < budget)) {
                g_view_upload.cursor++;
            }
            const u16 count = (u16)(g_view_upload.cursor - first);
            load_view_tile_run(vram_base, first, count);
            budget = (u16)(budget - count);
        }
    }

    dbg_wait_dma();
    if (!g_view_upload.full) {
        clear_view_bank_dirty_range(g_view_upload.bank, start_cursor,
                                    g_view_upload.cursor);
    }
    if (g_view_upload.cursor >= VIEW_TILE_COUNT) {
        finish_view_upload();
    }
}

void renderer_upload_scene_step(void) {
#if DEBUG_PERF
    const bool was_pending = g_view_upload.pending;
#endif
    upload_view_tilemap_step();
    upload_compass_tilemap();
#if DEBUG_PERF
    renderer_draw_perf_overlay((bool)(was_pending && !g_view_upload.pending));
#endif
}
