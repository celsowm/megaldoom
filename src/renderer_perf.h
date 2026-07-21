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
    // SparseTileOracle (Phase 3 measurement): of the tiles in changed
    // columns, how many actually carry wall pixels vs. full ceiling/floor
    // (which a sparse architecture would reference from a shared static tile,
    // costing 0 DMA during movement). Aggregated across rebuild frames;
    // P95 is computed by the offline decoder from *_sum / *_frames.
    u16 sparse_dyn_wall_last;
    u16 sparse_ceiling_last;
    u16 sparse_floor_last;
    u16 sparse_overlay_last;
    u16 sparse_dyn_runs_last;
    u16 sparse_dma_bytes_last;
    u16 sparse_dyn_wall_max;
    u16 sparse_ceiling_max;
    u16 sparse_floor_max;
    u16 sparse_overlay_max;
    u16 sparse_dyn_runs_max;
    u16 sparse_dma_bytes_max;
    u16 sparse_rebuild_frames;
    u32 sparse_dyn_wall_sum;
    u32 sparse_ceiling_sum;
    u32 sparse_floor_sum;
    u32 sparse_overlay_sum;
    u32 sparse_dyn_runs_sum;
    u32 sparse_dma_bytes_sum;
    // Cast-stage reciprocal-depth LUT (draw_seg's divu(BSP_INV_SCALE, invz)
    // replaced by a table lookup, see bsp_inv_depth_lut.h). max_invz is the
    // largest invz value observed at that call site all run, to empirically
    // corroborate the table's proven 1..1024 range bound; fallback_hits counts
    // how often the bounds-checked divu fallback actually fired (should be 0
    // given the proof, but kept live for real gameplay, not just captured
    // routes).
    u16 cast_lut_max_invz;
    u16 cast_lut_fallback_hits;
    // Billboard-measure reciprocal LUT (billboard_projection_lut.h), same
    // proven-bound pattern as the cast LUT above.
    u16 billboard_lut_max_forward;
    u16 billboard_lut_fallback_hits;
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
void renderer_perf_record_sparse(u16 dyn_wall, u16 ceiling, u16 floor,
                                 u16 overlay, u16 runs, u16 dma_bytes);
void renderer_perf_record_cast_lut(u16 invz, bool fallback);
void renderer_perf_record_billboard_lut(u16 forward, bool fallback);
RendererPerfSnapshot renderer_get_perf_snapshot(void);
#endif

#endif
