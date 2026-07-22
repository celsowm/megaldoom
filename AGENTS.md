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
segment) — it is the **worst case for temporal-coherence-based optimizations**.

`tools/routes/tour-east-combat.txt` (7335 frames, run with `-b 7400`) is the
first route that actually leaves the start room: through the NE corridor, C-opens
the group-0 door at x=1536, fights past enemies into the zigzag area, ending at
(2416, 3081) angle 106 (verify via the `g_player` mailbox). Its scenes are
richer than the three start-room routes: at release speed (cadence probe,
2026-07-21) pack averages **5.58 vblanks/rebuild vs 3.0 in the start room** —
the canned routes understate pack cost by ~2x. Max frame observed: 34 vblanks.
Chained turn legs drift (turn ramp scales with elapsed_frames, which varies
with scene cost), so appending legs needs pose re-verification after EVERY leg;
do not trust dead-reckoned headings more than ~2 legs deep.

## Real release framerate (measured 2026-07-21, cadence probe)

A checkpoint-only build (no -DebugPerf) publishes a `CadenceSnapshot`
(decode with `tools/decode-cadence.py`) — this is the ground truth, and it is
very different from what the DEBUG_PERF builds imply:

- **Idle frames hit the 2-vblank (30fps) target.**
- **Motion/rebuild frames cost ~12-13 vblanks (~5fps)**: cast 4.4-4.6 vb,
  pack 3.0 (start room) to 5.6 (tour route), projection ~1.25, billboard
  0.3-0.7, plus the fixed 2-vblank 300-tile upload.
- Cast's counted workload is tiny (~50 nodes, ~55 box projections, ~30 segs
  tested, 11-16 drawn per rebuild) — per-unit constant factors dominate.
- Projection re-measures all ~58 active billboards whenever the camera pose
  changes (the measure cache keys on exact pose, so it misses 100% in motion).

Consequence: DMA-side levers (sparse FB, partial uploads) can recover at most
~2 of ~12 vblanks; the frame is CPU-bound in cast+pack. Smoothness work should
target those two stages or frame-pacing, not upload bytes.

### Background upload pump (shipped 2026-07-21)

Those ~2 upload vblanks are now overlapped instead of spent: a V-INT callback
(`renderer_upload_background_pump`, armed ONLY around `bsp_cast_frame` in
`render_current_view`) DMAs the previous frame's queued upload during the
vblanks that fire inside the next frame's cast, and the pacing loop no longer
blocks on upload completion (non-DEBUG_PERF builds). The cast is the one safe
window: it is pure CPU (writes `g_ray_columns` only — no VDP access, no
`g_view_tiles` writes), so the interrupt can own the VDP without racing the
main loop. `wait_scene_upload_complete` in main.c drains any remainder before
anything writes `g_view_tiles` again; `upload_state_invalidate` now drops a
surviving in-flight upload (pause menu / level reset can interrupt one).

Measured on `checkpoints.txt` (same-pose-verified, cadence probe): motion
frames 12.05 -> 10.7 vblanks, route average 5.35 -> 4.67 vblanks (+13%
throughput). Only ~1.3-1.9 of the 2 vblanks come back because ~0.6 vb is real
DMA bus occupancy (the 68k is halted during the transfer) — that time now
lands INSIDE the cast subtick numbers, so expect cast to read ~0.5-0.7 vb
higher on motion frames post-change (4.4 -> ~4.9); it is not a cast
regression. DEBUG_PERF builds keep the old serial pacing loop and never arm
the pump (its per-run perf counters would race their main-thread readers), so
perf-overlay captures stay comparable with history but do NOT show the
overlap win — use the cadence probe for that.

### Cast internals split + box projection rewrite (2026-07-21)

DEBUG_PERF's deep-phase slots (`deep_subticks` in the RendererPerfSnapshot —
decode-perf-full.py parses but does not print them; snapshot must land
mid-motion or the BSP slots read stale idle zeros) split the cast roughly
50/50: **draw_seg ~95 subticks x ~31 calls, project_box_range ~40 subticks x
~64 calls, side-cache negligible** (shares only — the getSubTick bracket pair
inflates per-unit values). Traversal overhead outside those two is nil.

`project_box_range` was then rewritten (commit after 2d2e64b): corner extrema
via the exact monotonic-shift axis decomposition (floor of the min IS the min
of the floors because each corner is base + an independent subset of the two
Q8 extent terms), cheap half-plane reject axis-decomposed in the shifted
domain with a +-2-per-corner floor slack, corners assembled only in the rare
near-clip branch. `tools/test-bsp-render-math.py` now proves both properties
per box (extrema equality + "new cheap-reject implies old cheap-reject") over
all 604160 boxes. Result: cast -295 subticks/rebuild (~-5%), motion frames
10.7 -> 10.45 vb, zero traversal-counter drift.

### draw_seg release-speed split + strength reductions (2026-07-22)

The cadence probe grew a draw_seg split (build with
`-DCADENCE_DRAWSEG_SPLIT=1` on top of the checkpoint flags — OFF by default
because its getSubTick brackets add ~0.3 vb inside cast_subticks; the
per-sample counter `samples drawn` is free and always on). Measured at
release speed on checkpoints: **the per-sample fill loop is the MAJORITY of
draw_seg — ~1840 subticks/rebuild (~20 subticks x 80 samples) vs ~1180
fixed (~37/seg x 32 segs)**. The old "cast cost is per-unit constants, NOT
the sample loop" note came from DEBUG_PERF-distorted data and is wrong at
release speed.

Shipped byte-identical strength reductions (proof comments at each site):
sfix and scaled_u each collapse the 32x16 helper to a single MULU.W (their
operands provably fit u16), and reciprocal_depth reuses the existing
g_bsp_inv_depth_lut for depth <= 1024 (the table IS divu(16384, d)).
Cast 5927 -> 5755 subticks/rebuild; motion frames 10.45 -> ~10.26 vb.

What remains per sample (~18 subticks): the uz 32x16 interpolation, the
DIVS.W for u_col, the invz interpolation, height mul, RayColumn writes, and
find_next_open. Going lower means arithmetic-CHANGING work (forward
differencing / affine texture spans / LUT-multiply u_col) — all of it drifts
texels by +-1 and breaks byte-identity with the C reference, so it needs the
same eyeball + differential-model treatment the packer got, not a quick win.

### RAY_COL_STRIDE 4 prototype: measured, awaiting user eyeball (2026-07-22)

`RAY_COL_STRIDE` is now `#ifndef`-guarded in raycast.h, so
`EXTRA_FLAGS="-DRAY_COL_STRIDE=4"` builds a 40-column prototype without
touching the shipped stride-2 default (guardrails and the wall-quality
assertion still see `#define RAY_COL_STRIDE 2`). The stride-4 packer branch
in renderer_pack.c had bit-rotted against a generated array that no longer
exists (`FREEDOOM_WALL_PACKED_COLUMNS`); it now reuses the stride-2
`FREEDOOM_WALL_PACKED_PAIRS` table (`pair * 0x0101` spreads each 2px byte to
4px). Note the stride-4 packer is the old C implementation: no coherence
cache, no asm hotpath — its pack numbers have headroom if stride 4 ever
ships.

Same-pose A/B on checkpoints (x/angle equal, y off by 12 units — cadence
drift): per-rebuild stage sum 11614 -> 9621 subticks (9.07 -> 7.51 vb, about
-1.75 vb/motion frame with logic): cast 5755 -> 4781, pack 3761 -> 2839,
samples 80 -> 40. This fresh baseline also directly confirms the previously
extrapolated 5755-subtick cast figure. The tour-east-combat A/B DIVERGED
(different end poses — frame-timed inputs + changed cadence = different
trajectory), so its numbers are not comparable; only checkpoints is a valid
A/B. Screenshots confirm correct rendering (perspective/shading/textures
fine, columns visibly 4px chunky). Whether the resolution loss is acceptable
is the user's call — do not flip the default without their sign-off.

### Billboard projection spatial pre-cull: tried and rejected (2026-07-21)

A Chebyshev distance pre-cull (2 x BILLBOARD_MAX_DEPTH radius, two subtracts
+ compares before billboard_measure_cached) measured **slower** on the
same-pose checkpoints route: projection 1598 -> 1685 subticks/rebuild. E1M1's
active objects cluster inside the radius near the routes, so it culled almost
nothing while adding per-object overhead — the depth-reject path inside
billboard_measure_object it tried to bypass was never the cost. Projection's
~1600 subticks are the genuinely-near objects being fully re-measured; do not
re-attempt distance pre-culls without a route where objects are provably far. `average_vblanks_x10` is a 60-frame
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

| Route | dyn wall | overlay | est DMA bytes (incl. 600B tilemap) | verdict |
|-------|----------|---------|----------------------------------------|---------|
| `checkpoints.txt` (movement) | 10.5 / 64 | 64.5 / 75 | 3000 / 3320 | GO |
| `stationary-combat.txt` | 5.3 / 64 | 68.8 / 75 | 2971 / 3000 | GO |
| `slow-turn.txt` | 13.3 / 64 | 73.6 / 105 | 3381 / 4504 | GO (prototype) |

**Budget math (corrected):** `est_dma_bytes = (dyn_wall + overlay) * 32 + 600`.
Nominal VBlank ≈ 4800 B = 150 tiles * 32. Reserve 600 B for the mixed
tilemap commit → 4200 B remaining = 131 tiles. The **conservative safe gate
is 120 tiles (3840 B)** to leave headroom for per-run DMA command overhead,
HUD, and other VBlank work. Peak dynamic tiles = (max_dma − 600) / 32:
- movement peak: (3320 − 600)/32 = **85 tiles** → well under 120
- stationary peak: (3000 − 600)/32 = **75 tiles** → well under 120
- slow-turn peak: (4504 − 600)/32 = **122 tiles** → **over the 120 safe
  gate by 2 tiles / 64 B, but still under the 131-tile / 4504-B nominal
  budget (≈ 4800 − 4504 = 296 B, ~6% margin)**

So: average routes are comfortably within budget; **slow-turn's maximum (4504 B)
is close to the nominal 4800-B VBlank budget and over the 120-tile safe gate**.
That makes it a **GO for a prototype**, not "well under" — implementation
requires real DMA timing, run-count measurement, and overflow handling.

**Only ~5–13% of the changed-column tiles are wall** — the rest is
ceiling/floor served statically. Overlay (billboard copy-on-write) is the
dominant dynamic cost (~65–75 tiles, i.e. ~4–5 columns' worth), not the
wall: e.g. movement = 10.5 wall vs 64.5 overlay; slow-turn = 13.3 wall
vs 73.6 overlay. So the sparse architecture should be designed primarily
around **billboard copy-on-write**, not just walls. The double-buffer-per-
column hypothesis was the wrong lever; the sparse semantic framebuffer is the
right next architecture.

Caveat: this oracle counts tiles the **packer** emits. It does NOT yet measure
the actual sparse *upload* (one DMA run per wall column + the 600-B tilemap
commit + billboard COW tiles). The 120-tile gate is a DMA-byte estimate; the
true budget also pays per-run DMA command overhead, HUD, and other VBlank
work, so re-measure against the real upload path before declaring 60fps.
Also: `sparse_overlay_max` already hits 105 (slow-turn) — many-on-screen
enemies is the next risk to capture, since each is a copy-on-write of a
static tile. The definitive test is not the oracle but: `average_vblanks_x10`
near 10, no increase in missed deadlines, no tearing, no visual regression.

**Ceiling/floor cost ZERO DMA during motion — do NOT add sector-transition
rebake.** The current renderer picks ONE `RayFlatColor` ceiling from the
player's sector and applies it to the whole viewport; distant sectors add no
ceiling. The ceiling Bayer pattern repeats every 4px and tiles are 8px-aligned,
so every full ceiling tile of a given color is byte-identical → one atlas tile
per distinct `(primary, secondary, secondary_coverage)` key, generated once
(preferably offline beside `FREEDOOM_SECTOR_VISUALS`) and uploaded once at
level init, then selected by `g_sector_ceiling_tile[current_sector]` at
runtime. Floor is ROM-constant → one tile forever. Crossing into a sector with
a different ceiling costs no DMA — it just points those cells at the already-
resident atlas tile. So the real per-frame dynamic DMA is **only walls + doors
+ overlay COW**; the 600-B tilemap commit is the only constant. If you find
yourself writing per-sector ceiling rebake/upload code, you have misunderstood
this — revisit it before continuing.

**Before building the sparse architecture:** the sparse oracle is DEBUG_PERF-
only and free to leave in. Do NOT re-attempt per-column double-buffer — its
NO-GO is documented above and the sparse path supersedes it. Build the sparse
path behind a compile-time flag, keep the full positional `g_view_tiles`
framebuffer in RAM at first (reduce DMA only; don't mix in a CPU/raster
change simultaneously), and re-measure real DMA timing/run-count before
removing the existing uploader.
