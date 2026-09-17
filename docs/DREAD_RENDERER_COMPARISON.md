# Dread renderer vs MegalDoom renderer — why Dread is so efficient

Study of the Dread source drop (`.externals/Dread-source-drop/`, engine in
`dreadmake/src/`, tools in `dreadtool/src/`) and comparison with our renderer
(`src/renderer/`, `src/bsp/`). Dread targets the Amiga 500 / Atari ST:
7–8 MHz 68000, no FPU, with blitter (Amiga) or CPU `movep` LUT (ST) for
chunky-to-planar conversion.

> Notes:
> - `dreadmake/src/dread_engine.h` is nearly empty (only `RAND_TAB`). The real
>   structures live in `dread_data.h` (file format), `dread_estructs.h`
>   (runtime), and `dreadtool/src/dread_render.h` (tool-side mirror).
> - The asm has build-time variants. The shipping Amiga `Makefile` sets
>   `STRETCH_SCALING=1`, `WALL_RENDERER_VERSION=1`, `USE_VIS_GROUPS=1`
>   (`dreadmake/Makefile:57,87-88`); `Makefile.ST` does not set
>   `USE_VIS_GROUPS`. Line references below follow the Amiga variant.
> - `dreadtool/src/dread_render.cpp` is the tool's C preview renderer, not the
>   engine; it is cited only where it is the clearest statement of what the asm
>   does.

## 1. How the Dread renderer works

### 1.1 Frame pipeline

```text
Game_MainLoop: VScreen_BeginFrame → Dread_RunLogic → Dread_Render → VScreen_EndFrame
```

`Dread_Render` (`dreadmake/src/dread_soft.c:567-593`) is four calls:

```text
Dread_Render()
  Dread_FrameReset()        // init 160 render_columns from render_layout_delta (dread_asm.s:95-117)
  Dread_DrawVisible(vis)    // walk this subsector's baked vis list → e_vislines, e_visthings
  Dread_DrawLinesTable()    // draw vislines in stored offline order
  Dread_DrawSprites()       // transform, depth-sort, draw far→near
```

There is **no per-frame raycast, no BSP walk for visibility, no overdraw
resolver at runtime**. The single most important efficiency fact: visibility
and draw order are an **offline bake**, and the frame is a list walk.

### 1.2 Map format and offline visibility (the core trick)

File structures (`dread_data.h:9-52`): `DataVertex{xp,yp}` shorts;
`DataLine{v1,v2,tex_upper,offs_x,ceil,floor,modes[2],tex_lower,y1..y4}` (16 bytes);
`DataBSPNode{A,B,C,left,right}` where a negative child is `~vis offset`;
`DataSubSector{visoffset,type,height}`. The header carries **two extra BSPs**:
`map_collision_bsp_root` + `map_hitscan_bsp_root` (`dread_data.h:11-12`) —
collision and hitscan are separate trees from the render BSP, so gameplay
queries never pay for render topology.

The tool (`dreadtool/src/dread_mapgen.cpp:1794-1913`, `ComputeVisibility_Rec`)
does a beam/portal flood with recursion cap depth 32, producing a
**per-region ordered draw list** (`VisLine`/`VisSector` with `draw_order`,
`dread_mapgen.h:177-199`). It is exported in one of two token encodings
(`dread_mapgen.h:396-431`): the older flat list (`ENGINE_OLDVIS_LINEMODE_BASE`
/ `SUBSECTOR_BASE` / `CONDITION_*` / `ENDLIST`) or, in the shipping Amiga
build, **grouped** lists (`ENGINE_GVIS_GROUP_CALL_BASE` / `LINE_BASE` /
`SUBSECTOR_BASE` / `CONMODE_BASE`) that let regions share common sub-lists.
Each wall is baked to 4 heights (`LoadSectorHeights`,
`dread_mapgen.cpp:81-153`): `h1` = own ceil, `h2` = other ceil, `h3` = other
floor, `h4` = own floor (one-sided: `h1` = ceil, rest = floor). Two-sided lines
whose heights and flats match on both sides (sky-aware) are not drawn
(`IsDrawn`, `dread_mapgen.h:92-103`), and are dropped from the export unless
`IsRequired` keeps them for sector type/tag or line action 200
(`dread_mapgen.h:112-117`).

Runtime (`dread_asm_draw.s:145-271`, the `USE_VIS_GROUPS=1` variant; the flat
variant is `:39-127`): one asm point-locate (`Dread_FindSubSector`,
`dread_asm.s:123-151`, 16-bit `muls` walk), then a linear walk of that
subsector's vis list. Lines append to `e_vislines`; thing-sectors expand with
`visframe` dedup; condition-off runs are skipped. `Dread_DrawLinesTable`
(`dread_asm_draw.s:279-317`) dispatches **in stored offline order** — painter's
algorithm as data, not code.

Hard caps (`dread.h:13-21`): 1024 verts, 1536 lines, 512 subsectors, 512 things,
64 machines (doors etc.), 128 conditions, **540 vislines, 64 visthings per
frame**. See-through-door chains are capped at `MAX_SEE_THROUGH=2`
(`dread_mapgen.h:31`).

### 1.3 Wall drawing: generated scalers, ~1 byte-move per pixel

Framebuffer is **160×100 chunky** (`dread_render.h:10-13`), zoom 80
(`xmin = p1x*80/p1y + 80`, `dread_asm_lines.s:243-251`).
`ZOOM_CONSTANT = (80*32)<<(8+3)` with sub-vertex precision 3
(`dread_render.h:17`). Per line: frustum clip, `s = ZOOM_CONSTANT/depth`
(16-bit `divu`, `:261-264`), `ds = (s2-s1)/xlen`, then texture-u setup with
32-bit `du` (`dread_asm_lines.s:125-361`).

`dread_codegen.c:74-161` **generates the inner loops at load time**: the
`NUM_SOFT_SIZES=2048` scale values (`dread_framework.h:23`) are bucketed
(bucket width grows as `size/30`) and each bucket gets one scaler — a run of
`MOVE.b xxx(A2),(A7)+` instructions whose displacements are the precomputed
texel rows, ending in `JMP (A6)` (`:20-72`). Flat fills (`MOVE.b D5,(A7)+`)
and the sky copy (`MOVE.b (A2)+,(A7)+`) get their own generated runs
(`:167-202`). Callers jump into the middle of a run to start at a given row
and temporarily patch a `JMP (A6)` over the instruction after the last row
to end it (`dread_asm_sprites.s:504-510` shows the trick).

Each bucket also gets a `LineSizeCheat` (`dread_codegen.c:103-156`) that
precomputes ceil/upper/hole/floor span offsets for the common wall heights
(0, −64, −96, −128) relative to the constant −40 eye height. A per-line
"cheat mode" picks one of those layouts; the line core then just `jsr`s the
chosen function (`dread_asm_lines.s:384-396`). Perspective-vs-linear texture
mapping is chosen per line by a midpoint error test
(`dread_asm_lines.s:308-329`). Wall-pixel cost is ≈ **one byte move with a
precomputed source offset** — no multiply, no divide, no table lookup in the
inner loop.

Textures are 64 texels wide; **lighting is baked as separate texture variants**
(`tex_upper` "includes light level", `dread_estructs.h:42`) — shading costs zero
at runtime.

### 1.4 Floors, ceilings, sprites, display

- **Floors/ceilings are flat-coloured**, never textured at runtime. Per-line
  `ceil`/`floor` colour bytes; per column: fill `[0,y1)`, textured upper,
  gap shrinks the sprite window (`ymin/ymax`), textured lower, fill to row 100
  (reference C in `dreadtool/src/dread_render.cpp:586-604`). Sky is a
  scrolling column copy through the generated sky run; its source pointer is
  advanced once per frame from `view_angle` (`dread_soft.c:560-564`).
- **Sprites**: things linked per subsector; pass 1 culls by `visframe` +
  transforms, then a depth sort, then far→near drawing
  (`dread_asm_sprites.s:29-211`; the second sort pass is commented out as TBD).
  The sprite column core (`_Dread_LineCore_Sprite`, `:420-521`) **reuses the
  wall scalers** through the size-cheat table, tests each column against the
  wall scale stored in `rc_size_limit`, and draws per-column opaque span lists.
- **Display**: CPU renders chunky into a 2-slot `C2P_Scene_Queue`
  (`amiga_asm.h:13`); the **Amiga blitter does C2P under IRQ**
  (`amiga_framework.s:34-38` onward) while the CPU draws the next buffer.
  Copper builds the 4-bitplane list + 16-colour palette
  (`amiga_vscreen.c:39-100`); double buffers throughout. ST does CPU/LUT
  `movep.l` C2P (`st_c2p.s:23-80`).

### 1.5 Constraints Dread accepts (this is where the speed comes from)

Fixed 160×100 (`STRETCH_SCALING=1` halves texel resolution vertically), no
pitch (only `view_angle`), flat-colour floors and ceilings, ≤540 lines / ≤64
sprites per frame, 64-px textures, baked lighting. **The renderer ignores
camera z**: `view_pos_z` moves with gravity/step-down in
`Dread_Camera` (`dread_soft.c:350-402`), but walls and sprites are drawn for
a constant −40 eye (`dread_asm_lines.s:363-380` with the `view_pos_z` path
commented out, `dread_asm_sprites.s:175` "TBD: view_pos_z", and the
`LineSizeCheat` bake). `ENGINE_MIN_ZNEAR = ZOOM_CONSTANT/65536 + 1`
(`dread_render.h:18`) keeps `s = ZOOM_CONSTANT/depth` inside 16 bits so a
`divu.w` suffices. Integer-only throughout: fix14 trig (`fn.c:4`,
`sincos_fix14`), `muls/divs/divu`, error-gated perspective skip.

## 2. How our renderer differs

| Aspect | Dread | MegalDoom |
|---|---|---|
| Visibility | Offline beam flood → ordered per-subsector draw list; runtime is a list walk (`dread_asm_draw.s:145-271`) | Live front-to-back flat BSP cast every rebuild (`bsp/bsp_render.c:bsp_cast_frame`). Pose-dependent: 8–223 nodes visited and 4–115 segs tested across seven measured vantages (LOG 2026-09-07, near-plane clamp) |
| Draw order | Baked painter order, zero runtime cost | BSP front-to-back traversal into a `RayColumn` per sample column |
| Wall inner loop | Load-time **generated** `MOVE.b` scalers (`dread_codegen.c`); ~1 byte-move/px | Hand-written `renderer_hotpath.s` stride-2 span writer over **pre-shaded pair bytes** (`FREEDOOM_WALL_PACKED_PAIRS`: shade + 2 horizontal texels per byte; `[4 shades][43 tex][64][128]` = 1,410,048 bytes, plus a 557,056-byte door table). Vertical scaling is still stepped at runtime |
| Shading | Baked texture variants (0 runtime cost) | Baked shade planes, chosen once per column (same idea, different encoding) |
| Horizontal res | 160 true columns | 80 or 88 sampled columns (`RAY_COL_STRIDE=2` over 160/176-px viewports); stride 4 was shipped then reverted — the user rejected the in-motion fidelity loss (LOG 2026-07-27) |
| Vertical res | 100 rows, texels doubled vertically | Runtime-selectable viewport 20×15, 22×15 or 22×16 tiles (buffers sized for `_MAX` 22×16); textured 64×128 walls |
| Floors/ceilings | Per-line flat colour fill; sky column copy | Also untextured: level-wide floor colour, ROM ceiling row tables (indoor pattern or sky gradient), resident VRAM ceiling atlas tiles (`renderer_upload.c`) |
| Sprites | Reuse wall scalers, span lists, depth-sorted | Separate project → measure → draw billboard pipeline every scene frame; the tail dominates (point-blank worst frame ~30× the route mean, LOG 2026-08-04) |
| Display path | Chunky render → blitter/CPU C2P overlapped with next frame | Tile-based VDP: double-buffered bank swap, full rebuild = 300-tile DMA (~2 vblanks), sparse path DMAs only dynamic runs; background V-INT pump drains the upload during the cast (`renderer_upload.c`, `renderer.h:86-93`) |
| Frame profile | No comparable measurement available | Cadence probe: rebuild = cast ~3.4–15.6k (pose-dependent) + pack ~3–5k + projection ~1.5k + billboard ~1.6–2.5k subticks; idle holds 30 fps. CPU-bound in cast+pack; DMA levers cap at ~2 vblanks |

The honest summary: **both renderers already converged on several of the same
big ideas** — bake shading offline, untextured flats, integer-only inner loops,
overlap display upload with CPU work. The remaining gap is architectural:
Dread pays **nothing per frame for visibility** (list walk), while we re-cast a
BSP every rebuild, and cast is our largest and most pose-sensitive stage.
Conversely we keep things Dread gives up: unmodified DOOM geometry (no
authoring for the engine), selectable viewport sizes, and 64×128 wall
textures without a fixed 160×100 frame. Both engines have
runtime doors; Dread handles see-through doors with baked visibility
*conditions* rather than live traversal.

## 3. What we could borrow (and what we should not)

1. **Per-region ordered vis lists (biggest lever, biggest cost).** Dread's
   offline beam flood (`ComputeVisibility_Rec`, depth cap 32,
   `MAX_SEE_THROUGH=2`) replaces per-frame traversal with a list walk. For us
   this would mean precomputing, per leaf/cluster, the ordered seg set —
   essentially a PVS over our flat BSP. It attacks cast, our largest stage,
   and would most help the worst poses (the hall vantage costs ~4.6× the door one).
   But: our maps are real DOOM geometry (not maps authored for the engine),
   doors/windows are dynamic (Dread's conditions are the closest analogue,
   and they only *skip* baked runs), a list gives a superset that still needs
   per-column occlusion to stop overdraw, and the bake must stay valid under
   `SEG_WINDOW` splits and door motion. Prototype on E1M1 with a verifier
   (same discipline as `tools/test-sector-map.py`'s solid-cover check) before
   believing any speedup.
2. **Code-generated inner loops.** Dread generates its scalers at load time;
   we hand-maintain `renderer_hotpath.s` + a C reference with differential
   harnesses (`compare_stride2_column_asm`, gated on `RENDERER_ASM_DIFF`,
   verified via `npm run asm-diff`). A generator that *emits both sides* would
   fit the AGENTS.md rule that a harness replays the production emitter rather
   than a copy of it, and kill a whole class of asm/C drift. Two caveats: a
   generator that emits both sides can also agree with itself (see the pack
   asm/C self-comparison bug — negative-control it), and Dread's runtime
   generation spends RAM (`SCALER_BUFFER` ≈ 86 KB) that our 64 KB work RAM
   does not have, so ours would have to be a build-time generator into ROM.
3. **Baked y-bands per line.** Dread's `y1..y4` + `LineSizeCheat` precompute
   span extents for common heights at the fixed eye. Our analogue: the pair
   table already bakes shade and horizontal texel pairs; what is still per-pixel
   is vertical stepping, descriptor setup and overlay compositing. The overlay's
   per-pixel `wall_source_y` re-derivation was already deleted for exactly this
   reason (LOG 2026-08-30, pack −38% with a window on screen). Any further
   band-baking must respect that `wall_source_y` returns early/unmasked for
   full-height textures.
4. **Do NOT borrow: 160×100 with vertical texel doubling, ignoring camera z,
   hard per-frame line caps.** Each violates our fidelity contract (stride-4
   revert precedent, LOG 2026-07-27; fidelity is judged in motion only).
   Dread's speed is inseparable from what it refuses to draw.
5. **Do NOT borrow: blitter-C2P overlap as an excuse for main-RAM rendering.**
   Our equivalent already exists (V-INT pump during `bsp_cast_frame`). The
   binding constraint is work RAM (`tools/check-rom.ps1` errors below 16384
   free bytes and warns below 20480), not DMA — the partial-upload attempt
   already showed DMA levers cap at ~2 vblanks (LOG 2026-07-19).

## 4. Bottom line

Dread is efficient because it **moves visibility, ordering, scaling setup and
lighting out of the frame entirely** — into the map tool and the loader — leaving
a frame that is a list walk feeding generated byte-move loops, with display
conversion overlapped in hardware. Its price is a constrained world (maps built
for its tool, 160×100, a fixed render eye, hard per-frame caps). We kept DOOM's
world and pay for it in cast+pack every rebuild. The transferable ideas are the
ordered-vis bake (#1), build-time generated inner loops (#2) and further band
precomputation (#3); the resolution/fidelity cuts (#4) are explicitly out of
scope.
