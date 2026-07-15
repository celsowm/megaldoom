#include "renderer_perf.h"
#include "player_controller.h"

#if DEBUG_PERF
static RendererPerfSnapshot s_perf;

void renderer_debug_set_cast_subticks(u32 subticks) {
    s_perf.cast_subticks = subticks;
}

void renderer_debug_set_gameplay_subticks(u32 subticks) {
    s_perf.gameplay_subticks = subticks;
}

void renderer_debug_set_redraw_reasons(u16 reasons) {
    s_perf.redraw_reasons = reasons;
}

void renderer_debug_set_total_vblanks(u16 vblanks) {
    s_perf.total_vblanks = vblanks;
    if (vblanks > s_perf.max_vblanks) s_perf.max_vblanks = vblanks;
    if (vblanks > TARGET_FRAME_VSYNCS) s_perf.missed_deadlines++;
}

void renderer_perf_set_pack_subticks(u32 subticks) {
    s_perf.pack_subticks = subticks;
}

void renderer_perf_set_projection_subticks(u32 subticks) {
    s_perf.projection_subticks = subticks;
}

void renderer_perf_set_billboard_subticks(u32 subticks) {
    s_perf.billboard_subticks = subticks;
}

void renderer_perf_set_weapon_subticks(u32 subticks) {
    s_perf.weapon_subticks = subticks;
}

void renderer_perf_reset_overlay_tiles(void) {
    s_perf.overlay_restored_tiles = 0;
    s_perf.overlay_touched_tiles = 0;
    s_perf.overlay_overlap_tiles = 0;
}

void renderer_perf_record_overlay_touch(bool overlaps_previous) {
    s_perf.overlay_touched_tiles++;
    if (overlaps_previous) s_perf.overlay_overlap_tiles++;
}

void renderer_perf_record_overlay_restore(void) {
    s_perf.overlay_restored_tiles++;
}

void renderer_perf_begin_upload(u16 dirty_tiles, bool full, bool swap) {
    s_perf.upload_dirty_tiles = dirty_tiles;
    s_perf.upload_tiles = 0;
    s_perf.upload_runs = 0;
    s_perf.upload_full = full;
    s_perf.upload_swap = swap;
    s_perf.upload_prepare_subticks = 0;
    s_perf.dma_wait_subticks = 0;
}

void renderer_perf_record_upload_run(u16 tiles, u32 issue_subticks) {
    s_perf.upload_prepare_subticks += issue_subticks;
    s_perf.upload_tiles = (u16)(s_perf.upload_tiles + tiles);
    s_perf.upload_runs++;
}

void renderer_perf_record_dma_wait(u32 subticks) {
    s_perf.dma_wait_subticks += subticks;
}

RendererPerfSnapshot renderer_get_perf_snapshot(void) {
    return s_perf;
}
#endif
