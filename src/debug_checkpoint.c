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

void debug_checkpoint_publish_perf(const void *snapshot, u16 bytes) {
    const u16 clamped = (bytes > DEBUG_CHECKPOINT_PERF_MAILBOX_BYTES) ?
        DEBUG_CHECKPOINT_PERF_MAILBOX_BYTES : bytes;
    memcpy((void *)g_debug_perf_mailbox, snapshot, clamped);
}
#endif
