# LOG.md

Dated renderer investigations: what was tried, the numbers, and why it was kept
or dropped. Newest first. Standing rules distilled from these live in
[AGENTS.md](AGENTS.md) — if an entry here changes how future work should be
done, add the rule there too rather than relying on anyone reading this far.

Numbers are release-cadence subticks unless stated otherwise; ~100 m68k cycles
each, ~1282 to a vblank. See AGENTS.md for how to reproduce a measurement.

## Pack stage: DBRA posts in the mixed-tile hotpath (2026-08-04)

Rotation makes `pack` roughly double: **6.5 vb/rebuild spinning
(`barrel-spin`) vs 3.0-3.8 standing or translating**. New release-cadence
counters (`pack columns`, `pack tiles`, always on) say why, and it is *not*
mainly the coherence cache:

| route | pack | columns repacked | mixed tiles | flat tiles |
|---|---|---|---|---|
| `barrel-spin` | 8336 | 19.8 / 20 | **149.1** | 147.1 |
| `checkpoints` | 4866 | 16.4 | 75.1 | 170.3 |
| `stationary-combat` | 4065 | 14.3 | 58.2 | 156.8 |
| `barrel-pointblank` | 5657 | 19.4 | 90.5 | 200.1 |
| `tour-east-combat` | 7002 | 15.8 | 121.9 | 115.0 |

Coherence does collapse in a spin (19.8/20 columns repacked vs 14.3 standing),
but the real driver is that **a spin near walls doubles the mixed (wall) tile
count**, and a mixed tile costs ~42 subticks more than a flat one. Fitting
`columns`/`mixed`/`flat` together is rank-deficient — `columns * 15 == mixed +
flat` — so only the two tile coefficients are identifiable; don't try to solve
for a per-column term.

**Fix, in `src/renderer_hotpath.s`.** Each lane emits at most three monotonic
posts (ceiling, wall, floor). The loops re-tested both the post bound and `end_y`
every pixel — `cmp/bcc/cmp/bcs`, 26 cycles, where the post length is already
known and `DBRA` costs 10. The wall post additionally re-read `tex_y` from the
descriptor every pixel (`add.b 17(a1),d5`, 12 cycles); it now lives in `d4`,
which is free because `bottom` is dead once the post length is computed. Wall
post: ~104 -> ~72 cycles per byte.

| route | pack before | after | mixed tiles before/after |
|---|---|---|---|
| `stationary-combat` | 4065 | **3642** (-10.4%) | 58.2 / 58.2 (identical) |
| `barrel-spin` | 8336 | **7233** (-13.2%) | 149.1 / 154.5 (*more* work) |
| `barrel-pointblank` | 5657 | **4946** (-12.6%) | 90.5 / 89.6 |
| `checkpoints` | 4866 | **4325** (-11.1%) | 75.1 / 75.6 |

`stationary-combat` is the clean one: identical column and tile counts on both
sides, so -10.4% is pure per-tile speedup. Costs no RAM and ~30 bytes of ROM.

**The asm/C harness was silently disarmed for months — read this before
trusting it (found 2026-08-04).** `compare_stride2_tile_asm` (DEBUG_PERF) used
to take the *framebuffer tile the packer had just written* as its C side. That
was a real differential only while `RENDERER_HOTPATH_C_REFERENCE` defaulted to
1. The moment the default flipped to 0 and `write_mixed_stride2_tile` became
`renderer_write_mixed_stride2_tile_asm`, the harness began comparing **asm
against asm** and could not report a mismatch no matter what the assembly did.
Every `asm_mismatches=0` recorded in this file between those two events is
worthless, including the ones cited for the DBRA and hoisting commits.

It now runs *both* implementations locally into two canary-guarded scratch
tiles, so which one ships cannot disarm the check, and `write_mixed_stride2_tile_reference`
compiles in whenever `DEBUG_PERF` is set (kept deliberately separate from the
macro that selects the shipped implementation).

**A check that has never been observed to fail is not known to work.** This one
was proven by negative control: changing `andi.w #63,d5` to `#62` in the wall
post produced **3034 mismatches on `tour-east-combat` and 272 on `slow-turn`**,
where the old harness reported 0 for the identical fault. After reverting:
**22950 tiles checked, `asm_mismatches=0`, `asm_canary_failures=0`, 76 complete
300-tile cycles.** Never touch this file without reading those numbers back —
and if a change to the harness itself is involved, re-run the negative control.

**Deferred (NOT rejected on size): dropping `andi.w #63,d5` from the wall post**
by duplicating each packed column to 128 entries so `vertical_samples[i] +
tex_y` (max 126) needs no wrap. It saves 8 of ~72 cycles in the wall post.
`FREEDOOM_WALL_PACKED_PAIRS` is `[2][4][23][64][64]` = 736 KB and doubling the
last dimension adds another 736 KB.

**There is room for that.** An earlier revision of this section claimed the cart
was near a 1408 KB limit and called the idea unaffordable — that was wrong.
1408 KB is just where `sizebnd` currently pads (128 KB-aligned); the actual
guardrail in `tools/check-rom.ps1` is `$RomMaxBytes = 4 MB` and `.text` is
1387690 bytes. **Headroom is ~2.6 MB.** Do not cite ROM size as a blocker for a
precompute-vs-compute tradeoff without checking `size.exe out/rom.out` against
that 4 MB cap first.

The reason to defer is payoff, not budget: 8 of 72 cycles is ~11% of the wall
post, which is only part of a mixed tile, so expect low single-digit percent on
`pack` for +736 KB. Weigh it against other uses of that headroom.

**Bank switching does not help here, and would hurt.** SGDK's `ENABLE_BANK_SWITCH`
(SEGA/SSF mapper, `rom_head.c` already has the `"SEGA SSF"` branch) reaches 12 MB
by paging 512 KB banks through the `0x300000-0x3FFFFF` window. That is for *cold*
bulk assets read once. A table sampled inside the per-pixel wall loop must never
live behind the window — you would be paying a bank check or swap per lookup. Use
the plain sub-4 MB space for anything the renderer touches at pixel rate.

### Hoisting the post loops, and what it proved about pack (2026-08-04)

Going after the same loops without spending any ROM. Per-byte fat that was
sitting in plain sight:

- Destinations were `0(a6)`. A zero displacement is not free — `d16(An)` costs
  4 cycles more than `(An)` on every byte written. GAS does not fold it.
- The wall post read the DDA as `0(a4,d0.w)` and kept `d0` in step with an
  `addq.w`: 18 cycles where `(a4)+` is 8. `d0` was never read inside the loop,
  and its post-loop value is just `min(bottom, end_y)`, which the length
  computation already had.
- The wall post's `moveq #0,d5` is loop-invariant, which is not obvious:
  `andi.w #63,d5` leaves bits 6-15 clear, `move.b` writes only bits 0-7, and
  `add.b` discards its carry, so d5's high byte is still zero next iteration.
- The flat posts recomputed `((y&3)<<2)+lane` per pixel (move/andi/lsl/add =
  26 cycles) to index a 16-byte table with period 4. The lane term folds into
  the base pointer once and the index steps `+4 mod 16`.

Wall byte 74 -> 56 cycles, flat byte 70 -> 48. On the three routes that stayed
pose-stable (identical mixed/flat tile counts on both sides, so this is a pure
per-tile comparison):

| route | pack before | after | subticks/tile |
|---|---|---|---|
| `stationary-combat` | 3642 | **3425** (-6.0%) | 16.9 -> 15.9 |
| `checkpoints` | 4139 | **3876** (-6.4%) | 18.3 -> 17.2 |
| `slow-turn` | 3845 | **3621** (-5.8%) | 16.0 -> 15.1 |

Zero ROM, zero RAM. (The `asm_mismatches=0` originally reported here proved
nothing — see the harness note below. It was re-verified properly afterwards:
22950 tiles, 0 mismatches, 76 complete cycles.)

**The useful result is the ratio, not the 6%.** Inner-loop cycles were cut
~24% and `pack` moved 6%, so **the three post loops are only about a quarter of
the pack stage** — the other ~75% is C-side per-tile and per-column work
(descriptor construction, the coherence compare, tile classification, run
splitting). Two consequences:

1. This closes the `andi.w #63` / +736 KB idea for good. At a quarter of pack it
   would now buy ~1% of pack. Do not revisit it.
2. Further pack work belongs in `renderer_pack.c`, not `renderer_hotpath.s`.
   Attribute the per-tile overhead before writing any more assembly.

### Where the pack stage actually goes (2026-08-04)

Point 2 above was a guess, and measuring it proved it wrong. `CADENCE_PACK_SPLIT`
(off by default, `EXTRA_FLAGS="... -DCADENCE_PACK_SPLIT=1"`) times the per-column
prologue — four `describe_wall_column` calls plus the coherence compare, which
*every* column pays even when it is then skipped — against the 15-tile write
loop. Timed per column, not per tile: 40 `getSubTick` a rebuild instead of 600,
so the probe does not distort what it measures.

| route | pack | prologue | tile loop | unaccounted |
|---|---|---|---|---|
| `tour-east-combat` | 7489 | 678 (9%) | **6041 (81%)** | 770 (10%) |
| `barrel-spin` | 7160 | 682 (10%) | **5764 (81%)** | 714 (10%) |
| `barrel-pointblank` | 4998 | 705 (14%) | 3605 (72%) | 688 (14%) |
| `checkpoints` | 4205 | 761 (18%) | 2862 (68%) | 582 (14%) |
| `stationary-combat` | 3757 | 723 (19%) | 2462 (66%) | 572 (15%) |

The prologue is a flat ~34-38 subticks/column everywhere — it does not scale
with anything, and on the routes that hurt it is under 10%. **Not the lever.**
The tile loop is 66-81%, and it grows exactly where the mixed-tile count grows
(11.0/tile on `slow-turn` at 62 mixed, 20.5/tile on `tour-east-combat` at 168).

So the cost is neither the C prologue nor purely the post loops (those are ~25%,
per the section above). What is left is **per-tile overhead inside the tile
loop**: `renderer_write_mixed_stride2_tile_asm` is called once per 8-row tile,
so a column with 9 mixed tiles pays the `movem.l d2-d7/a2-a6` save/restore
(~196 cycles), the five-argument call sequence, and — the larger part — the
four lanes' ceiling/wall/floor split arithmetic **nine times over**, when the
wall geometry of that column is fixed for all 120 rows.

**Next step: one asm call per column, not per tile.** The layout permits it
exactly. Tiles are column-major (`view_tile_index = tile_x * VIEW_TILE_H +
tile_y`), so a column's 15 tiles are contiguous, and within that block screen
row `y` of lane `L` sits at byte `(y>>3)*32 + (y&7)*4 + L`, which is identically
`4*y + L`. **The tile boundary is invisible to a stride-4 byte walk** — one pass
over 120 rows per lane, three posts total instead of three posts per tile.

### Done: one asm call per column (2026-08-04)

Harness first, as required above. `renderer_write_mixed_stride2_tile_asm` became
`renderer_write_mixed_stride2_span_asm`, taking a `row_count` instead of
assuming 8. **The assembly barely changed** — it was already written in terms of
`y` and `end_y`, so the only per-tile assumption in the whole routine was
`addq.w #8,d1`. That became `move.w 58(sp),d1 / add.w d7,d1`, hoisted out of the
lane loop since end_y is now loop-invariant.

The C side slices the column once instead of re-deciding per tile. The three
classes are contiguous runs: whole-ceiling is exactly `tile_y < min_top / 8`,
whole-floor exactly `tile_y >= (max_bottom + 7) / 8`, and `min_top <=
max_bottom` always holds (every top <= its own bottom), so the mixed run between
them never inverts. One `write_mixed_stride2_span` call covers it.

Pose-stable routes, identical mixed/flat tile counts on both sides:

| route | pack before | after | subticks/tile |
|---|---|---|---|
| `checkpoints` | 3876 | **3021** (-22.1%) | 17.2 -> 13.4 |
| `stationary-combat` | 3425 | **2748** (-19.8%) | 15.9 -> 12.8 |

**~20% off pack**, on top of the ~6% from hoisting. Costs nothing: `.text` went
*down* 512 bytes (1387690 -> 1387178), work RAM unchanged at 23336 free.

Ignore `tour-east-combat`'s headline here (12.25 -> 4.08 avg vblanks, 71 -> 214
iterations). The route is not pose-fixed, so a faster build walks somewhere else
entirely and rasterizes a different scene — its mixed-tile count went *up*
165 -> 202. The usual hazard; only the two pose-stable routes above are evidence.

**The harness now checks the property that matters.**
`compare_stride2_column_asm` runs the asm as one span and builds the C side tile
by tile from `write_mixed_stride2_tile_reference`, so it verifies both that asm
agrees with C *and* that one N*8-row call equals N separate 8-row writes. Both
negative controls were run: `andi.w #63` -> `#62` gives 3034 mismatches, and
truncating the span to one tile gives 757 of 760 columns (the 3 clean ones have
0 or 1 mixed tile). Clean run: **1672 columns checked, 0 mismatches, 0 canary
failures, 83 complete 20-column cycles.**

## Billboard raster: the barrel-in-view framerate swing (2026-08-03)

Reported from play: spinning 180 degrees near a barrel makes the framerate dip
and recover in step with the barrel entering and leaving view. It reproduces
headlessly — barrels sit 350-440 world units from the E1M1 player start
(`bsp_things` 864,3328 / 1312,3264 / 1152,2912 vs start 1056,3616) and render at
`visual_scale 3`, so a near barrel is a very large sprite.

**First: the stage numbers above were being divided by the wrong thing.** `cast`
and `pack` run only on base-rebuild frames, but `projection` and `billboard` run
on *every* frame, and `tools/decode-cadence.py` divided all four by
`rebuild_frames`. On a route with many idle frames that overstated the last two
by `iterations / rebuilds` — about 6x on `stationary-combat.txt`. There is now a
`scene_frames` counter (incremented in `renderer_render_scene`) and the decoder
uses it for those two stages. Any billboard/projection figure quoted in this
file from before 2026-08-03 is inflated; the "billboard 0.3-0.7 vb" line in the
release-framerate section was wrong for that reason *and* because it predated
the current sprite population.

Corrected, `draw_projected_billboards` was **1.6-3.3 vblanks per scene frame** —
on `stationary-combat.txt` (3.28 vb) it cost more than the whole pack stage
(3.05 vb) and nearly as much as the cast. That is the swing the report describes.

**Root cause, from the generated asm (not from timers).** The function was one
~170-line scope with ~25 live values; GCC gave up and spilled almost all of them
(`lea (-240,%sp),%sp`, ~14 stack slots reloaded *per pixel*). On top of that,
every opaque texel built its nibble with `(u32)texel << shift` for a shift that
reaches 28, and a variable `LSL.L` on the 68000 costs `8 + 2n` — up to 64 cycles,
twice per texel. Measured cost was ~5.4 subticks (~545 cycles) per pixel slot for
a loop body of maybe a dozen useful instructions.

**Fix: rasterize a byte at a time instead of accumulating a 32-bit tile word.**
A packed tile row is a u32 of 8 nibbles with pixel `col` at bit
`(7 - (col & 7)) * 4`, so on this big-endian target byte `(col & 7) >> 1` of that
word *is* two adjacent screen pixels, even column in the high nibble. Nibble
placement becomes a constant `<< 4` or nothing, the whole `tile_x` / pair / pixel
loop nest collapses to one loop over bytes, and the live set fits the register
file. Two supporting changes: the at most two columns a whole-byte span adds
beyond `[x0, x1]` are pre-marked with the existing `0xFF` skip sentinel, which
removes the per-pixel bounds test entirely; and `pickup_post_contains` now takes
the already-selected offsets row, hoisting a 2D row multiply out of the pixel loop.

Result (subticks per scene frame, all routes):

| route | before | after |
|---|---|---|
| `stationary-combat` | 4202 (3.28 vb) | 2523 (1.97 vb) |
| `slow-turn` | 3886 (3.04 vb) | 2275 (1.78 vb) |
| `checkpoints` | 2462 (1.92 vb) | 1312 (1.02 vb) |
| `barrel-spin` | 2068 (1.62 vb) | 1298 (1.01 vb) |

**-40% to -47%**, ~4.0 -> ~2.3 subticks per pixel slot. ROM/RAM unchanged, all 21
guardrail tests pass. `tools/routes/barrel-spin.txt` (hold left from frame 300,
run with `-b 960`) is the route that reproduces the reported scenario.

### The bug this shipped with, and how it was caught

`view_tile_index` is **column-major** — `tile_x * VIEW_TILE_H + tile_y`, so a
changed column uploads as one DMA run. Walking to the next tile column is
therefore `tile_index += VIEW_TILE_H` (and `+= VIEW_TILE_H * 32` bytes), *not*
`++`. The first version used `++` and wrote into an unrelated tile. It rendered
plausibly and would not have been caught by eye.

**A Python model of the new algorithm passed `tools/test-billboard-raster.py`
at both strides.** That test models the algorithm, so it proves the *design* and
cannot see a C pointer-arithmetic bug. What caught it was an on-target
differential harness: `-DBILLBOARD_RASTER_VERIFY=1` keeps the old rasterizer
compiled in and runs both, **object by object**, comparing packed output. Two
details made it usable:

- A full `VIEW_TILE_COUNT` shadow buffer is 9600 B and left 13494 B of work RAM
  free — under the ~13.7 KB where SGDK panics at boot with "not enough memory to
  reset VDP". Verifying **one rotating tile row** (640 B) is still an exact
  comparison; the reference just drops commits outside the band, and 15 bands
  cycle in 15 frames.
- Comparing per frame only says the final buffers differ, and every later object
  then draws over a base the two paths already disagree about. Comparing per
  object localises it immediately.

0 mismatches on all five routes (including 169 scene frames of
`tour-east-combat`) is the correctness claim. `test-billboard-raster.py` now
carries the byte-wise model as the shipping contract and pins
`tile_index + VIEW_TILE_H` as a required token so the column-major step cannot
regress silently.

**Still on the table.** (The per-pixel floor claim here was too pessimistic — see
the point-blank section below, which halved the worst frame by resolving texels
per source row instead of per screen row.) Two per-byte tests survive
that are loop-invariant per object: the `post_offsets == NULL` check (twice) and
`has_door_overlay`. Specialising the loop would cost source duplication for maybe
4-8%. The larger remaining number in the spin scenario is not billboard at all —
`pack` measures **6.4 vb/rebuild on `barrel-spin` vs 3.0-3.7 on the other start-room
routes**. That was the next thing looked at for turning smoothness — see the pack
section below; the cause turned out to be the mixed-tile count doubling, not the
coherence cache, and the hotpath is now ~11-13% faster.

### Point-blank sprites: the tail, not the mean (2026-08-04)

The byte-wise rewrite above fixed the *average*, and the report came back that a
barrel right in your face was still slow. It was: **route averages were hiding a
30x tail.** `bb WORST frame` (max billboard subticks and bytes in any one scene
frame, always on) exists for this — `tour-east-combat` averaged 0.60 vb but its
worst frame spent **18.5 vb**. Always look at the worst frame for anything whose
cost scales with on-screen area.

`tools/routes/barrel-pointblank.txt` reproduces it deliberately: turn to heading
168 (the bearing from the E1M1 start to the barrel at 864,3328), walk until the
barrel's collision stops you ~51 units away, then turn back and forth in place.
Baseline worst frame: **25.9 vblanks of billboard alone**, 7750 pixel slots.

Attribution (`-DCADENCE_BB_SPLIT=1`): per-object setup is only **35-42
subticks/object, ~10%**; the row loop is ~90% at ~3-4 subticks per pixel slot,
and its single most expensive item is the two dependent loads of
`lut[tex_row[tex_x] & 0x0F]`. So the lever is doing that resolve fewer times, not
trimming setup.

**Fix: a gated magnified path.** A 32-row atlas patch stretched over ~110 screen
rows means ~5 consecutive screen rows resolve identical texels.
`gather_sprite_row` resolves one *source texel row* into packed bytes once and
`apply_sprite_row` stamps it onto every screen row mapping to it, additionally
storing fully-opaque tiles as one `move.l` instead of four byte RMWs (a magnified
sprite is mostly solid interior).

Same-source A/B (`-DBILLBOARD_MAGNIFIED_STEP=0` disables the path), worst frame at
**identical workload** (3875 bytes / 7750 px both sides):

| route | path off | path on |
|---|---|---|
| `barrel-pointblank` worst frame | 33155 (25.90 vb) | **15345 (11.99 vb)** |
| `stationary-combat` avg | 2546 | 2516 |
| `checkpoints` avg | 1376 | 1356 |

**-54% on the frame that hitches, -1% (neutral) everywhere else.**

**The gate is the whole point — do not remove it.** Applying the gather/apply
split unconditionally was measured at **+44% on `stationary-combat`** (2523 ->
3644): materializing the row costs a store and a reload per byte, and an
unmagnified sprite gathers every row anyway, so it is a straight loss. The gate
is `tex_y_step <= 0x8000` (each texel row covers >= 2 screen rows) plus at least
one full tile of width. `raster_sprite_row` remains the default path.

Cost: 260 bytes of work RAM for the row scratch (23596 -> 23336 free).

**Route averages on `barrel-pointblank` are not comparable across builds** — the
two A/B builds ran 110 vs 89 iterations and ended at different poses, so `avg
vblanks` and `cast` there mean nothing (the usual hazard). The worst-frame
comparison above is valid because both sides rasterized the identical 7750 pixel
slots. `stationary-combat` and `checkpoints` stayed pose-stable (identical byte
counts, identical `avg vblanks`), which is what makes their neutrality claim safe.

Verified byte-exact with `-DBILLBOARD_RASTER_VERIFY=1`: 0 mismatches across all
six routes, both paths.

## Visible-subsector billboard cull: conservative prototype, no-go (2026-07-29)

The tempting point-owner oracle was real: on normal-skill `checkpoints.txt`,
the cast visited 22.2 subsectors/rebuild and only 7.0 of the 58 active
billboards belonged to a visited leaf. It is **not** safe to cull from that
fact alone: a sprite near a BSP partition can project across into a visible
leaf even when its anchor point belongs to one skipped by solid coverage.

`BILLBOARD_VISIBLE_SUBSECTOR_CULL=1` is therefore a default-OFF prototype.
Before it skips an object, `bsp_find_subsector_with_margin` proves that a
96-world-unit disk (the largest 3x source-patch horizontal extent is 93) stays
on the same side of every root-to-leaf BSP partition. `test-bsp-render-math.py`
proves the L1 normal bound is conservative. The correct fallback is always the
old full active-list projection.

That proof is too conservative for E1M1: both `checkpoints.txt` and
`tour-east-combat.txt` measured only **1.0 footprint-safe, safe-to-skip object
per rebuild**. Keep the flag OFF; the extra bitset/cache/traversal cannot repay
one avoided billboard measurement. Do not re-enable or tune the radius as a
perf win. A future retry needs a different, rigorously validated multi-leaf
membership model plus same-pose visual differential captures, not point-owner
culling.

Frame time is quantized to whole vblanks via `VDP_waitVSync` — a CPU-side
saving only matters if it's large enough to remove a whole vblank from the
sampled window. Small percentage wins in one stage's subticks routinely show
up as **zero** change in `average_vblanks_x10` / `missed_deadlines`.

## RAY_COL_STRIDE 4 shipped, then REVERTED to 2 (2026-07-22 -> 2026-07-27)

**The default is stride 2 and must stay there absent a new user decision.**
Stride 4 shipped in a959edd on a preview-ROM eyeball ("no visual downgrade")
and the user rejected it five days later after playing it: the walls are "muito
pixelizado". This was a *taste* verdict, not a bug — do not treat the revert as
a defect fixed and do not re-flip the default as a perf win. `raycast.h` keeps
the `#ifndef` guard, so `EXTRA_FLAGS="-DRAY_COL_STRIDE=4"` still rebuilds the
fast profile for measurement.

Lesson worth more than the numbers: **a static preview screenshot does not
qualify a stride change.** Nothing in the pipeline interpolates horizontally
between sampled columns — `write_mixed_stride4_tile` stores each
FREEDOOM_WALL_PACKED_PAIRS byte (already one texel doubled) to two adjacent
byte lanes, flats use `REP4[c] = c*0x1111`, doors mask a 16-bit lane — so
RAY_COL_STRIDE *is* the wall's horizontal resolution: 40 columns over 160px at
stride 4. That reads as acceptable in a still and as chunky once the camera
moves. Any future stride/quality flip needs the user judging it in motion.

The A/B numbers stay on record because they make stride 4 a known, costed
option. Same-pose A/B on checkpoints, stride 2 -> 4: per-rebuild stage sum
11614 -> 9321 subticks (9.07 -> 7.28 vb, ~-1.8 vb/motion frame): cast 5755 ->
4777, pack 3761 -> 2478 (coherence cache recovered 361 of the prototype's
2839), samples 80 -> 40. Idle stays 30fps either way. The tour-east-combat A/B
DIVERGED (frame-timed inputs + changed cadence = different trajectory), so only
checkpoints is a valid A/B. The stride-4 correctness work also stands: the
coherence-cache port was byte-verified against the cache-less prototype (scene
pixels identical on all 12 route frames).

Post-revert cadence probe on checkpoints (stride 2, 2026-07-27, 18 iterations):
avg 10.17 vblanks/frame (5.9fps), max 13, 17/18 rebuild frames missed the
2-vblank budget; idle still hits hist[2] (30fps). This matches the pre-a959edd
"Real release framerate" baseline above (~12-13 vb) rather than the stride-4
~8.6vb figure — treat 10.17 vb as the current motion-frame ground truth.

Shared plumbing kept from that work: the tile-column coherence cache and the
door/overlay repack contract now live outside the stride branches
(PACK_LANES = 8/RAY_COL_STRIDE), and the stride-4 packer reuses the stride-2
FREEDOOM_WALL_PACKED_PAIRS table (the older `FREEDOOM_WALL_PACKED_COLUMNS` it
had bit-rotted against no longer exists). The asm mixed-tile hotpath and the
DEBUG_PERF pack oracles are stride-2-only, so the shipped profile is once again
the one with a byte-compare oracle behind it. Stride-sensitive guardrail models
(test-bsp-render-math, test-billboard-projection, test-wall-quality) are back at
stride 2; test-sector-map's assert tracks the `descriptors[0]` refactor, which
is shared by both branches and stays. `test-billboard-raster.py`'s `STRIDE = 4`
is an unrelated miniature model (VIEW_W = 32), not this constant.

## draw_seg release-speed split + strength reductions (2026-07-22)

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

## Cast internals split + box projection rewrite (2026-07-21)

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

## Background upload pump (shipped 2026-07-21)

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

## Billboard projection spatial pre-cull: tried and rejected (2026-07-21)

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
across most columns. The oracle is DEBUG_PERF-only and free to leave in.

### Descriptor quantization for cache reuse: STRUCTURALLY DEAD (do not retry)

`top & ~1` / `tex_x & ~1` in `describe_textured_column` shipped as `e29d6aa`
(2026-07-21) and was reverted 2 minutes later by `08e1357`. A field-diff
diagnostic measured `tex_x` as differing on 84–94% of would-be cache hits and
top/bottom/tex_x jointly on 57–94%, which looked like a big lever. The measured
win was `pack_subticks` **−1.3%** on checkpoints and −7.7% on
stationary-combat — far below the ceiling, and the user reported it introduced a
**wobble on the walls**.

**Why the ceiling estimate lied, quantified (2026-07-30).** Derive the actual
per-rendered-frame pose delta from `player_controller.c`: `elapsed_frames =
min(elapsed_vblanks, 4)` (`main.c:289`) feeds the Doom tic accumulator, which at
that clamp runs **~2.33 movement tics per rendered frame**; steady-state
momentum `thrust*8*32/3` gives ~11.8 world units/tic walking and ~23.5 running.
So between two consecutive rendered frames the player translates **27 (walk) to
55 (run) world units and turns up to 16 angle units = 22.5°**. At ~5 fps
consecutive frames are nearly uncorrelated views: a wall at depth 300 moves
`top` by ~6 px and `tex_x` by many texels. **The quantum needed to collapse a
frame-to-frame diff is ~8 px / ~8 texels** — which is the stride-4 "muito
pixelizado" rejection rotated into the vertical axis.

`& ~1` was therefore off by an order of magnitude, not a factor of two. Widening
the mask, adding a ±N dead-band, or putting hysteresis in `wall_desc_equal` all
fail identically. So does quantizing the render pose for whole-frame or
whole-layer reuse — same arithmetic, plus freezing the wall layer while sprites
keep moving desynchronizes sprite scale/occlusion from the walls behind them.
**Temporal coherence does not exist at this frame rate, and using temporal
coherence to raise the frame rate is circular.** Do not re-attempt.

### The wobble taxonomy (why that attempt looked bad, and what may not)

Two artifact classes get conflated. Keep them apart when judging any fidelity
trade:

1. **Spatial incoherence** — wobble / swim / ragged silhouette. Each output
   sample is quantized *independently*, so adjacent columns of one wall land in
   different buckets: a straight top edge becomes a staircase whose steps move
   independently frame to frame, and a smoothly sliding texture gets per-column
   lag. This is what `top & ~1` / `tex_x & ~1` produced — they quantized the
   *outputs* of a smooth interpolant, per column, in screen space.
2. **Bounded monotone error** — a sub-texel offset. The interpolant's *inputs* or
   its perspective-correction *schedule* are coarsened, but every column of a
   wall is still an exact monotone ramp, so the wall keeps its shape. Doom itself
   shipped this.

**Rule: quantize the interpolant, never its per-column outputs.** Anything
decided per screen column produces class 1. Anything decided per seg-span, with
monotone interpolation inside, produces class 2.

### Where cast time actually goes (same-pose, 2026-07-30)

Measured with `-DCADENCE_DRAWSEG_SPLIT=1` on `stationary-combat.txt`, which is
**pose-proof**: its input script has no UP and no LEFT/RIGHT, so the camera
cannot move and the route reports `rebuild frames = 1` — one wall rebuild at the
exact start pose. Traversal counters (`nodes visited / boxes projected / segs
tested / segs drawn`) being identical across builds is the proof the pose
matched. This is the A/B route to use for any change that alters frame cost;
"only checkpoints is a valid A/B" applies to routes that *move*.

| part of cast | subticks/rebuild | share | per unit |
|---|---|---|---|
| node traversal + `project_box_range` | **3640** | **55%** | 59 nodes, 61 boxes |
| `draw_seg` setup (cull/clip/project/reciprocals) | 1340 | 20% | 31.2 / seg tested |
| `draw_seg` sample loop | 1675 | 25% | **20.9 / sample** |
| total (probe-inflated; 6163 without it) | 6655 | | |

The decisive number for span-based optimizations: **80 samples across 18 drawn
segs = 4.4 samples per seg.** Doom's affine texture spans pay because it had
16-pixel runs; here a K=4 sub-span barely has an interior, so ~2 of every 4.4
samples still need the exact chain. Ceiling for piecewise-affine perspective is
therefore ~0.3–0.5 vb of a 12.9 vb frame (2–4%) — and frame time quantizes to
whole vblanks, so it would very likely measure as **zero**.

### Traversal split: no hot spot in there either (2026-07-30)

The 55% block above was then instrumented from the inside with
`-DCADENCE_TRAVERSE_SPLIT=1` (`project_box_range`, both occlusion queries, and
path counters; see `debug_checkpoint.h`). Same pose-proof route, one rebuild.
Timers cost ~5–10 subticks per bracket, so the absolute figures are inflated;
the ranges below are after subtracting that, and the split is an *attribution*,
not a release absolute. Clean cast on this route is 6235 subticks.

| inside traversal (~3640 subticks) | subticks | share |
|---|---|---|
| `project_box_range` | 1500–1860 | 41–51% |
| `g_range_closed` (solid-range query) | 0–330 | 0–9% |
| `g_all_closed` | ~0 | ~0% |
| recursion + leaf visits + seg loop | ~1450–2140 | 40–59% |

**The near-plane polygon branch is NOT the explanation — that hypothesis was
wrong.** Of 70 `project_box_range` calls per rebuild: **81% take the fast 2-DIVS
path**, 6% the expensive up-to-8-DIVS near-plane path, 11% early-out before any
divide, 1% cheap half-plane reject. Note `boxes_projected` (61) counts only calls
reaching a projection, so `box_calls` (70) is the true count. The occlusion
queries are effectively free — their measured time is almost entirely probe
overhead — so the successor-set/bitmask design is not a cost worth revisiting.

What is left is **diffuse**: ~26 subticks (~2600 cycles) per fast-path box for
6 `MULS.W` + 2 `DIVS.W` + compares, against ~1000–1200 cycles of expected
arithmetic, plus a comparable amount of plain call/recursion overhead spread
across 59 node visits, 70 boxed-child calls and ~22 leaf visits. The most
plausible remaining lever is that `project_box_range` computes everything in
`s32` while the map-bounds contract proves signed words suffice — every s32 op is
two word ops on a 68000. Optimistic ceiling ~0.5 vb, and it needs the same
differential-model treatment as the rest.

**Bottom line: the frame has no concentrated hot spot.** At 12.92 vb: pack 3.59,
cast 4.81 (box projection ~1.2, traversal overhead ~1.4, draw_seg setup ~1.0,
sample loop ~1.2), billboard raster 1.71, projection 1.39, misc/upload/logic
~1.41. Nothing exceeds 28% and every block already has an optimization pass
behind it. Summing every remaining live lever optimistically gives ~2 vb — 4.6
to 5.4 fps, imperceptible. **Reaching 20 fps needs 4.3×, which is not available
incrementally.** Further gains have to come from structural fidelity dials
(`RAY_COL_STRIDE`, view size, sprite count/size), all of which are user taste
calls, not perf decisions.

### Cast: four structural wins the subtick split could not see (2026-08-03)

The "no concentrated hot spot" conclusion above stands, but it was reached from
timers alone. Reading the **generated m68k** (`gcc -O3 -fno-web -fno-gcse
-fomit-frame-pointer -S` on `bsp_render.c`; the release build adds only `-flto`,
which changes none of this) found four wastes that no subtick bucket could
attribute, worth **cast 6463 -> 5635 subticks/rebuild (-12.8%, ~-0.65 vb)**
together. Same-pose proof on `stationary-combat.txt`: `nodes visited 54.5 / segs
tested 41.2 / segs drawn 16.3 / samples 80.0` identical before and after.

1. **The full-coverage test was in the one place it can never fire.**
   `render_node` opened with `g_all_closed()`, but its only non-root caller is
   `render_boxed_child` *after* `solid_sample_range_filled` returned FALSE —
   which proves a sample is open, which proves `g_solid_count <
   BSP_SAMPLE_COLS`. Dead on ~70 of 71 calls. Moved to the top of
   `render_boxed_child`, where it now skips a full `project_box_range` for every
   sibling still pending on the stack once the view closes: `box_calls` 64.5 ->
   59.5 per rebuild.
2. **The `bsp_traverse.h` callback vtable had exactly one caller passing one
   fixed set** and GCC would not devirtualize through the recursion — every
   node, box and leaf paid `move.l g_range_closed,%a0 / jsr (%a0)` plus argument
   pushes, ~155 indirect calls per rebuild, for callees as small as
   `cmp.w #79,g_solid_count`. Header deleted, callees called directly. This was
   the largest of the four.
3. **The per-frame column seed was ~100% dead stores.** `bsp_cast_frame` wrote
   all 14 `RayColumn` fields for all 80 samples (~264 cycles each, ~21k
   cycles/rebuild) and `draw_seg` overwrote nearly all of them. A temporary
   probe counted unclaimed columns across all four routes including the
   197-rebuild `tour-east-combat`: **zero, every frame** — E1M1 is enclosed, so
   walls always claim all 80. Now only `door.height` (the sentinel every door
   consumer short-circuits on) is cleared up front; the wall fields are applied
   post-traversal to the complement of `g_solid_words`, which is normally empty.
   `seed_unclaimed_columns` stays as the correct fallback for a view that does
   not close; do not delete it as "unreachable".
4. **`sizeof(BspNode)` was 28**, so every node visit emitted `MULU.W #28`
   (~70 cycles) plus a separate shift/add chain for the same index. Now
   `__attribute__((aligned(32)))` — one `LSL.L #5`. Costs 944 bytes of **ROM**
   (`bsp_nodes` is const), not work RAM. Expressed as an alignment, not a pad
   member, so the generated maps keep positional initializers.

**Do not try to fix a wide store by writing a whole-struct constant copy** —
`columns[c] = k_seed;` compiled straight back to the same 14 individual
`clr.b`/`move.w`. This m68k backend has no store-merging pass; the only way to
cut store traffic is to not perform the stores.

**Cross-build pixel diffs are not available as a guardrail here.** Captures are
keyed to emulator frames, and a faster ROM lands a *different game frame* on
each one — even `stationary-combat.txt` (pose-fixed) produced a different
capture set. Verify cast changes with the traversal counters as the pose proof
plus an in-ROM probe, the way point 3 above was verified.

**Evaluated and not taken:** replacing `g_node_side_generation` (`u16[640]`,
1280 B of work RAM) with a second 80-byte "computed" bitmap cleared on position
change. It is a genuine **~1.2 KB work-RAM** win and removes a word load/compare
per node, but the speed effect is ~5 subticks — under measurement noise — and
the cache has subtle invalidation semantics. Take it if work RAM ever gets
tight (it is at 23596 B free against a 20480 B guardrail), not as a perf lever.

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
