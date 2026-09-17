# Dread renderer vs MegalDoom renderer — why Dread is so efficient

Study of the Dread source drop (`.externals/Dread-source-drop/`, engine in
`dreadmake/src/`, tools in `dreadtool/src/`) and comparison with our renderer
(`src/renderer/`, `src/bsp/`). Dread targets the Amiga 500 / Atari ST:
7–8 MHz 68000, no FPU, with blitter (Amiga) or CPU `movep` LUT (ST) for
chunky-to-planar conversion.

> Note: `dreadmake/src/dread_engine.h` is nearly empty (only `RAND_TAB`).
> The real structures live in `dread_data.h` (file format), `dread_estructs.h`
> (runtime), and `dreadtool/src/dread_render.h` (tool-side mirror).

## 1. How the Dread renderer works

### 1.1 Frame pipeline

```text
Game_MainLoop: VScreen_BeginFrame → Dread_RunLogic → Dread_Render → VScreen_EndFrame
```

`Dread_Render` (`dreadmake/src/dread_soft.c:567-585`) is three steps:

```text
Dread_Render()
  FrameReset()              // init 160 render_columns from render_layout_delta
  Dread_DrawVisible(vis)    // walk this leaf's baked vis list → e_vislines, e_visthings
  Dread_DrawLinesTable()    // draw vislines in stored offline order
  Dread_DrawSprites()       // transform, depth-sort, draw far→near
```

There is **no per-frame raycast, no BSP walk for visibility, no overdraw
resolver at runtime**. The single most important efficiency fact: visibility
and draw order are an **offline bake**, the frame is a list walk.

### 1.2 Map format and offline visibility (the core trick)

File structures (`dread_data.h:9-52`): `DataVertex{xp,yp}` shorts;
`DataLine{v1,v2,tex_upper,offs_x,ceil/floor,modes[2],tex_lower,y1..y4}`;
`DataBSPNode{A,B,C,left/right}` where a negative child is `~vis offset`;
`DataSubSector{visoffset,type,height}`. The header carries **two extra BSPs**:
`map_collision_bsp_root` + `map_hitscan_bsp_root` (`dread_data.h:11-12`) —
collision and hitscan are separate trees from the render BSP, so gameplay
queries never pay for render topology.

The tool (`dreadtool/src/dread_mapgen.cpp:1794-1913`, `ComputeVisibility_Rec`)
does a beam/portal flood per region with recursion cap depth 32, producing a
**per-region ordered draw list** (`dread_mapgen.h:177-199`) exported as token
words (`LINEMODE_BASE` / `SUBSECTOR_BASE` / `CONDITION` / `END`,
`dread_mapgen.h:420-431`). Each wall is baked to 4 heights
(`LoadSectorHeights`, `dread_mapgen.cpp:81-153`):
`h1` = own ceil, `h2` = other ceil, `h3` = other floor, `h4` = own floor
(one-sided: `h1` = ceil, rest = floor). Lines whose heights+flats match on both
sides (sky-aware) are dropped entirely (`IsDrawn`/`IsRequired`,
`dread_mapgen.h:92-117`).

Runtime (`dread_asm_draw.s:39-126`): one asm point-locate (`Dread_FindSubSector`,
`dread_asm.s:121-151`, 16-bit `muls` walk), then a linear walk of that leaf's
vis list. Lines append to `e_vislines`; thing-sectors expand with `visframe`
dedup; condition-off runs are skipped. `Dread_DrawLinesTable`
(`dread_asm_draw.s:279-317`) dispatches **in stored offline order** — painter's
algorithm as data, not code.

Hard caps (`dread.h:13-21`): 1024 verts, 1536 lines, 512 subsectors, 512 things,
**540 vislines, 64 visthings per frame**. See-through-door chains capped at
`MAX_SEE_THROUGH=2` (`dread_mapgen.h:31`).

### 1.3 Wall drawing: generated scalers, ~1 byte-move per pixel

Framebuffer is **160×100 chunky** (`dread_render.h:10-13`), zoom 80
(`xmin = p1x*80/p1y + 80`, `dread_asm_lines.s:243-258`).
`ZOOM_CONSTANT = (80*32)<<(8+3)` with sub-vertex precision 3
(`dread_render.h:17`). Per line: frustum clip, `s = ZOOM/depth`,
`ds = (s2-s1)/xlen`, `u = mulu(tx,s)`, 32-bit `du = (u2-u1)/xlen`
(`dread_asm_lines.s:125-361`).

`dread_codegen.c:22-161` **generates the inner loops at load time**: one scaler
per size idol (`NUM_SOFT_SIZES=2048`, `dread_render.h:23`) built from shared
`MOVE.b xxx(A2),(A7)+` bodies; flat fills and sky copy get their own generators
(`:167-202`). Per-size `LineSizeCheat` precomputes ceil/upper/floor spans for
the fixed eye height (`dread_codegen.c:104-156`); the line core just `jsr`s
`fn_persp`/`fn_nopersp` (`dread_asm_lines.s:384-396`). Perspective-vs-linear is
chosen per span by a midpoint error test (`dread_asm_lines.s:313-329`).
Wall-pixel cost is ≈ **one byte move with a precomputed source offset** —
no multiply, no divide, no table lookup in the inner loop.

Textures are 64 texels wide; **lighting is baked as separate texture variants**
(`tex_upper` "includes light level", `dread_estructs.h:42`) — shading costs zero
at runtime.

### 1.4 Floors, ceilings, sprites, display

- **Floors/ceilings are flat-shaded**, never textured at runtime. Per-line
  `ceil_col`/`floor_col` bytes; per column: fill `[0,y1)`, textured upper,
  gap shrinks the sprite window (`ymin/ymax`), textured lower, fill to row 100
  (`dread_render.cpp:588-604`). Sky is a missing-ceil special case with a
  scrolling column-copy (`dread_soft.c:560-584`).
- **Sprites**: things linked per subsector; pass 1 culls by `visframe` +
  transforms, pass 3 depth-sorts, pass 4 draws far→near
  (`dread_asm_sprites.s:29-211`). The sprite column core **reuses the wall
  scalers** (`:504-510`), with per-column `size_limit` depth test and RLE spans.
- **Display**: CPU renders chunky into a 2-slot `C2P_Scene_Queue`
  (`amiga_asm.h:13-18`); the **Amiga blitter does C2P under IRQ**
  (`amiga_framework.s:34-38`, `:50-244`) while the CPU draws the next buffer.
  Copper builds the 4-bitplane list + 16-colour palette
  (`amiga_vscreen.c:39-100`); double buffers throughout. ST does CPU/LUT
  `movep.l` C2P (`st_c2p.s:23-80`).

### 1.5 Constraints Dread accepts (this is where the speed comes from)

Fixed 160×100 (`STRETCH_SCALING=1` line-doubling), fixed eye height, no pitch
variable (only `view_angle`), vertical-only walls, flat-or-sky ceilings, flat
floors, ≤540 lines / ≤64 sprites per frame, 64-px textures, baked lighting,
columns ≤100 px so scale stays 16-bit (`ENGINE_MIN_ZNEAR`). Integer-only:
fix14 trig (`fn.c:4-32`), `muls/divs/divu`, error-gated perspective skip.

## 2. How our renderer differs

| Aspect | Dread | MegalDoom |
|---|---|---|
| Visibility | Offline PVS + ordered draw list; runtime is a list walk (`dread_asm_draw.s:39-126`) | Live front-to-back flat BSP cast every rebuild (`bsp/bsp_render.c:bsp_cast_frame`); ~50 nodes, ~55 box projections, ~30 segs tested per rebuild |
| Draw order | Baked painter order, zero runtime cost | BSP front-to-back traversal + `RayColumn` buffer per sample |
| Wall inner loop | Load-time **generated** `MOVE.b` scalers (`dread_codegen.c`); ~1 byte-move/px | Hand-written `renderer_hotpath.s` stride-2 span writer over **pre-shaded pair bytes** (`FREEDOOM_WALL_PACKED_PAIRS`, 786 KB: v-scale+shade+2 texels baked) |
| Shading | Baked texture variants (0 runtime cost) | Baked pair table (0 per-pixel ALU; same idea, different encoding) |
| Horizontal res | 160 true columns | 80 sampled columns (`RAY_COL_STRIDE=2`), stride 4 reverted — user rejected in-motion fidelity loss (LOG 2026-07-27) |
| Vertical res | 100 rows, flat floors | Runtime-selectable viewport (buffers at `_MAX` 22×16 tiles); textured 64×128 walls |
| Floors/ceilings | Flat fill, 0 texture cost | Static VRAM atlas, 0 per-frame DMA (`renderer_upload.c`) |
| Sprites | Reuse wall scalers, RLE, depth-sorted | Full project→measure→draw billboard pipeline every scene frame; tail dominates (point-blank sprite 30× mean) |
| Display path | Chunky render → blitter/CPU C2P overlapped with next frame | Tile-based VDP: double-buffered 300-tile DMA (~2 vblanks) + background V-INT pump during cast (`renderer_upload.c`, `renderer.h:86-93`) |
| Frame profile | Quote from our side (cadence probe): rebuild = cast ~4–6k + pack ~3–5k + projection ~1.5k + billboard ~1.6–2.5k subticks; idle hits 30 fps | Same numbers — CPU-bound in cast+pack; DMA levers cap at ~2 vblanks |

The honest summary: **both renderers already converged on the same big ideas**
— bake shading offline, integer-only inner loops, overlap DMA with CPU. The
remaining gap is architectural: Dread pays **nothing per frame for visibility**
(PVS list walk), while we re-cast a BSP every rebuild (~4–6k subticks, our
single largest stage). Conversely we keep features Dread refuses: textured
walls at selectable viewport sizes, DOOM-geometry fidelity, runtime doors and
windows.

## 3. What we could borrow (and what we should not)

1. **Per-region ordered vis lists (biggest lever, biggest cost).** Dread's
   offline beam flood (`ComputeVisibility_Rec`, depth cap 32,
   `MAX_SEE_THROUGH=2`) replaces per-frame traversal with a list walk. For us
   this would mean precomputing, per leaf/cluster, the ordered seg set —
   essentially a PVS over our flat BSP. It attacks cast, our largest stage.
   But: our maps are real DOOM geometry (not Dread-authored constrained maps),
   doors/windows are dynamic (condition runs in Dread are the closest analogue,
   and they only *skip* baked runs), and the bake must stay valid under
   `SEG_WINDOW` splits and door motion. Prototype on E1M1 with a verifier
   (same discipline as `test-sector-map.py`'s solid-cover check) before
   believing any speedup.
2. **Code-generated inner loops.** Dread generates one scaler per size at load
   (`NUM_SOFT_SIZES=2048`); we hand-maintain `renderer_hotpath.s` + a C
   reference with differential harnesses (`compare_stride2_column_asm`,
   gated on `RENDERER_ASM_DIFF`, verified via `npm run asm-diff`). A generator
   that *emits both sides* would fit our "harness replays the production
   emitter" rule (AGENTS.md) and kill a whole class of asm/C drift. Cheap to
   try, low risk.
3. **Baked y-bands per line.** Dread's `y1..y4` + `LineSizeCheat` precompute
   span extents for the fixed eye height. Our analogue: the pair table already
   bakes v-scale/shade; what is still per-pixel is descriptor setup and overlay
   compositing. The overlay's per-pixel `wall_source_y` re-derivation was
   already deleted for exactly this reason (LOG 2026-08-30, was ~38% of pack
   with a window on screen). Any further band-baking must respect that
   `wall_source_y` returns early/unmasked for full-height textures.
4. **Do NOT borrow: flat-shaded floors, 160×100, fixed eye, 540-line caps.**
   Each violates our fidelity contract (stride-4 revert precedent, LOG
   2026-07-27; fidelity judged in motion only). Dread's speed is inseparable
   from what it refuses to draw.
5. **Do NOT borrow: blitter-C2P overlap as an excuse for main-RAM rendering.**
   Our equivalent already exists (V-INT pump during `bsp_cast_frame`). The
   binding constraint is work RAM (20480-byte floor, `tools/check-rom.ps1`),
   not DMA — partial-upload work already showed DMA levers cap at ~2 vblanks
   (LOG 2026-07-19).

## 4. Bottom line

Dread is efficient because it **moves visibility, ordering, scaling setup and
lighting out of the frame entirely** — into the map tool and the loader — leaving
a frame that is a list walk feeding generated byte-move loops, with display
conversion overlapped in hardware. Its price is a constrained world (authored
maps, flat floors, fixed eye, hard per-frame caps). We kept DOOM's world and pay
for it in cast+pack every rebuild. The transferable ideas are the ordered-vis
bake (#1), generated inner loops (#2) and further band precomputation (#3);
the resolution/fidelity cuts (#4) are explicitly out of scope.
