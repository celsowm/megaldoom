#include "debug_checkpoint.h"
#include <string.h>

#if DEBUG_BLASTEM_CHECKPOINT
/* Non-static and volatile: the BlastEm runner locates this byte by name in
 * the build's symbol table (see tools/resolve-symbol.py) and reads it
 * through --md-mailbox every frame, so its address must be stable and its
 * value must not be optimized into a register. */
volatile u8 g_debug_checkpoint_state = 0;
volatile u8 g_debug_perf_mailbox[DEBUG_CHECKPOINT_PERF_MAILBOX_BYTES];

void debug_checkpoint_reset(void) {
    g_debug_checkpoint_state = 0;
}

void debug_checkpoint_mark(u8 bits) {
    g_debug_checkpoint_state |= bits;
}

#if CADENCE_STAGE_PROBE
u32 g_cadence_cast_subticks;
u32 g_cadence_pack_subticks;
u32 g_cadence_projection_subticks;
u32 g_cadence_billboard_subticks;
u32 g_cadence_rebuild_frames;
u32 g_cadence_nodes_visited;
u32 g_cadence_boxes_projected;
u32 g_cadence_segs_tested;
u32 g_cadence_segs_drawn;
u32 g_cadence_drawseg_subticks;
u32 g_cadence_sample_subticks;
u32 g_cadence_samples;
u32 g_cadence_box_calls;
u32 g_cadence_box_near_path;
u32 g_cadence_box_cheap_reject;
u32 g_cadence_box_early_out;
u32 g_cadence_box_subticks;
u32 g_cadence_range_closed_calls;
u32 g_cadence_range_closed_subticks;
u32 g_cadence_all_closed_subticks;
#endif

void debug_checkpoint_publish_perf(const void *snapshot, u16 bytes) {
    const u16 clamped = (bytes > DEBUG_CHECKPOINT_PERF_MAILBOX_BYTES) ?
        DEBUG_CHECKPOINT_PERF_MAILBOX_BYTES : bytes;
    memcpy((void *)g_debug_perf_mailbox, snapshot, clamped);
}
#endif
