#include "renderer_internal.h"
#include "renderer_perf.h"

static u32 s_previous_bits[VIEW_DIRTY_WORD_COUNT];
static u32 s_current_bits[VIEW_DIRTY_WORD_COUNT];
static u32 s_base_snapshot_valid_bits[VIEW_DIRTY_WORD_COUNT];

void renderer_overlay_reset(void) {
    for (u16 i = 0; i < VIEW_DIRTY_WORD_COUNT; i++) {
        s_previous_bits[i] = 0;
        s_current_bits[i] = 0;
        s_base_snapshot_valid_bits[i] = 0;
    }
}

void renderer_overlay_base_rebuilt(void) {
    for (u16 i = 0; i < VIEW_DIRTY_WORD_COUNT; i++) {
        s_previous_bits[i] = 0;
        s_base_snapshot_valid_bits[i] = 0;
    }
}

void renderer_overlay_restore_previous(void) {
    for (u16 tile = 0; tile < VIEW_TILE_COUNT; tile++) {
        const u16 word = (u16)(tile >> 5);
        const u32 mask = (u32)1u << (tile & 31);
        if ((s_previous_bits[word] & mask) == 0) continue;

        for (u16 row = 0; row < 8; row++) {
            g_view_tiles[tile][row] = g_base_view_tiles[tile][row];
        }
#if DEBUG_PERF
        renderer_perf_record_overlay_restore();
#endif
        renderer_mark_tile_dirty(tile);
    }
}

void renderer_overlay_begin(void) {
    for (u16 i = 0; i < VIEW_DIRTY_WORD_COUNT; i++) s_current_bits[i] = 0;
}

void renderer_overlay_finish(void) {
    for (u16 i = 0; i < VIEW_DIRTY_WORD_COUNT; i++) {
        s_previous_bits[i] = s_current_bits[i];
    }
}

void renderer_mark_overlay_tile(u16 tile_index) {
    const u16 word = (u16)(tile_index >> 5);
    const u32 mask = (u32)1u << (tile_index & 31);

    if ((s_current_bits[word] & mask) != 0) return;
#if DEBUG_PERF
    renderer_perf_record_overlay_touch((bool)((s_previous_bits[word] & mask) != 0));
#endif
    if ((s_base_snapshot_valid_bits[word] & mask) == 0) {
        // Capture pristine base lazily: only overlay-touched tiles pay the copy.
        for (u16 row = 0; row < 8; row++) {
            g_base_view_tiles[tile_index][row] = g_view_tiles[tile_index][row];
        }
        s_base_snapshot_valid_bits[word] |= mask;
    }
    // restore_previous() cleaned old contamination; a newly touched tile was
    // already pristine, so no second snapshot-to-view copy is necessary.
    s_current_bits[word] |= mask;
    renderer_mark_tile_dirty(tile_index);
}
