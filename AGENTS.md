# AGENTS.md

Repo-wide notes for AI coding agents working on this codebase. This file is
**durable guidance**: how to measure, what the invariants are, and what not to
do. It should stay roughly this size.

Dated investigations — what was tried on a given day, with the before/after
tables and the reasoning — live in [LOG.md](LOG.md). Put new findings there and
leave only the standing rule here. If a LOG entry has no consequence for future
work, it does not need a line in this file.

## Standing rules

**Verify assembly against C, and prove the check can fail.** Two DEBUG_PERF
harnesses cover `src/renderer/renderer_hotpath.s`, and both run the asm and a C
reference into canary-guarded scratch and compare: `renderer_pack.c`'s
`compare_stride2_column_asm` for the mixed-tile span, and `renderer_doors.c`'s
`compare_overlay_posts_asm` for the two door/window overlay posts. They feed one
set of counters — read back `asm_mismatches`, `asm_canary_failures` and
`asm_checked_tiles` after any change to the asm, and isolate which side a
mismatch came from with `-DRENDERER_OVERLAY_C_REFERENCE=1` or
`-DRENDERER_HOTPATH_C_REFERENCE=1`. If you touch a harness, also inject a
deliberate fault and confirm it reports non-zero before reverting — the first
one spent months silently comparing asm against asm and reporting success (LOG,
2026-08-04). Keep both sides computed inside the harness, and have it replay the
production emitter rather than a copy of it; those are the two properties that
kept that from happening again (LOG, 2026-08-30).

**Not every route is valid for an A/B comparison.** Only routes that end at the
same player pose in both builds can be compared by run average; a faster build
walks somewhere else and rasterizes a different scene. `stationary-combat`,
`checkpoints` and `slow-turn` have held pose-stable; `tour-east-combat` and
`barrel-pointblank` do not. Confirm by checking that the mixed/flat tile counts
(or billboard byte counts) are identical on both sides before believing a delta.

**A route cannot hold pose; use the pose-locked harness instead.** The rule above
says to check a route's workload counters before believing a delta -- they
routinely fail that check, because the loop is vblank-paced, so a faster build
gets more iterations inside the same route frame and walks somewhere else. Build
with `EXTRA_FLAGS="-DDEBUG_BLASTEM_CHECKPOINT=1 -DPERF_FIXED_POSE=1
-DPERF_POSE_X=1300 -DPERF_POSE_Y=3300 -DPERF_POSE_ANGLE=0"` and run
`tools/routes/fixed-pose.txt` with `-b 3200`. That pins the camera, drops the
pack coherence and billboard measure caches (both key on pose, and would
otherwise report a static scene as free), and asks redraw policy for a rebuild
every frame -- so every iteration rasterizes ONE identical scene and the
workload counters come back as exact integers. Confirm they are identical on
both sides; if they are, the subtick delta is real. The harness compiles to
nothing when `PERF_FIXED_POSE` is unset (byte-identical ROM). Note the frame is
~18 vblanks at that pose, so proportionally more vblank/DMA steal lands inside
each stage timer than on a route -- deltas are valid, absolute shares read a
little compressed.

**The view tilemap is column-major**, `view_tile_index(tile_x, tile_y) =
tile_x * VIEW_TILE_H + tile_y`. A column's tiles are contiguous, and screen row
`y` of byte lane `L` sits at `(y>>3)*32 + (y&7)*4 + L`, which is identically
`4*y + L`. Two shipped optimizations rest on this, and one real bug came from
assuming row-major (LOG, 2026-08-03).

**Wall fidelity tuning is an offline 64x64 bake.** The shipped renderer remains
`WALL_TEX_DIM=64` at stride 2; its 786,432-byte shade/door packed table would
quadruple at 128x128 without increasing the 80 sampled screen columns. Curated
technological-wall simplification lives in `tools/world_assets.py` and must pass
`tools/wall_bake_preview.py` before regenerating assets. Non-target materials,
PAL3 and runtime/assembly consumers stay unchanged. COMPUTE2 is the precedent
that filtering alone cannot repair an oversized semantic panel: preserve its
world repeat but select/compose a readable source-derived 64x64 facade, and
always audit the close oblique checkpoint (LOG, 2026-08-22).

**Budgets: 64 KB work RAM, 4 MB ROM.** The work-RAM guardrail is the binding
one — `tools/check-rom.ps1` errors below 16 KB free and SGDK panics at boot
around 13.7 KB. ROM is *not* tight: `.text` is ~1.39 MB against a 4 MB cap, so
there is ~2.6 MB of headroom. The 1408 KB figure some notes used is just where
`sizebnd` pads `out/rom.bin`, not a limit. Check `size.exe out/rom.out` against
the 4 MB cap before ever calling a precompute-vs-compute tradeoff unaffordable.
`ENABLE_BANK_SWITCH` (SSF mapper, 12 MB) exists but its `0x300000` window is for
cold bulk assets — never put a table sampled at pixel rate behind it.

**Fidelity tradeoffs need the user judging motion.** A static screenshot
approval does not survive real gameplay; the stride-4 revert (LOG, 2026-07-27)
is the precedent.

**Frontend sprite animation needs `SYS_doVBlankProcess()`, not only
`VDP_waitVSync()`.** The latter waits for blanking but does not commit queued
`SPR_update()` sprite-table work. A boot card can therefore look correct in a
static capture while its animation is frozen. Keep the per-frame system VBlank
process wherever a frontend card moves sprites or cycles queued palette data.

**Do not promote Doom upper/lower portal bands to flat walls.** A full-height
runtime SEG changes rendering, collision and LOS and becomes a wedge at close
range. For strict-2D room simplification, first try a structurally validated
offline material transfer onto existing solid perimeter geometry, and prove
the recipe-on/off SEG geometry and per-column depth are identical (LOG,
2026-08-21).

**Reclassifying an already-solid line is a different thing, and is allowed.**
The rule above is about ADDING geometry for a band the flattener erased. Giving
a line the flattener ALREADY emits as a solid wall a new seg type -- as
`BSP_SEG_WINDOW` does for Doom's windows (2026-08-29) -- adds nothing, moves
nothing and keeps the line solid for collision and LOS, so none of the failure
modes above apply. The bar for it is that the emitted seg set stays
byte-identical: `tools/doom_map.py`'s `load_map(..., apply_windows=False)` is
the negative control, and `tools/test-sector-map.py` diffs the two and fails on
any change outside the type byte. Reuse that pattern rather than inventing a
new one, and keep the geometry proof in the test.

**Anything that writes a wall texel reads the baked pair table.**
`packed_wall_column()` + `wall_packed_y()` is the only sanctioned path: the pair
byte in `FREEDOOM_WALL_PACKED_PAIRS` / `FREEDOOM_WALL_DOOR_PACKED_PAIRS` already
carries the v-scale, the shade level, the door frame/safety silhouette and BOTH
horizontal texels. The door/window overlay spent years re-deriving that per pixel
through `wall_source_y` (two `__mulsi3` calls, since the 68000 has no 32x32
MULU), and it was ~38% of the pack stage with a window on screen (LOG,
2026-08-30). `wall_source_y` and `FREEDOOM_WALL_TEXTURES` serve the offline bake,
not the frame -- note that `wall_source_y` returns EARLY and UNMASKED for a
full-height texture and will index past a texture's 128 rows for any nonzero
`tex_v_offset`. Likewise, a stride-2 write into `g_view_tiles` is a byte store at
`4*y + lane`, never a 32-bit read-modify-write with a variable shift.

**A Doom SECRET door is still a door, but it looks like its wall.** The WAD's
SECRET flag may appear on only one linedef face, so `tools/doom_map.py` promotes
it to `BSP_SEG_FLAG_PLAIN_DOOR` across the whole physical door group. Keep the
ordinary `BSP_SEG_DOOR` type, lift state, collision, LOS and interaction; only
the packed-pair selection changes from the framed door plane to the normal wall
plane, both closed and moving. The moving overlay carries this variant in its
otherwise-unused `band_top` byte so `RayDoorOverlay` stays 10 bytes (LOG,
2026-09-04).

## Dead ends — do not redo without new evidence

Each is measured and written up in [LOG.md](LOG.md); the date locates the entry.

- **Partial base-bank upload** — the 300-tile upload is ~2 vblanks of a ~12
  vblank frame; DMA-side levers cannot recover what the CPU is spending
  (2026-07-19).
- **`RAY_COL_STRIDE` 4** — shipped, then reverted. Faster, but the user rejected
  the fidelity loss in motion (2026-07-22 → 2026-07-27).
- **Billboard projection spatial pre-cull** (2026-07-21) and **visible-subsector
  billboard cull** (2026-07-29) — both cost more than they saved.
- **Normalizing `fx_sin`/`fx_cos` to unit amplitude** — renders the world ~20%
  larger and costs 2.7x in sprite rasterization. See the invariant below
  (2026-07-30).
- **Duplicating `FREEDOOM_WALL_PACKED_PAIRS` to drop `andi.w #63`** — it fits in
  ROM now, but the post loops are only ~a quarter of the pack stage, so it would
  buy ~1% for +736 KB (2026-08-04).
- **Applying the billboard gather/apply row cache unconditionally** — +44% on
  `stationary-combat`. It is gated on magnification for a reason (2026-08-04).
- **Turning GCSE back on** — SGDK compiles with `-fno-web -fno-gcse`, and that is
  correct for this code: `#pragma GCC optimize("gcse","web",...)` on the five BSP
  translation units made cast **2.1% worse** on identical pose-locked workload.
  GCSE raises register pressure and spills on a 68000's eight data registers.
  Dropping `volatile` from the pure `muls.w`/`mulu.w` helpers to let it CSE them
  is the same dead end from the other side, and measured exactly zero
  (2026-08-30).

## How to measure renderer frame cost (headless, no BlastEm UI needed)

1. Build with perf instrumentation and the deterministic-route checkpoint hook:
   ```
   $env:EXTRA_FLAGS="-DDEBUG_BLASTEM_CHECKPOINT=1"; .\tools\build-windows.ps1 -DebugPerf
   ```
2. Resolve the perf mailbox symbol:
   ```
   python tools/resolve-symbol.py out/symbol.txt g_debug_perf_mailbox --bytes 128
   ```
3. Run the deterministic route through the custom BlastEm build and capture a report
   (the current frontend reaches gameplay around frame 1180):
   ```
    .externals/blastem/build/windows/blastem.exe -b 1400 out/rom.bin \
     --md-route tools/routes/checkpoints.txt --md-report out/perf.json \
     --md-perf-mailbox <ADDR>:128
   ```
   `-b <frames>` is required or BlastEm never exits and no report is written.
4. Decode the `perfMailbox` hex blob as `RendererPerfSnapshot` (see
   `src/renderer/renderer_perf.h`) using **m68000 struct alignment** (u16/u32 align to
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

## Real release framerate (current: 2026-08-04, cadence probe)

A checkpoint-only build (no -DebugPerf) publishes a `CadenceSnapshot`
(decode with `tools/decode-cadence.py`). **This is the ground truth**, and it
differs sharply from what DEBUG_PERF builds imply — DEBUG_PERF's own
instrumentation distorts the timing it reports.

- **Idle frames hit the 2-vblank (30fps) target.** The cost is entirely in
  rebuild frames, which is why a route's average says little on its own: check
  `rebuild frames` against `iterations`.
- **Rebuild frames, per stage** (`stationary-combat` / `tour-east-combat`):
  cast 5700 / 4100, pack 2750 / 5400, projection ~1500 both, billboard 2500 /
  1600 subticks. Cast and pack are the frame; pack roughly doubles in motion
  because rotation doubles the mixed (wall) tile count.
- Divide `cast` and `pack` by `rebuild frames`, but `projection` and
  `billboard` by `scene frames` — those two run every scene frame. Getting this
  wrong understated billboard cost ~6x for weeks (LOG, 2026-08-03).
- Cast's counted workload is tiny (~50 nodes, ~55 box projections, ~30 segs
  tested, 11-16 drawn per rebuild) — per-unit constant factors dominate.
- Projection re-measures all ~112 registry objects whenever the player moves
  **or turns** (the measure cache keys on exact pose, so it misses in motion),
  which is why it is a near-constant ~1500 regardless of what is on screen.
- Anything whose cost scales with on-screen area needs its **worst frame**, not
  its mean: `bb WORST frame` exists because a point-blank sprite ran 30x the
  route average (LOG, 2026-08-04).

Consequence: DMA-side levers (sparse FB, partial uploads) can recover at most
~2 vblanks; the frame is CPU-bound in cast+pack. Smoothness work should target
those two stages or frame-pacing, not upload bytes.

### `fx_sin`/`fx_cos` carry a deliberate 1.1839 gain (fixed 2026-07-30)

`sin_quarter_q8`'s 3rd/5th Taylor coefficients were 41 and 5 — each exactly a
quarter of correct (165, 20) — so the table was a near-linear ramp peaking at 361
instead of `FX_ONE`. The view basis is `(cos, sin)` with `right =
perp(forward)`, so a magnitude error scales `depth` and `lateral` together:
screen-x stayed exact (the scale cancels in `PROJ*lateral/depth`) but wall and
sprite height go as `1/|basis|`. `|basis|/FX_ONE` swung **1.0645..1.4102** with
heading, so **every wall and sprite pulsed 32.5% with a 90° period as the player
turned** — a wall 256 units dead ahead measured 28px facing an axis and 37px
facing diagonally — and frame cost breathed with it. `tools/test-bsp-render-math.py`
reproduced the same buggy polynomial, which is why the differential model agreed
with the ROM and never caught it.

**Normalizing to unit amplitude was measured and rejected.** It shrinks
view-space depth ~17%, rendering the world ~20% larger, which costs **2.7× in
sprite rasterization**: checkpoints 11.79 → 15.50 vb/frame (5.1 → 3.9 fps), of
which +2.84 of the +3.71 vb is billboard raster alone. The wall pipeline barely
moved. So the coefficients now carry a 1.1839 gain — the old table's *mean*
magnitude — giving the same average rendered size and movement speed as before
while removing the heading-dependent variation (magnitude ratio 1.3248 → 1.0192,
heading error −3.03..+4.43° → −0.19..+1.60°).

**The gain is user-approved in motion** (2026-07-30, playing the release ROM:
"no visual problems"), which per the stride-4 lesson below is the only judgement
that qualifies a fidelity change. Treat the non-unit amplitude as intended, not
as a leftover bug.

Consequences to know before touching trig or scale:
- `fx_sin`/`fx_cos` return `1.1839*sin`, **not** `sin`; `|basis|` is ~303.
  The gain cancels in any *ratio* of two basis projections, but not where a
  projection is an absolute length: height, view-space depth, `FOG_SHIFT`
  banding, and movement thrust all carry it.
- `tools/test-world-scale.py` models thrust as `command*THRUST_SCALE`, i.e. an
  exactly-unit basis, so real movement runs ~18% above its certified
  `walk=283.6u/s`. That gap predates the gain; do not "fix" it by normalizing
  the table without re-measuring the sprite cost above.
- E1M1 starts at angle **192, exactly axis-aligned**, where the old table sat at
  its 1.4102 maximum and rendered everything at its smallest and cheapest. So
  the new table renders ~19% *larger* than before at axis-aligned headings and
  ~16% smaller at diagonals; a start-room route will show it as a small
  regression that is a wash averaged over headings. Same-pose A/B on
  stationary-combat: cast 6090 → 6070, pack 4225 → 4615.
- Both baked reciprocal LUTs stay valid — `bsp_inv_depth_lut.h` guards on
  `depth >= BSP_NEAR` and `billboard_projection_lut.h` on the `forward` value
  itself, neither on world distance.
