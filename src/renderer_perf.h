#ifndef MEGALDOOM_RENDERER_PERF_H
#define MEGALDOOM_RENDERER_PERF_H

#include <genesis.h>

#if DEBUG_PERF
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
    u16 total_vblanks;
    u16 max_vblanks;
    u16 missed_deadlines;
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
RendererPerfSnapshot renderer_get_perf_snapshot(void);
#endif

#endif
