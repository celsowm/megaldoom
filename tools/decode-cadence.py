#!/usr/bin/env python3
"""Decode the release-cadence mailbox (CadenceSnapshot in src/main.c).

Usage: python tools/decode-cadence.py out/report.json

The snapshot is published only by builds compiled with
-DDEBUG_BLASTEM_CHECKPOINT=1 and WITHOUT -DebugPerf; a DEBUG_PERF build
publishes the full RendererPerfSnapshot instead (use decode-perf-full.py).
All fields are big-endian, m68000 2-byte alignment (no padding here: every
field is u16/u32).
"""
import json
import struct
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    report = json.load(open(sys.argv[1]))
    blob = bytes.fromhex(report["perfMailbox"])
    # u16 magic/last/max/missed, u32 iterations/vblank_sum, u16 hist[8],
    # u32 cast/pack/projection/billboard subtick sums, u32 rebuild_frames
    fields = struct.unpack(">4H2I8H32I", blob[: 8 + 8 + 16 + 128])
    magic, last, vmax, missed = fields[:4]
    iterations, vblank_sum = fields[4:6]
    hist = fields[6:14]
    cast_sum, pack_sum, proj_sum, bb_sum, rebuilds = fields[14:19]
    nodes, boxes, segs_tested, segs_drawn = fields[19:23]
    drawseg_sum, sample_sum, samples = fields[23:26]
    (box_calls, box_near, box_cheap, box_early, box_sum,
     range_calls, range_sum, all_closed_sum) = fields[26:34]
    scene_frames = fields[34]
    (bb_objects, bb_rows, bb_bytes, bb_opaque, bb_commits,
     bb_marks, bb_mismatch) = fields[35:42]
    bb_setup_sum, bb_rows_sum, bb_max_bytes, bb_max_subticks = fields[42:46]
    if magic != 0xCADE:
        print(f"bad magic 0x{magic:04X} (expected 0xCADE) - wrong build type? "
              "DEBUG_PERF builds publish RendererPerfSnapshot instead.")
        return 1
    avg = vblank_sum / iterations if iterations else 0.0
    print(f"  iterations       = {iterations}")
    print(f"  avg vblanks      = {avg:.2f}  ({60.0 / avg if avg else 0:.1f} fps)")
    print(f"  last / max       = {last} / {vmax}")
    print(f"  missed (>2 vb)   = {missed}  ({100.0 * missed / iterations if iterations else 0:.1f}%)")
    for bucket, count in enumerate(hist):
        if count:
            label = f"{bucket}" if bucket < 7 else "7+"
            print(f"  hist[{label:>2}] vblanks = {count}")
    # Subticks: 76800/s; a 60Hz vblank is 1280 subticks.
    print(f"  rebuild frames   = {rebuilds}")
    if rebuilds:
        # cast and pack run only on base-rebuild frames; projection and
        # billboard run on every scene frame. Dividing all four by rebuilds
        # inflated the latter two by iterations/rebuilds.
        for name, total in (("cast", cast_sum), ("pack", pack_sum)):
            per = total / rebuilds
            print(f"  {name:<10} avg   = {per:7.0f} subticks/rebuild "
                  f"({per / 1280.0:.2f} vblanks)")
    if scene_frames:
        print(f"  scene frames     = {scene_frames}")
        for name, total in (("projection", proj_sum), ("billboard", bb_sum)):
            per = total / scene_frames
            print(f"  {name:<10} avg   = {per:7.0f} subticks/scene-frame "
                  f"({per / 1280.0:.2f} vblanks)")
    if rebuilds:
        for name, total in (("nodes visited", nodes),
                            ("boxes projected", boxes),
                            ("segs tested", segs_tested),
                            ("segs drawn", segs_drawn),
                            ("samples drawn", samples)):
            print(f"  {name:<15} avg = {total / rebuilds:7.1f} /rebuild")
        print(f"  drawseg total    = {drawseg_sum / rebuilds:7.0f} subticks/rebuild "
              f"({drawseg_sum / rebuilds / 1280.0:.2f} vblanks; "
              f"{drawseg_sum / segs_tested if segs_tested else 0:.1f}/seg tested)")
        print(f"  sample loop      = {sample_sum / rebuilds:7.0f} subticks/rebuild "
              f"({sample_sum / samples if samples else 0:.1f}/sample; "
              f"fixed = {(drawseg_sum - sample_sum) / segs_tested if segs_tested else 0:.1f}/seg)")
        # Traversal attribution: box_calls is every project_box_range entry, so
        # it exceeds "boxes projected" (which counts only those reaching a
        # divide). near-path boxes pay up to 8 DIVS.W against the fast path's 2.
        if box_calls:
            fast = box_calls - box_near - box_cheap - box_early
            print(f"  box calls        = {box_calls / rebuilds:7.1f} /rebuild"
                  f"  (near-plane {100.0 * box_near / box_calls:.0f}%,"
                  f" cheap-reject {100.0 * box_cheap / box_calls:.0f}%,"
                  f" early-out {100.0 * box_early / box_calls:.0f}%,"
                  f" fast {100.0 * fast / box_calls:.0f}%)")
        if box_sum or range_sum or all_closed_sum:
            traversal = cast_sum - drawseg_sum
            for name, total, calls in (
                    ("project_box_range", box_sum, box_calls),
                    ("range_closed", range_sum, range_calls),
                    ("all_closed", all_closed_sum, nodes)):
                print(f"  {name:<17} = {total / rebuilds:7.0f} subticks/rebuild "
                      f"({total / rebuilds / 1280.0:.2f} vb; "
                      f"{total / calls if calls else 0:.1f}/call)")
            rest = traversal - box_sum - range_sum - all_closed_sum
            print(f"  traversal rest    = {rest / rebuilds:7.0f} subticks/rebuild "
                  f"({rest / rebuilds / 1280.0:.2f} vb; recursion + leaf visits)")
        # Billboard raster units. bb_pixels is the inner-loop trip count; the
        # opaque share is how much of that walk actually writes a texel.
        div = scene_frames or rebuilds
        print(f"  bb objects drawn = {bb_objects / div:7.1f} /scene-frame")
        for name, total in (("bb sprite rows", bb_rows), ("bb packed bytes", bb_bytes),
                            ("bb opaque texels", bb_opaque),
                            ("bb byte commits", bb_commits), ("bb overlay marks", bb_marks)):
            print(f"  {name:<17} = {total / div:8.1f} /scene-frame")
        if bb_bytes:
            pixels = bb_bytes * 2
            print(f"  bb opaque share  = {100.0 * bb_opaque / pixels:.1f}% of pixel slots")
            print(f"  bb subticks/px   = {bb_sum / pixels:.2f}"
                  f"   ({bb_sum / bb_objects if bb_objects else 0:.0f}/object)")
        if bb_setup_sum or bb_rows_sum:
            print(f"  bb setup/object  = {bb_setup_sum / div:7.0f} subticks/scene-frame"
                  f"  ({bb_setup_sum / bb_objects if bb_objects else 0:.0f}/object)")
            print(f"  bb row loop      = {bb_rows_sum / div:7.0f} subticks/scene-frame"
                  f"  ({bb_rows_sum / (bb_bytes * 2) if bb_bytes else 0:.2f}/px,"
                  f" {bb_rows_sum / bb_rows if bb_rows else 0:.0f}/row)")
        print(f"  bb WORST frame   = {bb_max_subticks} subticks "
              f"({bb_max_subticks / 1280.0:.2f} vblanks), {bb_max_bytes} bytes "
              f"({bb_max_bytes * 2} px)")
        print(f"  bb VERIFY mismatch = {bb_mismatch}"
              f"{'  <-- RASTER DIVERGED' if bb_mismatch else ''}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
