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

## Column-major view buffer + per-column upload oracle (2026-07-19)

`g_view_tiles` is now **column-major**: screen `(tile_x, tile_y)` maps to slot
`tile_x * VIEW_TILE_H + tile_y` (`view_tile_index` in `renderer_internal.h`),
so a screen column's 15 tiles are contiguous and *could* ship as one DMA run.
The tilemap array stays row-major (VDP scans screen rows) but each entry now
points at the column-major VRAM slot (`build_view_bank_tilemaps` in
`renderer.c`). This was a behavior-preserving refactor: `asm_mismatches=0` on
every route, pixels byte-identical.

**Watch out — the asm-verify probe was silently dead under the old row-major
layout.** `compare_stride2_tile_asm` only runs when
`tile_index == g_asm_compare_cursor`, and the cursor walks `0,1,2,…`. Row-major
emitted tiles in `0,20,40,…` order, so the gate almost never matched and the
per-tile pixel verify never actually ran. Column-major emits `0,1,2,…`, so the
verify now runs every tile. Consequence: `average_vblanks_x10` rose 155→181 on
`checkpoints.txt` purely because the (DEBUG_PERF-only) verify is now doing real
work — **this is not a release-build cost** (the probe is `#if DEBUG_PERF`).
Don't chase that delta as a regression.

### Per-column upload: measured NO-GO on current content (do not build yet)

The point of column-major was to enable per-column DMA (upload only the
columns the coherence cache repacked, instead of the unconditional 300-tile
`renderer_prepare_full_base_upload`). Before building the uploader, a
**ColumnReuseOracle** was added (`renderer_perf_record_column_reuse`, fields
`columns_changed*` in `RendererPerfSnapshot`) to count, per base-rebuild frame,
how many of the 20 tile columns actually change. Captured across three routes:

| Route (`tools/routes/`)      | avg cols changed / 20 | avg tiles/rebuild | % of full 300-DMA |
|------------------------------|-----------------------|-------------------|-------------------|
| `checkpoints.txt` (movement) | 17.9                  | 268               | 90%               |
| `slow-turn.txt`              | 17.9                  | 269               | 90%               |
| `stationary-combat.txt`      | 13.7                  | 205               | 68%               |

Per the reuse decision bands (0–8 cols = 60fps candidate, 9–12 = partial,
13–20 = stays 30fps), **every route lands in the 13–20 "stays 30fps" band.**
Even the best case (stationary combat) still uploads 68% of the tiles, before
the mixed-tilemap commit cost is added. A per-column uploader would add
marking/commit CPU (same class of cost that sank the 2026-07-19 partial-upload
attempt above) for a DMA saving too small to remove a whole vblank. **Deferred
until content changes** (larger view, or a route where the player is genuinely
stationary for long stretches) push average changed columns under ~10.

Why so few columns are reused even when "stationary": the coherence cache
invalidates a whole tile column if *any* of its 4 sampled `WallColumnDescriptor`
fields differ, and combat/animation perturbs `top`/`bottom`/`tex_x` by a pixel
across most columns. Quantizing presentation (plan §14, `top & ~1`) would
raise reuse but introduces wall-jitter — measure exact reuse first (the oracle
is still in place; just capture a new route). The oracle is DEBUG_PERF-only and
free to leave in.

## Sparse semantic tile oracle (2026-07-19)

The per-column oracle (above) killed the double-buffer-per-column path, but it
left the real question open: *of the 206–269 tiles in changed columns, how
many actually contain wall?* Most of a viewport is ceiling/floor, which a
sparse architecture would serve from shared static VRAM tiles (0 DMA during
movement). A second oracle (`renderer_perf_record_sparse`, fields
`sparse_*` in `RendererPerfSnapshot`) counts, per changed column, wall tiles
vs. full-ceiling/full-floor tiles vs. overlay (copy-on-write) tiles. Mailbox
raised to 256 B (`DEBUG_CHECKPOINT_PERF_MAILBOX_BYTES`) to fit the aggregates.

Captured across the three routes (avg / max over gameplay rebuild frames):

| Route | dyn wall | overlay | est DMA bytes | verdict |
|-------|----------|---------|----------------|---------|
| `checkpoints.txt` (movement) | 10.5 / 64 | 64.5 / 75 | 3000 / 3320 | GO |
| `stationary-combat.txt` | 5.3 / 64 | 68.8 / 75 | 2971 / 3000 | GO |
| `slow-turn.txt` | 13.3 / 64 | 73.6 / 105 | 3381 / 4504 | GO |

Gate (from plan §15, 120 dyn tiles / ~4200 B safe): every route is far
under. **Only ~5–13% of the changed-column tiles are wall** — the rest is
ceiling/floor served statically. Overlay (billboard copy-on-write) is now the
dominant dynamic cost (~65–75 tiles, i.e. ~4–5 columns' worth), not the
wall. So the sparse semantic framebuffer is the right next architecture; the
double-buffer-per-column hypothesis was the wrong lever.

Caveat: this oracle counts tiles the **packer** emits. It does NOT yet measure
the actual sparse *upload* (one DMA run per wall column + the 600-B tilemap
commit). The 120-tile gate is a DMA-byte estimate (dyn_wall*32 + overlay*32
+ 600); the true budget also pays per-run DMA command overhead, HUD, and other
VBlank work, so re-measure against the real upload path before declaring 60fps.
Also: `sparse_overlay_max` already hits 105 (slow-turn) — many-on-screen
enemies is the next risk to capture, since each is a copy-on-write of a
static tile.

**Before building the sparse architecture:** the sparse oracle is DEBUG_PERF-
only and free to leave in. Use it to re-measure after any content change
(larger view, more enemies). Do NOT re-attempt per-column double-buffer — its
NO-GO is documented above and the sparse path supersedes it.
