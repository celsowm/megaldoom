#include "debug_checkpoint.h"
#include <string.h>

#if DEBUG_BLASTEM_CHECKPOINT
/* Non-static and volatile: the BlastEm runner locates this byte by name in
 * the build's symbol table (see tools/resolve-symbol.py) and reads it
 * through --md-mailbox every frame, so its address must be stable and its
 * value must not be optimized into a register. */
volatile u8 g_debug_checkpoint_state = 0;
volatile u8 g_debug_perf_mailbox[DEBUG_CHECKPOINT_PERF_MAILBOX_BYTES];

#if DEBUG_E2E_ACTIVE
/* Kept in this translation unit for the same LTO-survival reason as the
 * checkpoint byte.  The host reads all sixteen bytes through --md-mailbox. */
volatile DebugE2EState g_debug_e2e_state;

void debug_e2e_level_start(u16 level) {
    g_debug_e2e_state.events = DEBUG_E2E_EVENT_STARTED;
    g_debug_e2e_state.keys_collected = 0;
    g_debug_e2e_state.keys_locked = 0;
    g_debug_e2e_state.keys_unlocked = 0;
    g_debug_e2e_state.start_level = (u8)level;
    g_debug_e2e_state.exit_level = 0xFF;
    g_debug_e2e_state.god_enabled = DEBUG_E2E_GOD ? 1 : 0;
    g_debug_e2e_state.god_hits = 0;
    g_debug_e2e_state.deaths = 0;
    g_debug_e2e_state.use_serial = 0;
    g_debug_e2e_state.use_action = 0;
    g_debug_e2e_state.use_target = 0xFF;
    g_debug_e2e_state.use_key = 0;
    g_debug_e2e_state.player_x = 0;
    g_debug_e2e_state.player_y = 0;
    g_debug_e2e_state.player_angle = 0;
}

void debug_e2e_mark(u8 events) { g_debug_e2e_state.events |= events; }
void debug_e2e_collect_key(u8 key_mask) {
    g_debug_e2e_state.events |= DEBUG_E2E_EVENT_KEY;
    g_debug_e2e_state.keys_collected |= key_mask;
}
void debug_e2e_locked(u8 key_mask) {
    g_debug_e2e_state.events |= DEBUG_E2E_EVENT_LOCKED;
    g_debug_e2e_state.keys_locked |= key_mask;
}
void debug_e2e_unlocked(u8 key_mask) {
    g_debug_e2e_state.events |= DEBUG_E2E_EVENT_UNLOCKED;
    g_debug_e2e_state.keys_unlocked |= key_mask;
}
void debug_e2e_exit(u16 level) {
    g_debug_e2e_state.events |= DEBUG_E2E_EVENT_EXIT;
    g_debug_e2e_state.exit_level = (u8)level;
}
void debug_e2e_god_hit(void) {
    if (g_debug_e2e_state.god_hits != 0xFF) g_debug_e2e_state.god_hits++;
}
void debug_e2e_death(void) {
    if (g_debug_e2e_state.deaths != 0xFF) g_debug_e2e_state.deaths++;
}
void debug_e2e_use(u8 action, u8 target, u8 required_key) {
    if (g_debug_e2e_state.use_serial != 0xFF) g_debug_e2e_state.use_serial++;
    g_debug_e2e_state.use_action = action;
    g_debug_e2e_state.use_target = target;
    g_debug_e2e_state.use_key = required_key;
}
void debug_e2e_pose(s32 x, s32 y, u16 angle) {
    g_debug_e2e_state.player_x = (s16)x;
    g_debug_e2e_state.player_y = (s16)y;
    g_debug_e2e_state.player_angle = angle;
}
#else
void debug_e2e_level_start(u16 level) { (void)level; }
void debug_e2e_mark(u8 events) { (void)events; }
void debug_e2e_collect_key(u8 key_mask) { (void)key_mask; }
void debug_e2e_locked(u8 key_mask) { (void)key_mask; }
void debug_e2e_unlocked(u8 key_mask) { (void)key_mask; }
void debug_e2e_exit(u16 level) { (void)level; }
void debug_e2e_god_hit(void) {}
void debug_e2e_death(void) {}
void debug_e2e_use(u8 action, u8 target, u8 required_key) {
    (void)action; (void)target; (void)required_key;
}
void debug_e2e_pose(s32 x, s32 y, u16 angle) {
    (void)x; (void)y; (void)angle;
}
#endif

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
u32 g_cadence_scene_frames;
u32 g_cadence_bb_objects;
u32 g_cadence_bb_rows;
u32 g_cadence_bb_bytes;
u32 g_cadence_bb_opaque;
u32 g_cadence_bb_commits;
u32 g_cadence_bb_marks;
u32 g_cadence_bb_mismatch;
u32 g_cadence_pack_columns;
u32 g_cadence_pack_flat_tiles;
u32 g_cadence_pack_mixed_tiles;
u32 g_cadence_bb_setup_subticks;
u32 g_cadence_bb_rows_subticks;
u32 g_cadence_bb_max_bytes;
u32 g_cadence_bb_max_subticks;
u32 g_cadence_pack_desc_subticks;
u32 g_cadence_pack_tiles_subticks;
#endif

void debug_checkpoint_publish_perf(const void *snapshot, u16 bytes) {
    const u16 clamped = (bytes > DEBUG_CHECKPOINT_PERF_MAILBOX_BYTES) ?
        DEBUG_CHECKPOINT_PERF_MAILBOX_BYTES : bytes;
    memcpy((void *)g_debug_perf_mailbox, snapshot, clamped);
}
#endif
