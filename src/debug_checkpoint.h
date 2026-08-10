#ifndef MEGALDOOM_DEBUG_CHECKPOINT_H
#define MEGALDOOM_DEBUG_CHECKPOINT_H

#include <genesis.h>

#if DEBUG_BLASTEM_CHECKPOINT
#define DEBUG_CHECKPOINT_TITLE    0x01
#define DEBUG_CHECKPOINT_MENU     0x02
#define DEBUG_CHECKPOINT_GAMEPLAY 0x04
#define DEBUG_CHECKPOINT_MOVED    0x08
#define DEBUG_CHECKPOINT_COMBAT   0x10
#define DEBUG_CHECKPOINT_DEATH    0x20

/* BlastEm's deterministic-route runner reads this single byte through
 * --md-mailbox to confirm a route actually reached the state it claims to
 * exercise, instead of trusting frame counts alone. Bits accumulate within a
 * run (the runner ORs every frame's value); debug_checkpoint_reset clears
 * them at the start of a fresh title->gameplay cycle so a stale bit from a
 * previous run doesn't mask a route that never got there this time. */
void debug_checkpoint_reset(void);
void debug_checkpoint_mark(u8 bits);

/* A byte-for-byte copy of RendererPerfSnapshot (src/renderer_perf.h),
 * published for BlastEm's --md-perf-mailbox to read. This does not live as
 * its own global in renderer_perf.c: under -O3 -flto, GCC's whole-program
 * analysis inlines every renderer_perf.c setter at its call site and, in
 * testing, dropped the struct's symbol from the final build entirely --
 * even declared non-static, volatile, and marked
 * __attribute__((used, externally_visible)). g_debug_checkpoint_state above
 * survives the same build unaffected, so routing the copy through this file
 * sidesteps the problem empirically rather than explains it; see
 * tools/resolve-symbol.py for how the runner locates the result. */
#define DEBUG_CHECKPOINT_PERF_MAILBOX_BYTES 256
void debug_checkpoint_publish_perf(const void *snapshot, u16 bytes);

/* Release-cadence stage probe: coarse per-stage subtick accumulators for
 * checkpoint builds WITHOUT DEBUG_PERF. DEBUG_PERF's per-sample getSubTick
 * hooks distort the frame so badly that its stage shares can't be projected
 * onto release; these are written at the same top-level stage boundaries but
 * cost only ~6 getSubTick calls per frame, so the measured cadence stays
 * representative. Accumulated across the run, published in CadenceSnapshot
 * (src/main.c), decoded by tools/decode-cadence.py. */
#ifndef DEBUG_PERF
#define DEBUG_PERF 0
#endif
#if !DEBUG_PERF
#define CADENCE_STAGE_PROBE 1
extern u32 g_cadence_cast_subticks;
extern u32 g_cadence_pack_subticks;
extern u32 g_cadence_projection_subticks;
extern u32 g_cadence_billboard_subticks;
extern u32 g_cadence_rebuild_frames;
/* Cast-stage unit counters (increments only, no getSubTick): how many BSP
 * nodes/box projections/segs a release-speed cast actually touches, to
 * attribute the cast cost between traversal overhead and sample work. */
extern u32 g_cadence_nodes_visited;
extern u32 g_cadence_boxes_projected;
extern u32 g_cadence_segs_tested;
extern u32 g_cadence_segs_drawn;
/* draw_seg split: total time inside draw_seg calls, time inside its
 * per-sample fill loop, and samples actually written. The two timers cost
 * ~4 getSubTick calls per tested seg (~130/frame ~= +0.3 vb inside
 * cast_subticks on motion frames), so they are OFF by default to keep the
 * headline cadence numbers comparable across sessions — opt in with
 * EXTRA_FLAGS="... -DCADENCE_DRAWSEG_SPLIT=1". The samples counter is a bare
 * increment and stays always on. */
#ifndef CADENCE_DRAWSEG_SPLIT
#define CADENCE_DRAWSEG_SPLIT 0
#endif
extern u32 g_cadence_drawseg_subticks;
extern u32 g_cadence_sample_subticks;
extern u32 g_cadence_samples;
/* Traversal split: the cast time OUTSIDE draw_seg is the single largest block in
 * a motion frame (55% of cast, measured 2026-07-30) and was never attributed.
 * These break it into project_box_range, the two occlusion queries
 * (g_range_closed / g_all_closed) and the remainder (recursion + leaf visits).
 *
 * The path counters are bare increments and stay always on. Note
 * g_cadence_boxes_projected above only counts boxes that reach a projection, so
 * box_calls is the larger true call count and box_calls - boxes_projected is the
 * number rejected before any divide. box_near_path counts the expensive
 * near-plane polygon branch (up to 8 DIVS.W per box) versus the 2-DIVS fast
 * path — the ratio is what explains the block's cost.
 *
 * The three timers cost ~2 getSubTick calls per box and per occlusion query
 * (~400/frame), so they are OFF by default like the draw_seg split — opt in with
 * EXTRA_FLAGS="... -DCADENCE_TRAVERSE_SPLIT=1" and read the numbers as an
 * attribution of the block, not as release-absolute. */
#ifndef CADENCE_TRAVERSE_SPLIT
#define CADENCE_TRAVERSE_SPLIT 0
#endif
extern u32 g_cadence_box_calls;
extern u32 g_cadence_box_near_path;
extern u32 g_cadence_box_cheap_reject;
extern u32 g_cadence_box_early_out;
extern u32 g_cadence_box_subticks;
extern u32 g_cadence_range_closed_calls;
extern u32 g_cadence_range_closed_subticks;
/* Retired: the full-coverage test is now an inline word compare at the top of
 * render_boxed_child, far too cheap to bracket with two getSubTick calls. The
 * variable and its CadenceSnapshot field stay so the mailbox layout and
 * tools/decode-cadence.py do not shift; it always reads 0. */
extern u32 g_cadence_all_closed_subticks;
/* Frames on which renderer_render_scene ran. The projection and billboard
 * stages execute on EVERY frame, not only on the base-rebuild frames the cast
 * and pack stages are gated on, so dividing their subtick sums by
 * rebuild_frames (as tools/decode-cadence.py did until 2026-08-03) overstates
 * them by iterations/rebuilds -- a factor of ~6 on a route with many idle
 * frames. This is their correct divisor. */
extern u32 g_cadence_scene_frames;
/* Billboard raster attribution: how many objects reach the raster, how many
 * sprite rows and packed bytes the loops walk (one byte = two screen pixels),
 * how many of those pixel slots are actually opaque, and how many byte
 * read-modify-writes and overlay tile marks result. Bare increments (no
 * getSubTick), so always on. */
extern u32 g_cadence_bb_objects;
extern u32 g_cadence_bb_rows;
extern u32 g_cadence_bb_bytes;
extern u32 g_cadence_bb_opaque;
extern u32 g_cadence_bb_commits;
extern u32 g_cadence_bb_marks;
/* Non-zero only in a -DBILLBOARD_RASTER_VERIFY=1 build: packed tile rows where
 * the byte-wise rasterizer and the retained reference implementation disagree.
 * Must read 0 on every route. */
extern u32 g_cadence_bb_mismatch;
/* Pack-stage unit counters: tile columns the coherence cache actually repacked,
 * and how those columns' 15 tiles split between whole ceiling/floor tiles (one
 * movem.l pair) and mixed wall tiles (the expensive path). Bare increments. */
extern u32 g_cadence_pack_columns;
extern u32 g_cadence_pack_flat_tiles;
extern u32 g_cadence_pack_mixed_tiles;
/* Worst single scene frame, not the run average: sprite raster cost scales with
 * on-screen area, so a point-blank sprite is many times the mean and the mean
 * hides it. */
/* Split of billboard cost into per-object setup vs the row raster loop. Costs
 * 2 getSubTick per drawn object, so OFF by default like the other splits --
 * opt in with EXTRA_FLAGS="... -DCADENCE_BB_SPLIT=1". Measured 2026-08-04:
 * setup is only 35-42 subticks/object (~10%); the row loop is ~90% at ~3
 * subticks per pixel slot, which is why the optimization work targets it. */
#ifndef CADENCE_BB_SPLIT
#define CADENCE_BB_SPLIT 0
#endif
extern u32 g_cadence_bb_setup_subticks;
extern u32 g_cadence_bb_rows_subticks;
extern u32 g_cadence_bb_max_bytes;
extern u32 g_cadence_bb_max_subticks;
/* Split of the pack stage into the per-column prologue (four
 * describe_wall_column calls plus the coherence compare, which every column
 * pays even when it is then skipped) vs the 15-tile write loop. Deliberately
 * measured per column, not per tile: 2 getSubTick per column is 40 calls a
 * rebuild, where per-tile timing would be 600 and would distort what it
 * measures. OFF by default; opt in with
 * EXTRA_FLAGS="... -DCADENCE_PACK_SPLIT=1". */
#ifndef CADENCE_PACK_SPLIT
#define CADENCE_PACK_SPLIT 0
#endif
extern u32 g_cadence_pack_desc_subticks;
extern u32 g_cadence_pack_tiles_subticks;
#else
#define CADENCE_STAGE_PROBE 0
#endif
#else
#define debug_checkpoint_reset()
#define debug_checkpoint_mark(bits)
#define debug_checkpoint_publish_perf(snapshot, bytes)
#define CADENCE_STAGE_PROBE 0
#endif

#endif
