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
    fields = struct.unpack(">4H2I8H12I", blob[: 8 + 8 + 16 + 48])
    magic, last, vmax, missed = fields[:4]
    iterations, vblank_sum = fields[4:6]
    hist = fields[6:14]
    cast_sum, pack_sum, proj_sum, bb_sum, rebuilds = fields[14:19]
    nodes, boxes, segs_tested, segs_drawn = fields[19:23]
    drawseg_sum, sample_sum, samples = fields[23:26]
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
        for name, total in (("cast", cast_sum), ("pack", pack_sum),
                            ("projection", proj_sum), ("billboard", bb_sum)):
            per = total / rebuilds
            print(f"  {name:<10} avg   = {per:7.0f} subticks/rebuild "
                  f"({per / 1280.0:.2f} vblanks)")
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
    return 0


if __name__ == "__main__":
    sys.exit(main())
