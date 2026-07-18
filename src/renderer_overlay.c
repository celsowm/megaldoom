#include "renderer_internal.h"
#include "renderer_perf.h"

static u32 s_previous_bits[VIEW_DIRTY_WORD_COUNT];
static u32 s_current_bits[VIEW_DIRTY_WORD_COUNT];
#define OVERLAY_SNAPSHOT_TILE_LIMIT 128
static u32 s_snapshot_rows[OVERLAY_SNAPSHOT_TILE_LIMIT][8];
static s16 s_snapshot_slot_by_tile[VIEW_TILE_COUNT];
static u16 s_snapshot_count;
static bool s_snapshot_overflow;

static void clear_snapshot_cache(void) {
    for (u16 tile = 0; tile < VIEW_TILE_COUNT; tile++) {
        s_snapshot_slot_by_tile[tile] = -1;
    }
    s_snapshot_count = 0;
    s_snapshot_overflow = FALSE;
}

void renderer_overlay_reset(void) {
    for (u16 i = 0; i < VIEW_DIRTY_WORD_COUNT; i++) {
        s_previous_bits[i] = 0;
        s_current_bits[i] = 0;
    }
    clear_snapshot_cache();
}

void renderer_overlay_base_rebuilt(void) {
    for (u16 i = 0; i < VIEW_DIRTY_WORD_COUNT; i++) {
        s_previous_bits[i] = 0;
    }
    clear_snapshot_cache();
}

void renderer_overlay_restore_previous(void) {
    for (u16 tile = 0; tile < VIEW_TILE_COUNT; tile++) {
        const u16 word = (u16)(tile >> 5);
        const u32 mask = (u32)1u << (tile & 31);
        if ((s_previous_bits[word] & mask) == 0) continue;

        const s16 slot = s_snapshot_slot_by_tile[tile];
        if (slot < 0) continue;
        for (u16 row = 0; row < 8; row++)
            g_view_tiles[tile][row] = s_snapshot_rows[slot][row];
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

bool renderer_overlay_requires_base_rebuild(void) {
    return s_snapshot_overflow;
}

void renderer_mark_overlay_tile(u16 tile_index) {
    const u16 word = (u16)(tile_index >> 5);
    const u32 mask = (u32)1u << (tile_index & 31);

    if ((s_current_bits[word] & mask) != 0) return;
#if DEBUG_PERF
    renderer_perf_record_overlay_touch((bool)((s_previous_bits[word] & mask) != 0));
#endif
    if (s_snapshot_slot_by_tile[tile_index] < 0) {
        if (s_snapshot_count >= OVERLAY_SNAPSHOT_TILE_LIMIT) {
            // The current frame is already correct. Request a full base rebuild
            // next time instead of reserving a 300-tile shadow framebuffer.
            s_snapshot_overflow = TRUE;
        } else {
            const u16 slot = s_snapshot_count++;
            s_snapshot_slot_by_tile[tile_index] = (s16)slot;
            for (u16 row = 0; row < 8; row++)
                s_snapshot_rows[slot][row] = g_view_tiles[tile_index][row];
        }
    }
    // restore_previous() cleaned old contamination; a newly touched tile was
    // already pristine, so no second snapshot-to-view copy is necessary.
    s_current_bits[word] |= mask;
    renderer_mark_tile_dirty(tile_index);
}
