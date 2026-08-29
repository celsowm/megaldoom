# Wall Texture Quality Improvement Plan

## Status

Proposed implementation plan for materially improving wall texture fidelity in MegaLDOOM without sacrificing the project's current performance discipline or breaking the existing renderer architecture.

This plan is intentionally separate from the Doom-style weapon bob work. It focuses only on wall texture representation, conversion, palette use, sampling quality, rendering cost, and visual validation.

**Reviewed 2026-08-28 against the tree.** The diagnosis and the vertical-first
ordering hold up and are backed by numbers (2, 5.2.1). Three corrections were
folded in:

- Phase 1 as originally written **exceeds the 4 MB ROM cap**; it now depends on a
  prerequisite commit that makes the packed door plane sparse (5.6).
- The benefit band is bounded to walls closer than ~160 world units, and past that
  the change *raises* vertical churn — so Phase 4's motion/churn gate moved into
  Phase 1's acceptance criteria (5.2.2, 5.7.1).
- Phase 2 is **blocked**, not merely sequenced: one 16-entry palette is shared by
  walls, flats, the weapon overlay and every billboard (6.0). Phase 5's "30 fps"
  gate was replaced with a regression bound against measured cadence (9.8).

---

## 1. Goal

Improve wall textures so that they read much closer to the original Doom materials on Mega Drive hardware, especially at short and medium distances, while preserving stable gameplay performance.

The target is not "make the image smoother at any cost." The target is:

- preserve recognisable Doom wall material structure;
- reduce blockiness on near walls;
- reduce palette collapse where distinct materials become visually similar;
- reduce horizontal column duplication artifacts;
- preserve correct texture alignment and repeat periods;
- avoid temporal crawling or shimmering introduced by aggressive offline processing;
- keep wall rendering deterministic and practical on the 68000 + VDP;
- retain the current BSP renderer and fixed-point architecture;
- measure every quality improvement against CPU, ROM, RAM, DMA, and frame-cadence cost.

---

## 2. Current Baseline

MegaLDOOM currently uses the following wall pipeline:

```text
Original Doom texture
        |
        v
offline WAD extraction
        |
        v
material-specific preprocessing
        |
        v
64 x 64 runtime texture
        |
        v
16-color PAL3 quantization
        |
        v
precomputed shaded packed wall data
        |
        v
RAY_COL_STRIDE = 2
        |
        v
80 sampled columns
        |
        v
2-pixel horizontal replication
        |
        v
160 x 120 viewport
```

The current implementation already contains substantial quality-oriented preprocessing:

- tone/gamma compensation;
- per-material contrast normalization;
- spatial smoothing;
- Oklab-aware palette selection;
- warm/earth material classification;
- protection against walls collapsing into floor/ceiling palette indices;
- fixed world palette stability rules;
- special recipes for technological wall materials;
- source-window selection for oversized textures;
- shade chains that avoid black and flat colors;
- deterministic quality tests and visual bake preview tooling.

These improvements are useful, but they are increasingly compensating for structural limitations rather than fixing the source of the quality loss.

### Measured facts behind this plan

These were verified against the tree, not assumed:

- 48 of the 53 entries in `FREEDOOM_WALL_TEXTURE_HEIGHT` are **128 tall**;
  the remaining five are 72/72/72/56 (`DOOR1`, `DOOR3`, `EXITDOOR`, plus one 56).
- `tools/world_assets.py` line ~723 does
  `opaque.resize((WALL_TEX_DIM, WALL_TEX_DIM), Image.Resampling.BOX)`,
  so half of every 128-tall source's rows are averaged away before quantization.
  **The vertical loss is real source data, not a theoretical limit.**
- The visible wall span is clamped to `RAY_VIEW_ROWS` (120), but the renderer
  preserves the unclipped projected height for vertical texture lookup. At the
  near clip, `height = 10240 / depth` reaches 640 pixels; using the clipped 120
  px value as the texture scale would stretch the entire door texture across
  the viewport. The generated lookup table therefore covers projected heights
  1..640 and selects the central visible slice for near walls.
- `FREEDOOM_WALL_PACKED_PAIRS` is `[2][4][53][64][64]` = **1,736,704 bytes**.
- `out/rom.bin` is **2,752,512 bytes** against `$RomMaxBytes = 4 MB` in
  `tools/check-rom.ps1` — roughly **1.38 MB of headroom**.

The main structural constraints are:

1. all wall textures are forced into a square 64 x 64 runtime representation;
2. the viewport is 120 pixels high, so near walls can stretch 64 vertical texels across almost twice as many display pixels;
3. walls are sampled horizontally every 2 pixels;
4. one 16-color palette line must represent a wide variety of world materials;
5. runtime shading further reduces the number of visually distinct colors at distance;
6. some original Doom textures are much wider than 64 pixels and lose structure when reduced.

The next quality improvements should therefore change the representation and renderer deliberately instead of continuing to add more per-material conversion heuristics.

---

# 3. Design Principles

## 3.1 Fix structural loss before adding more heuristics

Do not add another material-specific bake rule unless a measurable artifact remains after the runtime representation has been improved.

Preferred order:

```text
representation
    -> palette allocation
    -> sampling
    -> runtime shading
    -> only then material-specific exceptions
```

## 3.2 Preserve performance checkpoints

Every phase must be independently benchmarkable and independently revertible.

No large quality change should combine:

- texture format changes;
- palette changes;
- stride changes;
- hotpath rewrites;
- and asset algorithm changes

in the same commit.

That would make performance and visual regressions impossible to attribute correctly.

## 3.3 Prefer offline cost over runtime cost

The Mega Drive has abundant cartridge ROM relative to 68000 time.

If a quality improvement can be produced offline and stored in ROM without increasing the inner rendering loop, that is usually preferable.

Examples:

- precomputed vertical sampling tables;
- pre-shaded texture variants;
- per-texture metadata;
- prepacked column layouts;
- source-window selection;
- palette classification metadata.

## 3.4 Visual fidelity is not equivalent to source-pixel fidelity

A direct nearest-color conversion of the original PC texture is not automatically the best Mega Drive result.

The correct output is the one that preserves:

- major edges;
- material identity;
- panel divisions;
- lights;
- trims;
- readable contrast;
- stable appearance in motion.

Small high-frequency PC details that alias badly at 160 x 120 may need controlled offline simplification.

---

# 4. Phase 0 - Establish a Visual and Performance Baseline

Before changing the renderer, freeze an objective baseline.

## 4.1 Reference viewpoints

Create a deterministic set of wall-quality checkpoints across at least E1M1 and E1M2.

The set should include:

- a wall almost filling the viewport;
- a medium-distance corridor;
- a long-distance corridor;
- oblique walls;
- brown wall materials;
- neutral grey/metal materials;
- technological panels;
- doors;
- switches;
- walls next to floor colors that previously caused visual merging;
- a scene with strong N/S side shading;
- a scene with several distinct wall materials visible at once.

Each checkpoint should record:

```text
player x
player y
player angle
map
expected visible materials
```

## 4.2 Automated captures

Most of this infrastructure already exists and should be wrapped, not reinvented:

- `tools/routes/*.txt` — deterministic canned routes;
- `tools/test-checkpoints.ps1` — checkpoint runner;
- headless screenshots via BlastEm's `--md-capture-dir`;
- `tools/decode-cadence.py` — already emits per-stage cast/pack/projection/
  billboard subticks, per-rebuild averages, and the worst single frame.

Extend that tooling so a candidate wall pipeline can generate deterministic comparison images.

Suggested outputs:

```text
artifacts/wall-quality/baseline/<checkpoint>.png
artifacts/wall-quality/candidate/<checkpoint>.png
artifacts/wall-quality/diff/<checkpoint>.png
```

## 4.3 Performance baseline

`tools/decode-cadence.py` already produces most of this list; the report command
in section 16 should call it rather than re-derive the numbers.

Record at minimum:

- BSP cast subticks;
- pack subticks;
- upload/DMA cost;
- average frame cadence;
- worst checkpoint frame cadence;
- ROM size;
- generated wall-asset size;
- renderer RAM usage;
- number of sampled wall columns;
- number of packed mixed tiles;
- number of skipped columns from coherence caching.

## 4.4 Acceptance gate

No later phase should be merged without a direct comparison against this baseline.

---

# 5. Phase 1 - Decouple Wall Texture Width and Height

## 5.1 Motivation

The current format assumes:

```c
#define WALL_TEX_DIM 64
```

and uses that dimension for both axes.

This is unnecessarily restrictive.

A near wall can be approximately 120 pixels tall in the current viewport. Mapping only 64 source texels over that height guarantees visible vertical duplication.

The first structural change should therefore be:

```c
#define WALL_TEX_WIDTH  64
#define WALL_TEX_HEIGHT 128
```

instead of a single square dimension.

## 5.2 Why 64 x 128 first

This is intentionally not 128 x 128.

The current renderer only samples 80 horizontal columns because `RAY_COL_STRIDE == 2`, so increasing stored horizontal resolution before increasing sampled horizontal resolution gives limited benefit.

Vertical resolution is different: the renderer may draw approximately 120 vertical pixels from one wall column.

Therefore:

```text
64 x 64
    -> insufficient vertical information for near walls

64 x 128
    -> enough vertical information to approach 1 texel per display pixel
       for the closest practical wall slabs
```

This targets the highest-value quality loss first.

## 5.2.1 Per-axis Nyquist check (why vertical and not horizontal)

At the nearest un-clamped distance (`depth` ~ 85, where the projected height hits
the 120-row clamp) a wall filling the viewport gets:

```text
vertical    64 stored texels  ->  120 output rows   (starved 1.875x)
horizontal  ~85 stored texels ->  ~80 sampled cols  (essentially at Nyquist)
```

Vertical storage is the binding limit. Horizontal storage is not: `RAY_COL_STRIDE`
already caps the horizontal information at 80 samples, so widening storage alone
(section 7.2) buys almost nothing until stride changes too.

## 5.2.2 The benefit band is bounded — state it honestly

Because `height = 10240 / depth` and clamps at 120:

```text
depth <=  85  ->  height clamped at 120, full 2x benefit
depth == 160  ->  height 64, benefit reaches zero
depth >  160  ->  no benefit; the extra stored rows are pure undersampling
```

Near walls dominate screen area when they occur, so the win is large where it
applies — but this phase does **not** improve walls generally, and past ~160
world units it can make things worse (see 5.7).

## 5.3 Required runtime changes

Replace square assumptions with axis-specific constants.

Likely areas:

- `src/raycast.h`;
- `RayColumn` texture coordinate masks/types if needed;
- vertical sampling tables — generated by `tools/generate-renderer-assets.ps1`
  (`MEGALDOOM_WALL_TEX_Y_BY_HEIGHT[641][120]`; stays `u8`, since 0..127 still
  fits, while the extra rows preserve the near-wall crop);
- **`src/renderer/renderer_hotpath.s` line ~121, `andi.w #63,d5`** — the vertical
  wrap is hardcoded in the assembly. Missing this is silent texture corruption,
  not a build error. Any change here must read back `asm_mismatches` /
  `asm_canary_failures` / `asm_checked_tiles` from a `DEBUG_PERF` run;
- renderer wall descriptors;
- door sampling (`src/renderer/renderer_doors.c` derives `DOOR_FRAME_TEXELS` and
  `DOOR_SAFETY_TEXELS` from `WALL_TEX_DIM` on both axes — split them);
- packed wall generation;
- generated wall texture declarations;
- build-time assertions;
- quality tests.

Phase 1 is **RAM-neutral**: every table involved is ROM-resident, so the 64 KB
work-RAM budget is untouched. Record that explicitly in the section 13 table
rather than leaving the row blank.

Introduce:

```c
#define WALL_TEX_WIDTH 64
#define WALL_TEX_WIDTH_MASK (WALL_TEX_WIDTH - 1)

#define WALL_TEX_HEIGHT 128
#define WALL_TEX_HEIGHT_MASK (WALL_TEX_HEIGHT - 1)
```

Do not continue using one mask for both axes.

## 5.4 Vertical sampling tables

The current precomputed vertical sampling approach should be preserved.

Generate:

```text
projected wall height
        -> source Y lookup table
```

for `WALL_TEX_HEIGHT = 128`.

The inner renderer loop should still perform only a table lookup and mask/add operation rather than a divide.

**Generate the far-wall rows onto a fixed sub-lattice.** For projected heights
below ~64 a 128-row texture is strictly undersampled, and point-sampling twice as
many candidate rows raises vertical churn (see 5.7). Emit those rows of the table
so they land on a fixed even-row lattice, i.e. distant walls sample no worse than
today's 64-row behaviour. This costs nothing at runtime — it is purely how
`tools/generate-renderer-assets.ps1` fills the table — and it converts a
regression risk into a no-op.

## 5.5 Asset generation

Update `tools/world_assets.py` so conversion generates 64 x 128 wall textures.

Important: do not simply resize every source texture blindly to 64 x 128.

The converter should preserve original aspect and world repeat semantics through explicit metadata.

For textures whose original height is 128, the new representation can preserve native vertical structure directly.

Only five textures are not 128 tall, and three of them are doors:

```text
DOOR1     64 x 72
DOOR3     64 x 72
EXITDOOR 128 x 72
(+ one 56-high entry)
```

This is the riskiest case in the phase, not a footnote: doors are exactly where
misalignment is visible, `EXITDOOR` is already exempted from the churn ceiling in
`tools/test-wall-quality.py`, and 72 -> 128 means upscaling rather than recovering
data.

Decide it explicitly rather than leaving it open. The recommended default is
**retain the native row count with an explicit vertical sampling scale** (the
`USCALE_Q12` treatment, applied to V), because it neither invents detail nor
shifts door alignment. Do not blanket-resize 72-high sources to 128.

## 5.6 ROM cost — this phase does NOT fit as written

The naive version of this phase does not build. The arithmetic:

```text
FREEDOOM_WALL_PACKED_PAIRS  [2][4][53][64][64]   = 1,736,704 B
FREEDOOM_WALL_TEXTURES         [53][64][64]      =   217,088 B

doubling height                                  = +1,953,792 B

out/rom.bin today                                = 2,752,512 B
tools/check-rom.ps1 $RomMaxBytes                 = 4,194,304 B
headroom                                         = 1,441,792 B
```

+1.95 MB against 1.38 MB of headroom lands the image near **4.7 MB** and trips the
guardrail. Any plan that says "flip the constant and regenerate" is wrong.

### 5.6.1 Required prerequisite: make the door-style axis sparse

`FREEDOOM_WALL_PACKED_PAIRS`'s leading `[2]` is a door plane: a purely geometric
border + warning-stripe mask applied in `tools/world_assets.py` (~lines
1087-1103). It is generated for **all 53 textures** but only ever indexed when
`RAY_COLUMN_FLAG_DOOR` is set — so roughly 45 of those 53 style-1 planes are dead
weight, about 868 KB of ROM that can never be read.

Index the door plane through a small door-texture table instead:

```text
64 x 128 with a sparse door plane
    (4 * 53 + 4 * ~10) * 8192   ~=  2.06 MB
    vs 1.66 MB today            ~=  +0.4 MB net
```

That fits inside the existing headroom with room left over, and it is a smaller,
lower-risk change than the "base texture + shade LUT" restructure sketched in
section 13.1 — which would also add an indirection to the packer's inner loop,
where measurement says there is little slack.

Do this as its own visually-neutral commit **before** touching the height
constant, and confirm byte-identical rendering across the checkpoint route.

## 5.7 Acceptance criteria

Phase 1 succeeds if:

- near-wall vertical blockiness is visibly reduced;
- texture alignment remains correct;
- no door/switch alignment regression is introduced;
- no new vertical shimmering appears in motion;
- gameplay cadence remains within the agreed performance budget;
- quality tests pass without adding broad new smoothing heuristics;
- ROM stays under the 4 MB cap enforced by `tools/check-rom.ps1`;
- `asm_mismatches` and `asm_canary_failures` are zero on a `DEBUG_PERF` run.

### 5.7.1 The churn gate belongs here, not in Phase 4

Doubling stored rows improves walls inside ~160 units and makes everything past
that undersample harder. The prior "ugly walls" investigation concluded the actual
defect was **churn — the share of adjacent texels whose palette index differs —
not resolution**, and the metric already exists (`tools/test-wall-quality.py`,
`CHURN_LIMIT = 0.35`, with `EXITDOOR`/`TEKWALL2`/`TEKWALL5` exempt).

So Phase 4's motion testing cannot wait three phases. Phase 1 must gate on:

- the existing churn ceiling, measured on the **vertical** axis as well as the
  horizontal one;
- at least one motion route through a long corridor, checked for vertical crawl;
- the user's play-tested verdict in motion. A still that looks sharper is not
  evidence — that mistake has already been made once on `RAY_COL_STRIDE` 4.

---

# 6. Phase 2 - Re-evaluate Palette Ownership

## 6.0 Status: BLOCKED on palette separation

There is no per-surface palette in this renderer. `build_world_palette()` in
`tools/wad-map-extract.py` builds **one** 16-entry `FREEDOOM_WORLD_PALETTE` from a
histogram pooling wall textures, weapon sprites, world sprites and every flat.
`renderer_flats.c`, the weapon overlay, and every `billboard_*` visual (barrels,
pickups, enemies) quantize against that same array.

A change aimed at "walls read better" is therefore never a walls-only change.
Three separate attempts in this area have already been rejected in play-testing,
including ones framed as "just a floor index" and "just a curve on one input".

This phase is not merely sequenced after Phase 1 — it is **blocked** until the
weapon/sprite consumers are moved off the shared line. Section 6.3 correctly notes
that this plan must not depend on the weapon-bob migration; the honest consequence
is that Phase 2 cannot start until some equivalent separation lands.

## 6.1 Motivation

The current world palette is frozen and must represent too many material families at once.

A single 16-color line has to provide useful colors for:

- neutral metal;
- grey stone;
- brown/earth walls;
- green technological details;
- blue details;
- red indicators;
- floor/ceiling separation;
- wall shading;
- other world objects that share the line.

This creates inevitable palette collapse.

The converter now contains significant logic whose purpose is to work around these collisions.

## 6.2 Inventory all four VDP palette lines

Before redesigning PAL3, document exactly what owns every palette line and every index.

Create a table like:

| Palette | Current Owner | Dynamic? | Transparent index constraints | Candidates for migration |
| --- | --- | --- | --- | --- |
| PAL0 | ... | ... | ... | ... |
| PAL1 | ... | ... | ... | ... |
| PAL2 | ... | ... | ... | ... |
| PAL3 | world | ... | ... | ... |

The weapon-bob plan may move first-person weapon rendering from BG_A to hardware sprites. That change may create an opportunity to change which palette line the weapon uses, but this wall-quality plan must not depend on that migration being complete.

## 6.3 Preferred objective

Reserve as much of one palette line as practical specifically for wall/world material fidelity.

A possible target architecture is conceptually:

```text
PAL0 -> UI / common HUD
PAL1 -> HUD numbers / interface assets
PAL2 -> face / weapon / selected sprites
PAL3 -> world walls / world shading
```

The final assignment must be based on actual asset usage, not this example.

## 6.4 Redesign the world palette offline

Once palette ownership is clearer, rerun palette optimization with wall quality as a stronger objective.

The new optimizer should consider:

- usage frequency by map;
- material family coverage;
- luminance ladder quality;
- hue preservation;
- shade-chain quality;
- floor/ceiling separation;
- switch readability;
- perceptual error on reference wall materials.

Avoid optimizing only global per-pixel error. A palette that minimizes total error can still destroy a visually important but less frequent material.

## 6.5 Material-aware palette scoring

Score candidate palettes using weighted groups:

```text
brown walls
neutral walls
technology walls
doors
switches
lights
floor separation
ceiling separation
shade-chain stability
```

Reject candidate palettes that make one family catastrophically bad even if their aggregate error is lower.

## 6.6 Reduce conversion heuristics after palette improvement

After a better palette is chosen, reevaluate each existing wall-conversion workaround.

For every heuristic, classify it as:

```text
still required
no longer required
actively harmful under the new palette
```

Candidates to reassess include:

- aggressive contrast expansion;
- special warm-family restrictions;
- flat-avoidance reassignment;
- per-material smoothing;
- curated technology recipes;
- source-window exceptions.

The long-term goal is a simpler converter whose output is good because the runtime representation is good, not because many exceptions fight one another.

## 6.7 Acceptance criteria

- brown materials remain visibly brown instead of grey;
- grey/metal materials remain neutral;
- technological materials retain important green/red details;
- walls remain distinct from floor and ceiling;
- shade levels remain monotonic and hue-stable;
- overall visual quality improves at the reference checkpoints;
- palette changes do not unexpectedly recolor HUD, actors, weapon, or face assets.

The last criterion is **unsatisfiable while walls, flats, the weapon/hand overlay,
barrels and pickups share one 16-entry line** — which is the point of 6.0. Until
separation lands, the only meaningful check is a play-tested verdict: capture
frames that include the hand/gun and a barrel, and look at them. Static
per-texture metrics (chroma histograms, block-average perceptual error) have
already failed to surface this class of regression.

---

# 7. Phase 3 - Improve Horizontal Texture Representation Without Changing Stride Yet

Before doubling runtime wall samples, improve what each existing sample sees.

## 7.1 Preserve source repeat period explicitly

Original Doom wall textures have varying widths.

Do not assume every source should visually repeat every 64 texels merely because runtime storage is 64 columns wide.

Keep explicit metadata for:

- original width;
- sampled source width;
- runtime storage width;
- world-space repeat period;
- U scale.

The current `USCALE_Q12` concept should remain or be generalized.

## 7.2 Evaluate 128-wide storage selectively

For high-value materials whose visual identity depends on horizontal detail, test an optional 128-wide representation while still rendering at stride 2.

Do not globally switch every texture to 128-wide storage yet. Per 5.2.1 the
horizontal axis is already near Nyquist at 80 samples, so wider storage at stride
2 is close to a no-op in aggregate — this is a targeted experiment on materials
whose *structure* (not sharpness) is destroyed by the 2:1 squash, and it should be
held to that standard before spending ROM on it.

Candidate materials:

- COMPUTE2-like panels;
- large door textures;
- walls with multiple distinct facade modules;
- textures where a 64-wide reduction demonstrably destroys structure.

Possible format:

```c
typedef struct {
    const u8 *texels;
    u16 width;
    u16 height;
    u16 u_scale_q12;
    u8 width_mask;
} WallTextureMeta;
```

If variable runtime widths add too much inner-loop overhead, generate specialized metadata that keeps the hotpath branch-free.

## 7.3 Atlas/variant strategy

If only a small number of textures need 128 columns, represent them as two 64-column pages or a separate packed layout rather than forcing all textures to the largest size.

The goal is to spend ROM where humans can actually see the difference.

## 7.4 Acceptance criteria

- large panels keep recognizable internal layout;
- texture repeat behavior matches the original material intent;
- no per-pixel runtime branch is added to the hotpath;
- ROM growth remains bounded and documented.

---

# 8. Phase 4 - Reduce Temporal Artifacts

Static screenshots are insufficient. Wall quality must also be stable while moving and turning.

## 8.1 Identify crawling sources

Common sources include:

- stride-2 horizontal sampling;
- high-frequency post-contrast detail;
- point-sampled vertical rescaling;
- source textures whose features are close to one runtime texel wide;
- shade-level transitions;
- perspective U quantization.

## 8.2 Motion-aware quality tests

Create deterministic short camera routes and compare frame sequences.

Measure or detect:

- palette-index churn;
- single-pixel temporal toggling;
- vertical band crawling;
- repeated-column shimmer;
- texture-coordinate discontinuities across BSP segment boundaries.

## 8.3 Conservative offline filtering

Filtering should be applied only where temporal instability is measured.

Do not globally blur walls.

Prefer edge-preserving low-pass processing that preserves large structural boundaries while removing sub-pixel patterns that cannot survive the runtime sampling grid.

## 8.4 Acceptance criteria

A candidate that looks sharper in one screenshot but crawls badly during motion is not an improvement.

---

# 9. Phase 5 - Move from Stride 2 to Stride 1

## 9.1 Motivation

With:

```c
#define RAY_COL_STRIDE 2
```

MegaLDOOM renders only 80 independent horizontal wall samples across a 160-pixel viewport.

Every sampled wall column is effectively stretched across two output pixels.

This is a fundamental horizontal resolution limit.

The final major quality step is:

```c
#define RAY_COL_STRIDE 1
```

which yields 160 independent wall samples.

## 9.2 Do not flip the constant first

The current renderer contains important stride-2-specific optimizations:

- packed pair data;
- four lanes per 8-pixel tile column;
- hand-written assembly hotpath;
- coherence assumptions;
- renderer performance tuning.

A naive stride-1 build is useful only as a quality/performance experiment, not as the final implementation.

## 9.3 Build a reference stride-1 implementation

First implement a correct C reference path.

Use it to validate:

- exact wall geometry;
- texture U coordinates;
- vertical sampling;
- shade selection;
- door behavior;
- tile packing;
- visual quality gain.

Instrument it heavily.

## 9.4 Measure where the cost actually doubles

Do not assume the entire frame cost doubles.

Measure separately:

```text
BSP segment projection
sample generation
texture-coordinate interpolation
wall descriptor generation
mixed tile packing
DMA/upload
billboards
other scene work
```

Some work is per segment or per tile and will not double.

## 9.5 Optimize sample generation

Potential techniques:

- incremental perspective interpolation;
- reciprocal tables;
- packed two-column iteration;
- replacing repeated field stores with compact descriptors;
- precomputing texture metadata;
- avoiding full `RayColumn` writes where the packer can consume a smaller sampled structure;
- processing two adjacent 1-pixel samples per loop iteration.

## 9.6 Rewrite the packer for stride 1

The desired tile organization becomes:

```text
8 independent wall samples per 8-pixel tile column
```

instead of four 2-pixel lanes.

Design a new packed source representation optimized for the 68000.

Potential options:

### Option A - one packed nibble per texel

Read eight independent palette indices and compose one 32-bit tile row.

### Option B - prepacked 4-pixel groups

Precompute groups that let the renderer combine two 32-bit halves efficiently.

### Option C - pair-of-columns LUT

Precompute shaded pixel pairs for adjacent texture columns where memory cost is acceptable.

The best option must be selected from benchmarks, not intuition.

## 9.7 Assembly hotpath

Only after the C reference is correct and profiled should the stride-1 mixed-wall path be rewritten in 68000 assembly.

Required validation:

```text
C output == ASM output
```

byte-for-byte across deterministic checkpoints.

Keep the C path behind a build flag for verification.

## 9.8 Cadence target — restated against measured ground truth

The "30 fps target remains sustainable" framing this section originally carried is
not the current baseline and should not be used as a gate.

Measured release-speed ground truth at stride 2:

```text
idle frames        2 vblanks       (30 fps)
motion frames     ~12.9 vblanks    (~4.6 fps)
```

Stride 1 roughly doubles the `draw_seg` sample loop and the mixed-tile count that
dominates `pack`. It moves motion frames further from 30 fps, not toward it. The
gate must therefore be stated as a **regression bound** against measured stride-2
motion cadence on a pose-stable route, not as an absolute fps target.

## 9.9 Stride is a taste decision, not a metric

`RAY_COL_STRIDE` 4 has already shipped once and was reverted — not for a bug, but
because the user judged the replicated walls too pixelated **in motion** after a
still-screenshot preview had been approved. Nothing in the packer interpolates
horizontally between sampled columns, so this constant literally *is* the wall's
horizontal resolution.

Two consequences for this phase:

- do not re-derive a stride change from screenshots or from a perf table;
- any stride change, in either direction, needs the user judging real gameplay
  motion before it becomes the default.

Also note: only `checkpoints`-style pose-stable routes are valid for stride A/B.
Cadence changes trajectory, so free-running routes end at different player poses
and their comparisons are meaningless.

---

# 10. Phase 6 - Adaptive Horizontal Quality If Full Stride 1 Is Too Expensive

If 160 samples everywhere is too expensive, investigate selective sampling.

## 10.1 Near-wall stride 1, far-wall stride 2

Visual error from duplicated columns is most obvious nearby.

Possible policy:

```text
near depth       -> stride 1
medium/far depth -> stride 2
```

The implementation must avoid obvious vertical seams where sampling density changes.

## 10.2 Edge-sensitive supersampling

Use stride 1 around:

- strong wall boundaries;
- doors;
- switches;
- high-frequency textures;
- large perspective gradients.

Use stride 2 for visually low-information spans.

This is more complex and should only be considered if the simple depth-based approach is insufficient.

## 10.3 Checkerboard temporal sampling is not preferred

Do not alternate odd/even columns between frames unless experiments prove it is stable.

At 30 fps, temporal reconstruction artifacts could be more distracting than stride-2 duplication.

---

# 11. Phase 7 - Revisit Runtime Wall Shading

Wall shading should be evaluated after texture and palette improvements because the same LUT may behave differently with a new palette.

## 11.1 Preserve Doom-like side shading

N/S wall darkening is an important depth cue and should remain.

## 11.2 Re-evaluate distance fog

The current discrete shade levels may remove too much material detail at medium distance.

Test:

```text
side shading only
side + current distance shading
side + later distance thresholds
side + material-aware shade chains
```

Do not increase per-pixel runtime cost. Any new shade behavior should still reduce to one LUT selection per column.

## 11.3 Acceptance criteria

- distant walls retain recognizable material family;
- shade levels remain stable in motion;
- no wall shades into black holes;
- no wall shades into floor/ceiling colors;
- side shading remains visually useful.

---

# 12. Phase 8 - Simplify the Offline Converter

After the structural improvements are stable, remove obsolete complexity.

## 12.1 Audit every workaround

For each constant and special path in `tools/world_assets.py`, document:

```text
original artifact it fixed
whether the artifact still exists
whether the workaround still improves current output
```

## 12.2 Prefer generic transformations

Keep generic operations when measurable:

- source-window metadata;
- controlled tone mapping;
- perceptual palette matching;
- narrow edge-aware smoothing;
- transparent-gap handling.

Remove material-name exceptions that no longer provide measurable benefit.

## 12.3 Keep explicit exceptions only for intentional art direction

A special recipe is acceptable when it represents a deliberate choice such as:

> use the recognizable 64-pixel COMPUTE2 facade module rather than compressing four modules into unreadable noise

It is not acceptable when it merely compensates for a renderer limitation that has since been removed.

---

# 13. Memory and ROM Budget

Every phase must publish a size report.

Track:

| Resource | Baseline | Candidate | Delta |
| --- | ---: | ---: | ---: |
| wall base texture bytes | | | |
| packed shaded wall bytes | | | |
| generated metadata | | | |
| ROM image size | | | |
| static RAM | | | |
| stack high-water mark | | | |
| VRAM | | | |

## 13.1 Expected biggest growth

Measured, not estimated (see 5.6): the naive height doubling costs **+1,953,792
bytes** against **1,441,792 bytes** of headroom. It does not fit. The sparse
door-plane prerequisite in 5.6.1 brings the net change to roughly **+0.4 MB**,
which does.

Do not cite ROM size as a blocker without running `size out/rom.out` (or checking
`out/rom.bin`) against the 4 MB cap in `tools/check-rom.ps1` — the number that
matters is that cap, not where `sizebnd` currently pads.

Before accepting a large multiplication in ROM size, compare alternatives such as:

- base texture + shade LUT;
- partial prepacking;
- per-material packed representation;
- only prepacking hot/common textures;
- separate door representation.

Runtime speed remains more important than small ROM savings, but avoid exponential duplication when a compact representation offers the same hotpath cost.

---

# 14. Performance Budget

Visual quality work must preserve a defined cadence target.

The target is a **regression bound against measured stride-2 motion cadence
(~12.9 vblanks)**, not 30 fps. See 9.8. Only pose-stable routes
(`checkpoints`, `stationary-combat`) produce valid A/B comparisons.

For every phase report:

```text
average frame time
P95 frame time
worst checkpoint frame time
BSP time
pack time
upload time
ROM delta
```

Recommended merge policy:

- Phase 1 and palette work should have minimal runtime impact;
- stride-1 work may consume additional budget but must be optimized before becoming the shipping default;
- no quality change should silently lower the target cadence.

---

# 15. Testing Strategy

## 15.1 Unit tests

Add tests for:

- width/height masks;
- source-to-runtime U scaling;
- vertical sampling bounds;
- texture wrapping;
- door texture alignment;
- switch alignment;
- palette-family constraints;
- shade-chain monotonicity;
- floor/ceiling separation;
- generated asset dimensions;
- packed table sizes.

## 15.2 Golden-image tests

Use selected checkpoints for visual regressions.

Do not require byte-identical screenshots after intentional quality changes.

Instead retain approved reference images and optionally compute perceptual metrics plus human review.

## 15.3 Motion tests

Automate short deterministic routes and produce contact sheets or frame sequences.

Critical routes:

- walk directly toward a detailed wall;
- strafe along a textured wall;
- turn slowly near a technological panel;
- move through a doorway with differently textured rooms;
- approach a switch;
- run down a long corridor.

## 15.4 ASM equivalence tests

Any optimized assembly wall packer must be tested against the C reference.

The acceptance requirement is exact generated tile data, not merely similar screenshots.

---

# 16. Tooling

Extend existing tools rather than creating unrelated one-off scripts.

Likely tooling changes:

- `tools/world_assets.py`
  - non-square wall dimensions;
  - improved palette experiments;
  - revised recipes;
  - variable-width experiments.

- `tools/wall_bake_preview.py`
  - 64x64 vs 64x128 comparison;
  - candidate palette comparison;
  - source/runtime side-by-side output.

- `tools/test-wall-quality.py`
  - new format contracts;
  - quality metrics;
  - palette tests;
  - temporal artifact metrics where practical.

Add a new comparison command if useful, for example:

```bash
python tools/wall-quality-report.py
```

that produces:

```text
quality metrics
asset sizes
palette report
selected texture previews
checkpoint render comparisons
```

---

# 17. Proposed Implementation Order

## Milestone A - Baseline

1. Freeze reference checkpoints.
2. Generate baseline screenshots and motion routes.
3. Record performance and size numbers.
4. Add a reproducible quality report command.

## Milestone B - 64 x 128 Walls

0. **Make the packed door plane sparse first** — the height change does not fit
   in ROM otherwise (5.6.1).
1. Split width/height constants, including the asm hotpath's `andi.w #63`.
2. Update vertical sampling tables (far heights on a fixed sub-lattice).
3. Update converter and generated assets; keep 72/56-high textures native.
4. Update packed wall representation.
5. Validate doors and switches.
6. Benchmark ROM and frame cost.
7. Compare reference screenshots **and a motion route**; churn gate on both axes.
8. Get the user's play-tested verdict before this becomes the default.

## Milestone C - Palette Redesign (BLOCKED, see 6.0)

1. Audit palette ownership.
2. Build candidate world palettes.
3. Score by material family and reference scenes.
4. Select a new frozen palette only if clearly superior.
5. Rebuild shade chains.
6. Remove obsolete palette workarounds.

## Milestone D - Horizontal Asset Fidelity

1. Audit wide original textures.
2. Identify textures visibly harmed by 64-column storage.
3. Add selective 128-wide or paged storage experiments.
4. Preserve original repeat metadata.
5. Measure ROM cost.

## Milestone E - Stride 1 Reference

1. Add correct C stride-1 path.
2. Generate 160 samples.
3. Validate geometry and texture coordinates.
4. Measure visual improvement.
5. Profile exact CPU cost.

## Milestone F - Stride 1 Optimization

1. Optimize sample generation.
2. Design stride-1 packed representation.
3. Implement optimized C packer.
4. Implement 68000 assembly hotpath.
5. Verify C/ASM equivalence.
6. Recover target cadence.

## Milestone G - Adaptive Quality Fallback

Only if full stride 1 remains too expensive:

1. prototype near/far sampling policy;
2. validate seams and motion stability;
3. compare against full stride 1 and current stride 2;
4. ship only if the compromise is clearly better.

## Milestone H - Converter Cleanup

1. audit old heuristics;
2. delete obsolete exceptions;
3. keep only measured, intentional preprocessing;
4. update documentation and tests.

---

# 18. Suggested File Changes

Expected areas of modification:

```text
src/raycast.h
src/bsp/bsp_render.c
src/bsp/generated_assets.h          (NOT src/renderer/ — generated wall data)
src/renderer/renderer_pack.c
src/renderer/renderer_hotpath.s     (andi.w #63,d5 — hardcoded vertical wrap)
src/renderer/renderer_doors.c
src/renderer/generated_renderer_assets.h

tools/world_assets.py
tools/world_palette.py
tools/wad-map-extract.py            (owns build_world_palette — Milestone C)
tools/generate-renderer-assets.ps1  (owns MEGALDOOM_WALL_TEX_Y_BY_HEIGHT)
tools/wall_bake_preview.py
tools/test-wall-quality.py
tools/check-rom.ps1                 (4 MB cap)
tools/decode-cadence.py             (per-stage subticks, worst frame)
```

Potential new tooling:

```text
tools/wall-quality-report.py
tools/wall-motion-preview.py
```

Generated files should continue to be generated from authoritative converter inputs rather than edited manually.

---

# 19. Risks

## 19.1 ROM growth

64 x 128 textures can multiply generated wall data substantially, especially where shade/door variants are fully prepacked.

Mitigation:

- measure each representation;
- avoid duplicating data that can be recovered through cheap LUTs;
- selectively use larger representations when appropriate.

## 19.2 Stride-1 CPU cost

Doubling horizontal samples may move the bottleneck from pack/upload to BSP/sample generation or vice versa.

Mitigation:

- build the reference path first;
- profile by stage;
- optimize only the measured bottleneck;
- retain adaptive sampling as a fallback.

## 19.3 Palette regressions outside walls

A better wall palette can silently damage weapon, actors, HUD, or other world objects if palette ownership is not separated correctly.

Not hypothetical: three attempts here have already been rejected in play-testing.

Mitigation:

- explicit palette ownership audit;
- capture frames that **include** the hand/gun overlay and a barrel, and look at
  them — the weapon is drawn every frame, so every capture already contains it;
- do not modify frozen palette indices without cross-system tests;
- get the user's play-tested verdict before committing, every time. No exemption
  for "just a floor index" or "just a curve on one input".

## 19.4 Overprocessing

More contrast and smoothing can make static images look superficially clearer while destroying Doom's original material identity or creating temporal artifacts.

Mitigation:

- compare source, bake, and runtime result;
- include motion tests;
- prefer structural improvements before stronger filters.

## 19.5 Renderer complexity

Variable-width textures and adaptive sampling can make the renderer difficult to reason about.

Mitigation:

- keep the common hotpath simple;
- encode complexity offline;
- introduce variable formats only after proving visual value.

---

# 20. Definition of Done

The wall texture quality project is complete when all of the following are true:

- wall textures are no longer constrained by a single square `WALL_TEX_DIM` assumption;
- near walls retain substantially more vertical detail;
- major Doom wall materials remain recognizable after quantization;
- wall/floor/ceiling separation is robust without excessive material-specific hacks;
- technological panels retain important structure;
- horizontal 2-pixel duplication is eliminated or replaced by a measured adaptive solution;
- wall texture motion is stable without severe crawling or shimmer;
- door and switch texture alignment remains correct;
- shade behavior remains Doom-like and hue-stable;
- the converter is simpler than the current workaround-heavy state where possible;
- all generated assets are reproducible;
- C and ASM renderer paths have equivalence coverage where applicable;
- ROM/RAM/VRAM costs are documented;
- the shipping build meets the agreed frame-cadence target on real Mega Drive-compatible hardware or the project's accepted emulator/performance test environment;
- approved reference checkpoints are visibly better than the current baseline.

---

# 21. Recommended First Implementation Slice

Three commits, in this order. The height change is **second**, because it does not
fit in ROM until the first one lands (see 5.6).

### Commit 1 - sparse door plane (visually neutral)

```text
1. freeze visual/performance checkpoints
2. index FREEDOOM_WALL_PACKED_PAIRS's door plane through a door-texture table
   instead of generating it for all 53 textures
3. confirm byte-identical rendering on the checkpoint route
4. record the ROM delta (expect ~-0.85 MB)
```

### Commit 2 - 64 x 128 walls

```text
1. split WALL_TEX_DIM into WIDTH (64) and HEIGHT (128), with separate masks
2. update renderer_hotpath.s: andi.w #63,d5 -> #127
3. regenerate MEGALDOOM_WALL_TEX_Y_BY_HEIGHT, with far heights on a fixed
   even-row sub-lattice (5.4)
4. keep the 72/56-high textures at native rows with an explicit V scale (5.5)
5. regenerate wall assets
6. preserve RAY_COL_STRIDE = 2, the current palette, and current shading
7. verify: check-rom under 4 MB, asm_mismatches == 0, churn ceiling on both axes
8. compare quality, ROM size, and cadence against the frozen baseline
```

### Commit 3 - judgement

```text
1. run a corridor motion route and a near-wall approach
2. get the user's play-tested verdict in motion, not from stills
```

This isolates one question:

> How much wall quality do we recover by eliminating the 64-texel vertical
> bottleneck alone, in the depth band under ~160 units where it actually applies?

Only after that result is measured should horizontal-fidelity or stride work begin.
Palette redesign stays blocked per 6.0 regardless of this outcome.
