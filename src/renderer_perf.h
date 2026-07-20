#ifndef MEGALDOOM_RENDERER_PERF_H
#define MEGALDOOM_RENDERER_PERF_H

#include <genesis.h>

#if DEBUG_PERF
typedef enum {
    RENDERER_PERF_DEEP_BSP_SIDE = 0,
    RENDERER_PERF_DEEP_BSP_BOX,
    RENDERER_PERF_DEEP_BSP_SEGMENT,
    RENDERER_PERF_DEEP_PACK_MIXED,
    RENDERER_PERF_DEEP_PACK_FLAT,
    RENDERER_PERF_DEEP_PICKUP_POSTS,
    RENDERER_PERF_DEEP_COUNT
} RendererPerfDeepPhase;

typedef struct {
    u16 upload_dirty_tiles;
    u16 upload_tiles;
    u16 upload_runs;
    u16 redraw_reasons;
    u16 overlay_restored_tiles;
    u16 overlay_touched_tiles;
    u16 overlay_overlap_tiles;
    bool upload_full;
    bool upload_swap;
    u32 gameplay_subticks;
    u32 cast_subticks;
    u32 pack_subticks;
    u32 projection_subticks;
    u32 billboard_subticks;
    u32 weapon_subticks;
    u32 upload_prepare_subticks;
    u32 dma_wait_subticks;
    u32 diagnostics_subticks;
    u16 total_vblanks;
    u16 average_vblanks_x10;
    u16 p95_vblanks;
    u16 max_vblanks;
    u16 missed_deadlines;
    u32 deep_subticks[RENDERER_PERF_DEEP_COUNT];
    u16 deep_units[RENDERER_PERF_DEEP_COUNT];
    u8 deep_phase;
    u16 asm_compare_tile;
    u16 asm_checked_tiles;
    u16 asm_mismatches;
    u16 asm_canary_failures;
    u16 asm_cycles;
    // ColumnReuseOracle (Phase 2 measurement): how the per-column coherence
    // cache would translate into per-column DMA. columns_changed/reused are the
    // last base-rebuild frame's split; hypothetical_tiles_uploaded is
    // columns_changed * VIEW_TILE_H (what a per-column uploader would ship vs
    // the current unconditional 300).
    u16 columns_changed;
    u16 columns_reused;
    u16 hypothetical_tiles_uploaded;
    // Aggregated across every base-rebuild frame in the run so a single mailbox
    // read at end-of-route yields the distribution, not just the last frame.
    u16 columns_changed_max;
    u16 columns_rebuild_frames;
    u32 columns_changed_sum;
} RendererPerfSnapshot;

void renderer_debug_set_cast_subticks(u32 subticks);
void renderer_debug_set_gameplay_subticks(u32 subticks);
void renderer_debug_set_redraw_reasons(u16 reasons);
void renderer_debug_set_total_vblanks(u16 vblanks);
void renderer_perf_set_pack_subticks(u32 subticks);
void renderer_perf_set_projection_subticks(u32 subticks);
void renderer_perf_set_billboard_subticks(u32 subticks);
void renderer_perf_set_weapon_subticks(u32 subticks);
void renderer_perf_reset_overlay_tiles(void);
void renderer_perf_record_overlay_touch(bool overlaps_previous);
void renderer_perf_record_overlay_restore(void);
void renderer_perf_begin_upload(u16 dirty_tiles, bool full, bool swap);
void renderer_perf_record_upload_run(u16 tiles, u32 issue_subticks);
void renderer_perf_record_dma_wait(u32 subticks);
void renderer_perf_record_diagnostics(u32 subticks);
RendererPerfDeepPhase renderer_perf_get_deep_phase(void);
void renderer_perf_record_deep(RendererPerfDeepPhase phase, u32 subticks, u16 units);
void renderer_perf_record_asm_compare(u16 tile, bool mismatch, bool canary_failure,
                                      bool completed_cycle);
void renderer_perf_record_column_reuse(u16 columns_changed, u16 columns_reused,
                                       u16 hypothetical_tiles_uploaded);
RendererPerfSnapshot renderer_get_perf_snapshot(void);
#endif

#endif
