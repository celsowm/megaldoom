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
#else
#define debug_checkpoint_reset()
#define debug_checkpoint_mark(bits)
#define debug_checkpoint_publish_perf(snapshot, bytes)
#endif

#endif
