#include "renderer_perf.h"
#include "player_controller.h"

#if DEBUG_PERF
#define PERF_VBLANK_HISTORY 60
#define PERF_VBLANK_BUCKETS 32

static RendererPerfSnapshot s_perf;
static u8 s_vblank_history[PERF_VBLANK_HISTORY];
static u8 s_vblank_buckets[PERF_VBLANK_BUCKETS];
static u16 s_vblank_history_sum;
static u16 s_vblank_history_count;
static u16 s_vblank_history_cursor;

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
    const u8 sample = (u8)((vblanks < (PERF_VBLANK_BUCKETS - 1)) ?
                               vblanks : (PERF_VBLANK_BUCKETS - 1));

    s_perf.total_vblanks = vblanks;
    if (vblanks > s_perf.max_vblanks) s_perf.max_vblanks = vblanks;
    if (vblanks > TARGET_FRAME_VSYNCS) s_perf.missed_deadlines++;

    if (s_vblank_history_count == PERF_VBLANK_HISTORY) {
        const u8 previous = s_vblank_history[s_vblank_history_cursor];
        s_vblank_history_sum = (u16)(s_vblank_history_sum - previous);
        s_vblank_buckets[previous]--;
    } else {
        s_vblank_history_count++;
    }
    s_vblank_history[s_vblank_history_cursor] = sample;
    s_vblank_history_sum = (u16)(s_vblank_history_sum + sample);
    s_vblank_buckets[sample]++;
    s_vblank_history_cursor++;
    if (s_vblank_history_cursor == PERF_VBLANK_HISTORY) s_vblank_history_cursor = 0;
    s_perf.average_vblanks_x10 = divu((u32)s_vblank_history_sum * 10u,
                                      s_vblank_history_count);

    // Deep instrumentation rotates once per completed game frame. The next
    // phase is cleared here, so its timer contains exactly one representative
    // sample and the other five retain their last measurements for the overlay.
    s_perf.deep_phase++;
    if (s_perf.deep_phase >= RENDERER_PERF_DEEP_COUNT) s_perf.deep_phase = 0;
    s_perf.deep_subticks[s_perf.deep_phase] = 0;
    s_perf.deep_units[s_perf.deep_phase] = 0;
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

void renderer_perf_record_diagnostics(u32 subticks) {
    u16 accumulated = 0;
    const u16 target = divu((u32)s_vblank_history_count * 95u + 99u, 100u);

    s_perf.diagnostics_subticks = subticks;
    for (u16 bucket = 0; bucket < PERF_VBLANK_BUCKETS; bucket++) {
        accumulated = (u16)(accumulated + s_vblank_buckets[bucket]);
        if (accumulated >= target) {
            s_perf.p95_vblanks = bucket;
            break;
        }
    }
}

RendererPerfDeepPhase renderer_perf_get_deep_phase(void) {
    return (RendererPerfDeepPhase)s_perf.deep_phase;
}

void renderer_perf_record_deep(RendererPerfDeepPhase phase, u32 subticks, u16 units) {
    if (phase != (RendererPerfDeepPhase)s_perf.deep_phase) return;
    s_perf.deep_subticks[phase] += subticks;
    s_perf.deep_units[phase] = (u16)(s_perf.deep_units[phase] + units);
}

void renderer_perf_record_asm_compare(u16 tile, bool mismatch, bool canary_failure,
                                      bool completed_cycle) {
    s_perf.asm_compare_tile = tile;
    if (s_perf.asm_checked_tiles != 0xFFFF) s_perf.asm_checked_tiles++;
    if (mismatch && s_perf.asm_mismatches != 0xFFFF) s_perf.asm_mismatches++;
    if (canary_failure && s_perf.asm_canary_failures != 0xFFFF) {
        s_perf.asm_canary_failures++;
    }
    if (completed_cycle && s_perf.asm_cycles != 0xFFFF) s_perf.asm_cycles++;
}

void renderer_perf_record_column_reuse(u16 columns_changed, u16 columns_reused,
                                       u16 hypothetical_tiles_uploaded) {
    s_perf.columns_changed = columns_changed;
    s_perf.columns_reused = columns_reused;
    s_perf.hypothetical_tiles_uploaded = hypothetical_tiles_uploaded;
    if (columns_changed > s_perf.columns_changed_max) {
        s_perf.columns_changed_max = columns_changed;
    }
    s_perf.columns_changed_sum += columns_changed;
    s_perf.columns_rebuild_frames++;
}

void renderer_perf_record_sparse(u16 dyn_wall, u16 ceiling, u16 floor,
                                 u16 overlay, u16 runs, u16 dma_bytes) {
    s_perf.sparse_dyn_wall_last = dyn_wall;
    s_perf.sparse_ceiling_last = ceiling;
    s_perf.sparse_floor_last = floor;
    s_perf.sparse_overlay_last = overlay;
    s_perf.sparse_dyn_runs_last = runs;
    s_perf.sparse_dma_bytes_last = dma_bytes;
    if (dyn_wall > s_perf.sparse_dyn_wall_max) s_perf.sparse_dyn_wall_max = dyn_wall;
    if (ceiling > s_perf.sparse_ceiling_max) s_perf.sparse_ceiling_max = ceiling;
    if (floor > s_perf.sparse_floor_max) s_perf.sparse_floor_max = floor;
    if (overlay > s_perf.sparse_overlay_max) s_perf.sparse_overlay_max = overlay;
    if (runs > s_perf.sparse_dyn_runs_max) s_perf.sparse_dyn_runs_max = runs;
    if (dma_bytes > s_perf.sparse_dma_bytes_max) s_perf.sparse_dma_bytes_max = dma_bytes;
    s_perf.sparse_dyn_wall_sum += dyn_wall;
    s_perf.sparse_ceiling_sum += ceiling;
    s_perf.sparse_floor_sum += floor;
    s_perf.sparse_overlay_sum += overlay;
    s_perf.sparse_dyn_runs_sum += runs;
    s_perf.sparse_dma_bytes_sum += dma_bytes;
    s_perf.sparse_rebuild_frames++;
}

void renderer_perf_record_cast_lut(u16 invz, bool fallback) {
    if (invz > s_perf.cast_lut_max_invz) s_perf.cast_lut_max_invz = invz;
    if (fallback && s_perf.cast_lut_fallback_hits != 0xFFFF) {
        s_perf.cast_lut_fallback_hits++;
    }
}

void renderer_perf_record_billboard_lut(u16 forward, bool fallback) {
    if (forward > s_perf.billboard_lut_max_forward) {
        s_perf.billboard_lut_max_forward = forward;
    }
    if (fallback && s_perf.billboard_lut_fallback_hits != 0xFFFF) {
        s_perf.billboard_lut_fallback_hits++;
    }
}

void renderer_perf_record_visible_subsectors(u16 visible_subsectors,
                                             u16 visible_subsector_objects,
                                             u16 safe_objects,
                                             u16 cullable_objects) {
    s_perf.visible_subsectors_last = visible_subsectors;
    s_perf.visible_subsector_objects_last = visible_subsector_objects;
    if (visible_subsectors > s_perf.visible_subsectors_max) {
        s_perf.visible_subsectors_max = visible_subsectors;
    }
    if (visible_subsector_objects > s_perf.visible_subsector_objects_max) {
        s_perf.visible_subsector_objects_max = visible_subsector_objects;
    }
    s_perf.visible_subsectors_sum += visible_subsectors;
    s_perf.visible_subsector_objects_sum += visible_subsector_objects;
    s_perf.visible_subsector_safe_objects_last = safe_objects;
    s_perf.visible_subsector_cullable_objects_last = cullable_objects;
    if (safe_objects > s_perf.visible_subsector_safe_objects_max) {
        s_perf.visible_subsector_safe_objects_max = safe_objects;
    }
    if (cullable_objects > s_perf.visible_subsector_cullable_objects_max) {
        s_perf.visible_subsector_cullable_objects_max = cullable_objects;
    }
    s_perf.visible_subsector_safe_objects_sum += safe_objects;
    s_perf.visible_subsector_cullable_objects_sum += cullable_objects;
    s_perf.visible_subsector_frames++;
}

RendererPerfSnapshot renderer_get_perf_snapshot(void) {
    return s_perf;
}
#endif
