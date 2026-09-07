#include "renderer_internal.h"
#include "renderer_perf.h"
#include "debug_checkpoint.h"

static u32 s_previous_bits[VIEW_DIRTY_WORD_COUNT];
static u32 s_current_bits[VIEW_DIRTY_WORD_COUNT];
// Bit set of tile columns (0..VIEW_TILE_W-1) whose tiles a restorable overlay
// (billboard / damage-flash) baked into g_view_tiles. build_bsp_tilemap must
// force-repack any column that carried such an overlay LAST frame; otherwise a
// coherence-skipped column keeps stale overlay pixels once the overlay moves,
// because the rebuild path erases old overlays only by re-packing the base (it
// never runs restore_previous). Weapon art lives on a separate BG_A layer and
// never enters g_view_tiles, so it is deliberately not tracked here.
static u32 s_prev_overlay_columns;
static u32 s_cur_overlay_columns;
// tile index -> screen tile column, for the COLUMN-major g_view_tiles layout
// (view_tile_index(x, y) == x * VIEW_TILE_STRIDE + y, renderer_internal.h).
//
// This used to be a 300-byte const table of 15-long runs, because the
// arithmetic form was `tile_index / 15` and GCC turns a divide by 15 into a
// 32-bit mulu. The stride is now the power-of-two RAY_VIEW_TILE_H_MAX, so the
// same mapping is a single shift -- cheaper than the indexed load the table
// was bought for, and it no longer bakes one viewport shape into the file.
#define OVERLAY_TILE_COLUMN(tile_index) ((tile_index) / VIEW_TILE_STRIDE)
// s_cur_overlay_columns / s_prev_overlay_columns are a u32 bit per tile column.
_Static_assert(RAY_VIEW_TILE_W_MAX <= 32,
               "overlay column mask is a u32; RAY_VIEW_TILE_W_MAX must fit it");
#if DEBUG_PERF
/* The asm/C canary harness and perf mailboxes consume several KB of work RAM.
 * A smaller debug-only snapshot cache keeps enough heap for frontend DMA; its
 * existing overflow path requests a correct full base rebuild. */
#define OVERLAY_SNAPSHOT_TILE_LIMIT 64
#else
#define OVERLAY_SNAPSHOT_TILE_LIMIT 128
#endif
static u32 s_snapshot_rows[OVERLAY_SNAPSHOT_TILE_LIMIT][8];
// One byte per tile, not one word: the pool is OVERLAY_SNAPSHOT_TILE_LIMIT
// entries (128 at most), so a slot index fits a u8 and 0xFF is free to mean
// "no snapshot". At VIEW_TILE_ALLOC that halves the table, which is work RAM
// the larger viewport sizes need.
#define OVERLAY_SNAPSHOT_NO_SLOT 0xFFu
_Static_assert(OVERLAY_SNAPSHOT_TILE_LIMIT < OVERLAY_SNAPSHOT_NO_SLOT,
               "snapshot slot index must fit a u8 alongside the empty marker");
static u8 s_snapshot_slot_by_tile[VIEW_TILE_ALLOC];
static u16 s_snapshot_count;
static bool s_snapshot_overflow;

static void clear_snapshot_cache(void) {
    // Walks the ALLOCATION rather than the current size: shrinking the viewport
    // must not leave a stale slot behind on a tile that has left the view and
    // could come back when the player picks a larger size again.
    for (u16 tile = 0; tile < VIEW_TILE_ALLOC; tile++) {
        s_snapshot_slot_by_tile[tile] = OVERLAY_SNAPSHOT_NO_SLOT;
    }
    s_snapshot_count = 0;
    s_snapshot_overflow = FALSE;
}

void renderer_overlay_reset(void) {
    for (u16 i = 0; i < VIEW_DIRTY_WORD_COUNT; i++) {
        s_previous_bits[i] = 0;
        s_current_bits[i] = 0;
    }
    s_prev_overlay_columns = 0;
    s_cur_overlay_columns = 0;
    clear_snapshot_cache();
}

// Columns that a restorable overlay baked into g_view_tiles on the last
// completed frame. build_bsp_tilemap() consults this before coherence-skipping.
u32 renderer_overlay_prev_columns(void) {
    return s_prev_overlay_columns;
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

        const u8 slot = s_snapshot_slot_by_tile[tile];
        if (slot == OVERLAY_SNAPSHOT_NO_SLOT) continue;
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
    s_cur_overlay_columns = 0;
}

void renderer_overlay_finish(void) {
    for (u16 i = 0; i < VIEW_DIRTY_WORD_COUNT; i++) {
        s_previous_bits[i] = s_current_bits[i];
    }
    s_prev_overlay_columns = s_cur_overlay_columns;
}

bool renderer_overlay_requires_base_rebuild(void) {
    return s_snapshot_overflow;
}

void renderer_mark_overlay_tile(u16 tile_index) {
    const u16 word = (u16)(tile_index >> 5);
    const u32 mask = (u32)1u << (tile_index & 31);

    // A repeat touch cannot add a column: the first touch of this tile already
    // set its bit this frame. So the column mask is maintained past the
    // early-out, keeping it off the per-pixel-run repeat path.
    if ((s_current_bits[word] & mask) != 0) return;
    s_cur_overlay_columns |= (u32)1u << OVERLAY_TILE_COLUMN(tile_index);

#if CADENCE_STAGE_PROBE
    g_cadence_bb_marks++;
#endif
#if DEBUG_PERF
    renderer_perf_record_overlay_touch((bool)((s_previous_bits[word] & mask) != 0));
#endif
    if (s_snapshot_slot_by_tile[tile_index] == OVERLAY_SNAPSHOT_NO_SLOT) {
        if (s_snapshot_count >= OVERLAY_SNAPSHOT_TILE_LIMIT) {
            // The current frame is already correct. Request a full base rebuild
            // next time instead of reserving a 300-tile shadow framebuffer.
            s_snapshot_overflow = TRUE;
        } else {
            const u16 slot = s_snapshot_count++;
            s_snapshot_slot_by_tile[tile_index] = (u8)slot;
            for (u16 row = 0; row < 8; row++)
                s_snapshot_rows[slot][row] = g_view_tiles[tile_index][row];
        }
    }
    // restore_previous() cleaned old contamination; a newly touched tile was
    // already pristine, so no second snapshot-to-view copy is necessary.
    s_current_bits[word] |= mask;
    renderer_mark_tile_dirty(tile_index);
}
