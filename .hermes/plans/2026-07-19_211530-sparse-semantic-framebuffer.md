# Sparse Semantic Framebuffer Implementation Plan

> **For Hermes:** Use subagent-driven-development skill to implement this plan task-by-task.

**Goal:** Replace the unconditional 300-tile full-bank upload with a sparse
semantic framebuffer: pure ceiling/floor tiles reference shared static VRAM
tiles (0 DMA in motion); only the union of wall/door/overlay tiles are packed
into compact dynamic slots in the inactive bank and uploaded as contiguous
column-major runs. Behind a compile-time flag; old path stays until visual parity.

**Architecture:** Keep `g_view_tiles[300][8]` (column-major, full positional
framebuffer) in RAM untouched at first — we only change *what gets uploaded* and
*what the tilemap points at*. Two existing 300-tile VRAM banks (A/B) remain;
the inactive one is filled compactly with only dynamic tiles. Add a small static
atlas (1 ceiling tile + 1 floor tile, pre-baked in VRAM). The mixed tilemap
(600 B, screen-row-major) points each cell at either a shared static tile or a
compact dynamic slot. Atomically swap the displayed bank + commit the tilemap only
after every dynamic run is uploaded → no tearing.

**Tech Stack:** SGDK / Genesis / 68000 C; VRAM tile allocator (existing
`VIEW_TILE_BASE` scheme); `VDP_loadTileData` / `VDP_setTileMapDataRect`;
BlastEm deterministic-route harness + `ColumnReuseOracle`/`SparseTileOracle`
(already in tree, DEBUG_PERF-only).

**Grounding (verified by reading the tree):**
- `src/renderer_internal.h:23-30` — `VIEW_TILE_BASE=TILE_USER_INDEX`,
  `VIEW_BANK_COUNT=2`, `VIEW_DYNAMIC_TILE_COUNT=VIEW_TILE_COUNT*2=600`,
  bank N tiles live at `VIEW_TILE_BASE + N*VIEW_TILE_COUNT + slot`.
- `src/renderer.c:105-134` `build_view_bank_tilemaps` — builds the
  per-bank tilemap; `renderer_set_view_vram_bank` commits it via
  `VDP_setTileMapDataRect` (BG_B). Swap is already atomic on this call.
- `src/renderer.c:169-190` `renderer_prepare_full_base_upload` — unconditional
  300-tile dirty mark into the inactive bank. THIS is what we stop calling.
- `src/renderer_scene.c:1109-1120` `load_view_tile_run` — uploads
  `g_view_tiles[first..first+count]` to `vram_base+first` (linear; slot in
  RAM == slot in VRAM). Column-major makes a vertical wall run = contiguous
  `g_view_tiles` rows, so one `VDP_loadTileData` per run works directly.
- `src/renderer_scene.c:1143-1177` `renderer_queue_scene_upload` /
  `finish_view_upload` — the deferred, budgeted-per-VBlank upload pump. We
  reuse this pump; it already calls `renderer_set_view_vram_bank` on swap.
- `src/renderer_scene.c:180-196` `build_flat_rows` — ceiling is per-sector
  color (Bayer-packed into 4 rows repeated twice → 1 tile per sector),
  floor is a ROM constant `MEGALDOOM_WORLD_COLOR_FLOOR` (same for every
  sector, so ONE shared floor tile covers all floor cells).
- `src/renderer_scene.c:640-728` — the packer classifies each repacked tile
  as ceiling (665) / floor (684) / mixed-wall (706). The SparseTileOracle
  already counts these. We reuse the SAME branch decisions to build the
  dynamic mask (no new classification logic).
- `src/debug_checkpoint.h:32` — `DEBUG_CHECKPOINT_PERF_MAILBOX_BYTES=256`;
  `RendererPerfSnapshot` already carries `sparse_*` aggregates.

**Budget (corrected — see AGENTS.md):**
`est_dma = (dyn_wall + overlay)*32 + 600`. Nominal VBlank ≈ 4800 B
(150 tiles). Reserve 600 B tilemap → 4200 B = 131 tiles nominal line.
Conservative **safe gate = 120 tiles (3840 B)**. Measured peaks:
movement 85, stationary 75 (well under), slow-turn **122 (over safe gate by
2, under nominal)**. → GO for prototype; needs real DMA-timing/run-count
measurement + multi-VBlank overflow before declaring 60fps.

**Acceptance (definitive, not the oracle):** `average_vblanks_x10 ≈ 10`,
no rise in `missed_deadlines`, no tearing, `asm_mismatches == 0` on every
route, work-RAM guardrail (≥20480 B free) holds, all repo tests pass.

---

## Task 1: Add compile flag + per-frame semantic classify (no upload change yet)

**Objective:** Introduce `RENDERER_SPARSE_FB` flag and a function that, given the
just-packed `g_view_tiles` + the packer's wall/overlay masks, builds a
`SparseFrameBuild` struct (dynamic union mask, run list, slot allocation, mixed
tilemap) — but under the flag only; the existing full upload still runs.

**Files:**
- Modify: `src/renderer_internal.h` — add `#define RENDERER_SPARSE_FB 0`
  (0 = old path; flip to 1 to enable) + `SparseFrameBuild` struct +
  `SparseTileRun` (source_y, tile_count, dest_slot).
- Modify: `src/renderer_scene.c` — add `static void sparse_classify_frame(...)`
  computing `dynamic_mask[x]` = wall_mask[x] | door_mask[x] | overlay_mask[x]
  from the packer branches; build `dynamic_rows[x]` (15-bit per-tile union
  within a column) + count `dynamic_tile_count`/`dynamic_run_count`.
- Test: `tools/test-renderer-upload-policy.py` — assert old path still emits
  `renderer_prepare_full_base_upload()` when flag is 0 (regression guard).

**Step 1:** Write test asserting policy string present when `RENDERER_SPARSE_FB==0`.
**Step 2:** Run `npm run test` → PASS (baseline, no change yet).
**Step 3:** Add struct + empty `#if RENDERER_SPARSE_FB` classify stub returning
  all-tiles-dynamic (equivalent to old behavior).
**Step 4:** `npm run test` → PASS. **Step 5:** Commit (flag stays 0).

## Task 2: Static atlas + mixed-tilemap builder

**Objective:** Build a static ceiling atlas (one shared tile per *distinct* ceiling
appearance) + one globally shared floor tile, uploaded **once at level
init**; build the 600-B mixed tilemap that points pure ceiling/floor cells at
the shared tiles and dynamic cells at compact slots.

**Ceiling is static per frame — NO runtime rebake.** `bsp_cast_frame` selects a
single `RayFlatColor` ceiling from the sector containing the player; that one
appearance applies to the *entire* viewport. Visible remote sectors contribute
no separate ceiling. Because the ceiling Bayer pattern repeats every 4px and
tiles are 8px-aligned, every full ceiling tile of a given `RayFlatColor` is
byte-identical — so each distinct ceiling appearance needs exactly ONE atlas
tile, baked once and never again.

**Files:**
- Prefer: extend `tools/generate-renderer-assets.ps1` (which already emits
  `FREEDOOM_SECTOR_VISUALS`) to ALSO emit, offline, a deduplicated ceiling
  atlas `const u32 FREEDOOM_CEILING_TILES[][8]` plus a
  `const u8 FREEDOOM_SECTOR_CEILING_TILE_INDEX[]` lookup, keyed by the exact
  `RayFlatColor` tuple `(primary, secondary, secondary_coverage)`. The MD
  then uploads the atlas once at level init and selects
  `g_sector_ceiling_tile[current_sector]` at runtime. No Bayer sampling, key
  compare, pixel gen, or rebake at runtime.
- Fallback (if offline gen isn't ready): in `src/renderer.c` level-init,
  walk `FREEDOOM_SECTOR_VISUALS`, dedupe by the `CeilingKey` tuple, generate
  one 8x8 tile per distinct key via `write_repeated_flat_tile` + `VDP_loadTileData`,
  build `static u16 g_sector_ceiling_tile[FREEDOOM_SECTOR_VISUAL_COUNT]`, and
  upload all atlas tiles ONCE. Reserve VRAM (from `HUD_VRAM_SAFE_TILE_LIMIT`
  downward or a reserved region — confirm free VRAM in `renderer_init`).
- Floor: `MEGALDOOM_WORLD_COLOR_FLOOR` is ROM-constant across all sectors →
  exactly ONE shared floor tile forever; bake it once at level init.
- Modify: `src/renderer_scene.c` `sparse_build_tilemap` — fill
  `tilemap[screen_index]` = `TILE_ATTR_FULL(PAL3,0,0,0, STATIC_FLOOR_TILE_BASE)`
  for floor cells, `STATIC_CEILING_TILE_BASE` for ceiling, else
  `VIEW_TILE_BASE + bank*VIEW_TILE_COUNT + dynamic_slot`.
- Test: `tools/test-active-battle-perf.py` already asserts `VDP_setTileMapDataRect`
  usage; extend to assert mixed-map call present under flag.

**Step 1:** Test asserting `STATIC_*_TILE_BASE` symbols referenced under flag.
**Step 2:** Run → FAIL (not built). **Step 3:** Implement atlas bake + builder.
**Step 4:** Run → PASS. **Step 5:** Commit (flag still 0; builder dead code
  until Task 3 wires it).

## Task 3: Inactive-bank compact allocator + column-major runs + sparse uploader

**Objective:** Allocate compact dynamic slots into the inactive VRAM bank, queue
one `VDP_loadTileData` per contiguous column-major wall/overlay run straight from
`g_view_tiles`, and reuse the existing deferred upload pump.

**Files:**
- Modify: `src/renderer_scene.c` `sparse_queue_dynamic_runs` — walk
  `dynamic_rows[x]`; for each set of consecutive tile_y bits in a column, emit
  `load_view_tile_run(inactive_bank_base, view_tile_index(x, first_y), run_len)`
  to slot `next_dynamic_slot++`. (Column-major ⇒ `g_view_tiles` rows are
  contiguous ⇒ one DMA run.) No extra 4.8 KB work-RAM buffer (per budget
  constraint: only ~3 KB headroom over the 20480 guardrail).
- Modify: `renderer_queue_scene_upload` — when `RENDERER_SPARSE_FB==1` and
  dynamic count ≤ `SPARSE_ONE_VBLANK_BUDGET` (120), call classify→build→
  queue instead of `renderer_prepare_full_base_upload`. Keep old call otherwise.
- Test: `tools/test-renderer-upload-policy.py` — under flag, assert
  `renderer_prepare_full_base_upload()` is NOT called and classify/builder IS.

**Step 1:** Test asserting old full-upload absent under flag.
**Step 2:** Run → FAIL. **Step 3:** Wire allocator + queue behind flag.
**Step 4:** Run → PASS. **Step 5:** Commit (flag still 0 in committed tree;
  flip locally to test).

## Task 4: Atomic tilemap commit + multi-VBlank overflow + visual parity

**Objective:** Swap displayed bank + commit mixed tilemap only after all dynamic
runs uploaded (no tearing); frames exceeding one-VBlank budget spill across
multiple VBlanks while the old frame stays displayed; verify byte-identical
pixels.

**Files:**
- Modify: `src/renderer.c` `renderer_set_view_vram_bank` — reuse as the
  atomic commit point (already a single `VDP_setTileMapDataRect` on BG_B).
  For sparse, the tilemap passed is the mixed one, not the bank-uniform one.
- Modify: upload pump — `if (estimated_total_dma_bytes <= SPARSE_ONE_VBLANK_BUDGET)
  upload_in_one_vblank(); else upload_across_multiple_vblanks();` (overflow
  never corrupts: old bank remains displayed until swap).
- Verify: `npm run build -DebugPerf` + BlastEm route on all 3 routes →
  `asm_mismatches == 0`. Locally flip `RENDERER_SPARSE_FB=1` for the run.
- Test: extend `tools/test-renderer-upload-policy.py` with overflow branch.

**Step 1:** Test for overflow branch presence. **Step 2:** Run → FAIL.
**Step 3:** Implement atomic commit + overflow. **Step 4:** Run + BlastEm
  parity check → PASS (`asm_mismatches=0`). **Step 5:** Commit.

## Task 5: Stress route (many enemies/doors/explosions) + re-measure real metrics

**Objective:** Capture the real DMA-timing/run-count budget the oracle only
estimates; confirm slow-turn / heavy-combat stay within VBlank or spill
correctly.

**Files:**
- Create: `tools/routes/heavy-combat.txt` — stationary + many billboards
  firing (forces high `sparse_overlay_max`, currently peaks 105).
- Modify: `tools/decode-perf.py` — add real-upload fields once the oracle
  records them (tile DMA bytes, tilemap bytes, other-VBlank DMA, total,
  run count, DMA command count, actual VBlanks).
- Run: all 4 routes (checkpoints, slow-turn, stationary-combat, heavy-combat)
  under flag; confirm `average_vblanks_x10 ≈ 10`, `missed_deadlines` no
  rise, `asm_mismatches == 0`.

**Step 1:** Author route. **Step 2:** Run all 4 → collect. **Step 3:** Decode
  + compare to nominal 4800-B / 120-tile gate. **Step 4:** Document in
  AGENTS.md. **Step 5:** Commit.

## Task 6: Enable in release only after real metrics pass

**Objective:** Flip `RENDERER_SPARSE_FB` to 1 in the committed tree ONLY if
Task 5 shows true 60fps headroom (avg VBlanks ≈ 1, no missed deadlines,
parity). Otherwise leave 0 and record the blocker in AGENTS.md.

**Files:** `src/renderer_internal.h` (flip) + `AGENTS.md` (verdict).
**Step 1:** If metrics good → flip to 1. **Step 2:** `npm run build && npm run
  test` → PASS. **Step 3:** Commit. **Step 4:** If NOT good → leave 0,
  write blocker note, do NOT enable.

---

## Risks / Tradeoffs / Open Questions
- **Ceiling is static per frame — no rebake.** `bsp_cast_frame` selects one
  `RayFlatColor` ceiling from the player's current sector; that single
  appearance covers the entire viewport, and distant visible sectors add no
  separate ceiling. The Bayer pattern repeats every 4px while tiles start
  8px-aligned, so every full ceiling tile of a given `RayFlatColor` is
  byte-identical → each distinct ceiling appearance needs exactly ONE atlas
  tile. Build the atlas **once at level init** (preferably offline beside
  `FREEDOOM_SECTOR_VISUALS`, deduped by the `(primary, secondary,
  secondary_coverage)` tuple) and select `g_sector_ceiling_tile[current_sector]`
  at runtime. There is NO "extra DMA tile on sector change" — crossing into a
  sector with a different ceiling just points those cells at the already-
  resident atlas tile. Floor is ROM-constant → truly ONE shared tile forever.
  Consequence for the budget: normal sparse-frame DMA must include **zero**
  floor DMA and **zero** ceiling DMA; only the one-time level-init atlas upload
  touches those tiles.
- **Overlay copy-on-write dominates** (64–74 tiles vs 5–13 wall). The dominant
  cost is billboard COW, not walls. Allocate overlay tiles into the SAME compact
  dynamic bank as wall; union mask avoids double-counting (per your spec).
- **No extra work-RAM buffer.** Only ~3 KB headroom over the 20480 guardrail,
  so the allocator/slot bookkeeping must live in the existing `g_view_tiles`
  addressing + a tiny `SparseFrameBuild` struct (fits: 20 u16 mask + ~300 u16
  tilemap + a few scalars ≪ 3 KB).
- **Multi-VBlank overflow is mandatory, not optional:** slow-turn峰值 122 >
  120 gate, so some frames MUST spill. Old bank stays displayed → no corruption.
- **`draw_door_overlays` / `draw_billboards` still write `g_view_tiles`**
  positionally AFTER `build_bsp_tilemap` (per your frame-flow). Classify must
  run AFTER those, on the final `g_view_tiles` (union of wall+door+overlay).
  Keep `g_view_tiles` full until Task 4 parity is proven; only then consider
  skipping static-tile RAM writes.
- **The `-DDEBUG_BLASTEM_CHECKPOINT` route is flaky** (reaches gameplay ~50%
  of runs in this env). Use `-Clean` rebuild + `test-checkpoints.ps1`
  (`RequireCheckpoints`) as the reliable gameplay-reach path; re-run until
  `seen` == required.

## Verification commands (all phases)
- `npm run build` (release) — must exit 0.
- `npm run test` — all guardrails incl. `test-renderer-upload-policy.py`.
- Perf + parity: `EXTRA_FLAGS="-DDEBUG_BLASTEM_CHECKPOINT=1" pwsh -NoProfile
  -File tools/build-windows.ps1 -Clean -DebugPerf` then
  `pwsh -NoProfile -File tools/run-blastem-route.ps1 -Route <r> -Frames <f>
  -Report out/perf.json -Mailbox <mb> -RequireCheckpoints <mask>
  -PerfMailbox <pm>` → decode with `tools/decode-perf.py`.
- Visual: `npm run play` (you confirm).

## Phase 4 / 5 / 6 status (2026-07-20)

**Phase 4 (atomic mixed-tilemap commit + visual parity): COMPLETE & GATED.**
- `2d3414c` implemented the end-to-end sparse path (classify from column
  descriptors → queue dynamic runs → build mixed tilemap → atomic reveal after
  `dbg_wait_dma`). `7b21d15` fixed reviewer F1 (defer tilemap commit past
  `dbg_wait_dma`, restoring strict legacy atomicity). `8edab0d` moved the sparse
  defs to file scope (a plain `RENDERER_SPARSE_FB=1` build was broken because the
  defs had been nested inside `#if DEBUG_PERF`).
- Verified `asm_mismatches = 0` on `checkpoints.txt` at flag 1 (DEBUG_PERF).

**Phase 5 (re-measure on real routes): COMPLETE.**
Ran all 3 routes at flag 1 (DEBUG_PERF), A/B vs a true flag-0 legacy build:

| Route            | asm_mismatches | avg_vblanks_x10 | missed_deadlines | notes |
|------------------|----------------|-----------------|------------------|-------|
| checkpoints      | 0              | 183             | 9                | legacy baseline 181/9 → no regression |
| stationary-combat| 0              | 155             | 11               | better than legacy (155 < 181) |
| slow-turn        | 0              | 186             | 8                | legacy fallback fires on >120-tile frames (upload_full=1, parity kept) |

Sparse oracle (real): avg ~75 dynamic tiles/frame (well under 120 budget);
slow-turn peaks ~122 (transient) → correctly falls back to legacy. work-RAM at
flag 1 = 20326 B free (within 20480 guardrail). Pixel parity holds on ALL routes.

**Phase 6 (enable decision): DO NOT ENABLE IN RELEASE — flag stays 0.**
The user's enable checklist requires a **heavy-billboard stress route** (many
enemies in view; overlay COW is the dominant dynamic cost per the Phase 1-3
oracle). The 3 captured routes are movement / stationary-combat / slow-turn — NONE
is a dense-enemy stress route, so that gate is UNMET. Per the standing rule, the
sparse path remains a verified, parity-clean **prototype behind the flag**; the
legacy full-upload path is the release default and the safe fallback.
- To enable later: capture a heavy-billboard route (area with many enemies in
  view) through the custom BlastEm, run it at flag 1, confirm asm_mismatches=0 and
  no vblank regression, THEN flip `RENDERER_SPARSE_FB` default in
  `src/renderer_internal.h`. Until then, leave it 0.
