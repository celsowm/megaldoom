# AGENTS.md

Repo-wide notes for AI coding agents working on this codebase. Read this before
touching the renderer's performance-sensitive paths — it records dead ends
that were already tried and measured, so they aren't silently re-attempted.

## How to measure renderer frame cost (headless, no BlastEm UI needed)

1. Build with perf instrumentation and the deterministic-route checkpoint hook:
   ```
   $env:EXTRA_FLAGS="-DDEBUG_BLASTEM_CHECKPOINT=1"; .\tools\build-windows.ps1 -DebugPerf
   ```
2. Resolve the perf mailbox symbol:
   ```
   python tools/resolve-symbol.py out/symbol.txt g_debug_perf_mailbox --bytes 128
   ```
3. Run the deterministic route through the custom BlastEm build and capture a report:
   ```
   .externals/blastem/build/windows/blastem.exe -b 600 out/rom.bin \
     --md-route tools/routes/checkpoints.txt --md-report out/perf.json \
     --md-perf-mailbox <ADDR>:128
   ```
   `-b <frames>` is required or BlastEm never exits and no report is written.
4. Decode the `perfMailbox` hex blob as `RendererPerfSnapshot` (see
   `src/renderer_perf.h`) using **m68000 struct alignment** (u16/u32 align to
   2 bytes, bool is 1 byte) — a plain-C decode gets the field offsets wrong.

`tools/routes/checkpoints.txt` ends in sustained movement (no combat/rotation
segment) — it is the **worst case for temporal-coherence-based optimizations**
and currently the *only* captured route. `average_vblanks_x10` is a 60-frame
rolling average (see `renderer_debug_set_total_vblanks` in `renderer_perf.c`),
not a single-frame snapshot, so it's a reasonably trustworthy signal even
though it comes from one route.

Frame time is quantized to whole vblanks via `VDP_waitVSync` — a CPU-side
saving only matters if it's large enough to remove a whole vblank from the
sampled window. Small percentage wins in one stage's subticks routinely show
up as **zero** change in `average_vblanks_x10` / `missed_deadlines`.

## Renderer perf: reverted attempts (don't redo without new evidence)

### Partial base-bank upload (attempted and reverted 2026-07-19)

**The idea:** every base-rebuild frame forces a full 300-tile DMA upload to
the inactive VRAM bank (`renderer_prepare_full_base_upload` in `renderer.c`),
even when the Pack-stage coherence cache (`build_bsp_tilemap`'s per-column
skip logic) only actually changed a handful of tiles. The fix explored: make
both VRAM banks' dirty-bit sets accumulate continuously (delete
`g_view_dirty_bank_mask`, mark both banks unconditionally on every write via
`renderer_mark_tile_dirty`), mark whole repacked columns dirty from
`build_bsp_tilemap`, and let the existing `choose_full_view_upload` threshold
heuristic (used today only by the overlay-only path) decide full-vs-partial
on rebuild frames too. This elegantly solves "how stale is bank N" for free —
no N-frame history needed, since continuous marking means whichever bank is
targeted already holds the exact accumulated diff since it was last synced.

**Why it was reverted:** it made `average_vblanks_x10` **worse** (155→164 on
`checkpoints.txt`), not better. The design only priced in DMA savings and
assumed marking was free — it isn't. Marking added real Pack-stage CPU cost
(`pack_subticks` 5385→6645, +23%), and collapsing the per-tile marking into
one call per column (`renderer_mark_tile_column_dirty` in `renderer.c`
instead of 15 cross-TU calls) only recovered a small fraction of that
(6645→6485) — the remaining cost is inherent to the bit-tracking work, not
call overhead. On `checkpoints.txt` (pure movement, worst case for the
coherence cache — see above), almost every column is repacked, so nearly
every column pays the new marking cost, *and* the accumulated dirty count
still crosses `VIEW_DIRTY_FULL_THRESHOLD` (220) and triggers a full upload
anyway. Paying marking cost for zero DMA saving is a straight loss, and it
was enough to tip frames into an extra vblank.

Correctness was never in question (`asm_mismatches=0`, all structure tests
passed, RAM budget improved slightly from the net code deletion) — this was
purely a perf regression, caught by measuring before declaring victory.

**Before retrying this lever:**
- It may still net-win during stationary/rotating combat, where the
  coherence cache actually skips most columns (few tiles marked, upload
  genuinely partial). `checkpoints.txt` cannot test that — it has no
  stationary/rotating segment. Build a second route that captures one before
  spending more time here.
- If a stationary-route capture *still* shows marking cost exceeding DMA
  savings, tile-level dirty tracking (300 bits) is too fine-grained for this
  CPU. Try column-level dirty bits (20 bits) instead, accepting coarser DMA
  run granularity in exchange for ~15x fewer bit-set operations per repacked
  column.
- Don't just re-run the same tile-level design expecting a different result
  on the same route — the two capture-and-revert cycles already did that.
