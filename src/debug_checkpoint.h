#ifndef MEGALDOOM_DEBUG_CHECKPOINT_H
#define MEGALDOOM_DEBUG_CHECKPOINT_H

#include <genesis.h>

#if DEBUG_BLASTEM_CHECKPOINT
#define DEBUG_CHECKPOINT_TITLE    0x01
#define DEBUG_CHECKPOINT_MENU     0x02
#define DEBUG_CHECKPOINT_GAMEPLAY 0x04
#define DEBUG_CHECKPOINT_MOVED    0x08
#define DEBUG_CHECKPOINT_COMBAT   0x10

/* BlastEm's deterministic-route runner reads this single byte through
 * --md-mailbox to confirm a route actually reached the state it claims to
 * exercise, instead of trusting frame counts alone. Bits accumulate within a
 * run (the runner ORs every frame's value); debug_checkpoint_reset clears
 * them at the start of a fresh title->gameplay cycle so a stale bit from a
 * previous run doesn't mask a route that never got there this time. */
void debug_checkpoint_reset(void);
void debug_checkpoint_mark(u8 bits);

/* A byte-for-byte copy of RendererPerfSnapshot (src/renderer_perf.h),
 * published for BlastEm's --md-perf-mailbox to read. This does not live as
 * its own global in renderer_perf.c: under -O3 -flto, GCC's whole-program
 * analysis inlines every renderer_perf.c setter at its call site and, in
 * testing, dropped the struct's symbol from the final build entirely --
 * even declared non-static, volatile, and marked
 * __attribute__((used, externally_visible)). g_debug_checkpoint_state above
 * survives the same build unaffected, so routing the copy through this file
 * sidesteps the problem empirically rather than explains it; see
 * tools/resolve-symbol.py for how the runner locates the result. */
#define DEBUG_CHECKPOINT_PERF_MAILBOX_BYTES 256
void debug_checkpoint_publish_perf(const void *snapshot, u16 bytes);

/* Release-cadence stage probe: coarse per-stage subtick accumulators for
 * checkpoint builds WITHOUT DEBUG_PERF. DEBUG_PERF's per-sample getSubTick
 * hooks distort the frame so badly that its stage shares can't be projected
 * onto release; these are written at the same top-level stage boundaries but
 * cost only ~6 getSubTick calls per frame, so the measured cadence stays
 * representative. Accumulated across the run, published in CadenceSnapshot
 * (src/main.c), decoded by tools/decode-cadence.py. */
#ifndef DEBUG_PERF
#define DEBUG_PERF 0
#endif
#if !DEBUG_PERF
#define CADENCE_STAGE_PROBE 1
extern u32 g_cadence_cast_subticks;
extern u32 g_cadence_pack_subticks;
extern u32 g_cadence_projection_subticks;
extern u32 g_cadence_billboard_subticks;
extern u32 g_cadence_rebuild_frames;
/* Cast-stage unit counters (increments only, no getSubTick): how many BSP
 * nodes/box projections/segs a release-speed cast actually touches, to
 * attribute the cast cost between traversal overhead and sample work. */
extern u32 g_cadence_nodes_visited;
extern u32 g_cadence_boxes_projected;
extern u32 g_cadence_segs_tested;
extern u32 g_cadence_segs_drawn;
#else
#define CADENCE_STAGE_PROBE 0
#endif
#else
#define debug_checkpoint_reset()
#define debug_checkpoint_mark(bits)
#define debug_checkpoint_publish_perf(snapshot, bytes)
#define CADENCE_STAGE_PROBE 0
#endif

#endif
