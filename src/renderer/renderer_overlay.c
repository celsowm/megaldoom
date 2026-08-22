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
// (view_tile_index(x, y) == x * VIEW_TILE_H + y, renderer_internal.h). The
// arithmetic form is `tile_index / VIEW_TILE_H`; a byte table keeps it to one
// indexed load instead of the 32-bit mulu GCC emits for a divide by 15. ROM
// only (300 bytes const), so it costs nothing against the work-RAM budget.
#define OVERLAY_COL_RUN(c) \
    (c), (c), (c), (c), (c), (c), (c), (c), (c), (c), (c), (c), (c), (c), (c)
static const u8 s_tile_column[VIEW_TILE_COUNT] = {
    OVERLAY_COL_RUN(0),  OVERLAY_COL_RUN(1),  OVERLAY_COL_RUN(2),  OVERLAY_COL_RUN(3),
    OVERLAY_COL_RUN(4),  OVERLAY_COL_RUN(5),  OVERLAY_COL_RUN(6),  OVERLAY_COL_RUN(7),
    OVERLAY_COL_RUN(8),  OVERLAY_COL_RUN(9),  OVERLAY_COL_RUN(10), OVERLAY_COL_RUN(11),
    OVERLAY_COL_RUN(12), OVERLAY_COL_RUN(13), OVERLAY_COL_RUN(14), OVERLAY_COL_RUN(15),
    OVERLAY_COL_RUN(16), OVERLAY_COL_RUN(17), OVERLAY_COL_RUN(18), OVERLAY_COL_RUN(19),
};
#undef OVERLAY_COL_RUN
// The literal table above bakes VIEW_TILE_H == 15 (run length) and
// VIEW_TILE_W == 20 (run count); the column mask additionally needs W <= 32.
typedef char overlay_tile_column_shape_check[
    (VIEW_TILE_H == 15 && VIEW_TILE_W == 20) ? 1 : -1];
#if DEBUG_PERF
/* The asm/C canary harness and perf mailboxes consume several KB of work RAM.
 * A smaller debug-only snapshot cache keeps enough heap for frontend DMA; its
 * existing overflow path requests a correct full base rebuild. */
#define OVERLAY_SNAPSHOT_TILE_LIMIT 64
#else
#define OVERLAY_SNAPSHOT_TILE_LIMIT 128
#endif
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
    s_cur_overlay_columns |= (u32)1u << s_tile_column[tile_index];

#if CADENCE_STAGE_PROBE
    g_cadence_bb_marks++;
#endif
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
