#!/usr/bin/env python3
"""Estimate the space/cast tradeoff of heading-binned BSP programs.

This is deliberately an offline study, not a runtime bake.  It samples points
in each player's BSP cell and a few headings in each bin, then asks the real
``leaf_program`` builder to price the resulting temporary segment filter.
The default interval mode is conservative for the horizontal frustum: it uses
the whole cell AABB and each segment's endpoint AABB, so it keeps a segment
whenever any point in that over-approximation could enter the bin's 90-degree
view.  ``--mode sampled`` is retained as a faster, optimistic upper-bound
experiment.  Neither mode is a runtime correctness proof; the oracle is still
required before emitting a directional bake.
"""

import argparse
import math
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from bsp_vis import (DEFAULT_EPS, VisMap, centroid, leaf_program,
                     subtree_leaves, SEG_TRIGGER)
from doom_map import load_map
from wad_reader import WadFile

FX_SHIFT = 8
VIEW_COLS = 160
PROJ_X = 80
NEAR = 16
FOV_HALF = 33  # 45 degrees plus one unit for integer projection/fringe.


def sin_quarter(angle):
    x = ((angle & 63) << FX_SHIFT) // 64
    x2 = (x * x) >> FX_SHIFT
    x3 = (x2 * x) >> FX_SHIFT
    x5 = (x3 * x2) >> FX_SHIFT
    return ((479 * x) - (196 * x3) + (24 * x5)) >> FX_SHIFT


def fsin(angle):
    q = (angle & 255) // 64
    a = angle & 63
    if q == 0:
        return sin_quarter(a)
    if q == 1:
        return sin_quarter(63 - a)
    if q == 2:
        return -sin_quarter(a)
    return -sin_quarter(63 - a)


def points(poly):
    """Small deterministic sample of a convex player cell."""
    if not poly:
        return []
    c = centroid(poly)
    return list(poly) + [c] + [((x + c[0]) * 0.5, (y + c[1]) * 0.5)
                                for x, y in poly]


def segment_visible(md, seg_index, px, py, angle):
    seg = md.out_segs[seg_index]
    if seg["type"] == SEG_TRIGGER:
        return False
    ax, ay = md.vertices[seg["v1"]]
    bx, by = md.vertices[seg["v2"]]
    if (px - ax) * seg["nx"] + (py - ay) * seg["ny"] <= 0:
        return False
    fw_x, fw_y = fsin((angle + 64) & 255), fsin(angle)
    # The renderer's right vector is (-sin, cos), with the same Q8 basis.
    rx, ry = -fw_y, fw_x
    xs = []
    for x, y in ((ax, ay), (bx, by)):
        dx, dy = x - px, y - py
        depth = int(dx * fw_x + dy * fw_y) >> FX_SHIFT
        lateral = int(dx * rx + dy * ry) >> FX_SHIFT
        xs.append((depth, lateral))
    if xs[0][0] < NEAR and xs[1][0] < NEAR:
        return False
    if xs[0][0] < NEAR:
        d0, l0 = xs[0]
        d1, l1 = xs[1]
        l0 += int((l1 - l0) * (NEAR - d0) / (d1 - d0))
        xs[0] = (NEAR, l0)
    elif xs[1][0] < NEAR:
        d0, l0 = xs[0]
        d1, l1 = xs[1]
        l1 += int((l0 - l1) * (NEAR - d1) / (d0 - d1))
        xs[1] = (NEAR, l1)
    projected = []
    for depth, lateral in xs:
        if depth < NEAR:
            depth = NEAR
        projected.append(VIEW_COLS // 2 + (lateral * PROJ_X) // depth)
    left, right = sorted(projected)
    return right >= 0 and left < VIEW_COLS and left != right


def _arc_parts(start, length):
    """Split a circular arc in [0, 256) into one or two linear intervals."""
    if length >= 256:
        return [(0, 256)]
    start %= 256
    end = start + length
    if end <= 256:
        return [(start, end)]
    return [(start, 256), (0, end - 256)]


def _angle_arc_from_box(x0, y0, x1, y1):
    """Return the shortest arc containing directions from the origin to box."""
    if x0 <= 0 <= x1 and y0 <= 0 <= y1:
        return 0, 256
    angles = sorted((math.atan2(y, x) * 256.0 / (2.0 * math.pi)) % 256.0
                    for x, y in ((x0, y0), (x0, y1), (x1, y0), (x1, y1)))
    gaps = [(angles[(i + 1) % 4] - angles[i]) % 256.0 for i in range(4)]
    widest = max(range(4), key=lambda i: gaps[i])
    start = angles[(widest + 1) % 4]
    return start, 256.0 - gaps[widest]


def _arcs_overlap(a_start, a_len, b_start, b_len):
    for a0, a1 in _arc_parts(a_start, a_len):
        for b0, b1 in _arc_parts(b_start, b_len):
            if a0 <= b1 and b0 <= a1:
                return True
    return False


def interval_possible(md, vm, leaf, seg_index, bin_index, bins):
    """Conservative frustum test over the whole player cell AABB.

    Facing, near clipping and exact segment interpolation are intentionally
    ignored here: ignoring them can only retain more SEG words.  A SEG is
    removed only when even the AABB's possible direction interval misses the
    whole heading bin plus its horizontal FOV.
    """
    cell = vm.leaf_cell[leaf]
    if not cell:
        return True
    cx = [p[0] for p in cell]
    cy = [p[1] for p in cell]
    seg = md.out_segs[seg_index]
    a = md.vertices[seg["v1"]]
    b = md.vertices[seg["v2"]]
    # q - p, with p in the cell and q on the segment.  AABB subtraction is a
    # superset of the true relative-vector set and is therefore safe.
    vx0 = min(a[0], b[0]) - max(cx)
    vx1 = max(a[0], b[0]) - min(cx)
    vy0 = min(a[1], b[1]) - max(cy)
    vy1 = max(a[1], b[1]) - min(cy)
    start, length = _angle_arc_from_box(vx0, vy0, vx1, vy1)
    lo = (256 * bin_index) / bins
    hi = (256 * (bin_index + 1)) / bins
    return _arcs_overlap(start, length, lo - FOV_HALF,
                         (hi - lo) + 2 * FOV_HALF)


def emit_directional(directory, entries, bins, mode):
    """Write an unreferenced prototype data set for manual/oracle testing."""
    os.makedirs(directory, exist_ok=True)
    c_lines = ["// Experimental directional BSP programs; not part of the build.",
               '#include "bsp_map.h"', "", "#if BSP_VIS_LIST && BSP_VIS_DIRECTIONAL", ""]
    asm_lines = ["/* Experimental directional BSP programs; not part of the build. */", ""]
    cases = []
    for _entry_index, (prefix, vm, programs_by_bin) in enumerate(entries):
        # The bank section is the campaign level, not the position in the
        # selected --maps list (the prototype is often emitted for one map).
        level = int(prefix[-1]) - 1
        c_lines.append("// %s: %d bins, mode=%s" % (prefix, bins, mode))
        for b, programs in enumerate(programs_by_bin):
            words = []
            offsets = []
            lengths = []
            seen = {}
            for program in programs:
                program = tuple(program)
                if program not in seen:
                    seen[program] = len(words)
                    words.extend(program)
                offsets.append(seen[program])
                lengths.append(len(program))
            dat_name = "directional_bsp_vis_%s_b%d.dat" % (prefix, b)
            dat_path = os.path.join(directory, dat_name)
            with open(dat_path, "wb") as fh:
                fh.write(b"".join(struct.pack(">H", w) for w in words))
            label = "megaldoom_vis_directional_%s_b%d" % (prefix, b)
            asm_lines.extend([
                '    .section .wallpack%d,"a"' % level,
                "    .align  2",
                "    .globl  %s" % label,
                "%s: /* %d leaves, %d words, longest %d */" %
                (label, vm.leaf_count, len(words), max(lengths, default=0)),
                '    .incbin "%s"' % os.path.join(directory, dat_name).replace("\\", "/"),
                "",
            ])
            c_lines.append("extern const u16 %s[];" % label)
            c_lines.append("static const u32 %s_offset[%d] = {" %
                           (label, vm.leaf_count))
            for i in range(0, len(offsets), 8):
                c_lines.append("    " + ",".join(str(x) for x in offsets[i:i + 8]) + ",")
            c_lines.append("};")
            c_lines.append("static const u16 %s_length[%d] = {" %
                           (label, vm.leaf_count))
            for i in range(0, len(lengths), 16):
                c_lines.append("    " + ",".join(str(x) for x in lengths[i:i + 16]) + ",")
            c_lines.extend(["};", ""])
            cases.append((prefix, b, label, vm.leaf_count))
            print("  emitted %s bin %d: %d unique words (%d bytes)" %
                  (prefix, b, len(words), 2 * len(words)))
    c_lines.append("const u16 *bsp_vis_directional_program(const BspMapData *map, "
                   "u16 subsector, u8 bin, u16 *length) {")
    for prefix, b, label, count in cases:
        c_lines.append("    if (bin == %d && map == &g_%s_map && subsector < %d) {" %
                       (b, prefix, count))
        c_lines.append("        *length = %s_length[subsector];" % label)
        c_lines.append("        return &%s[%s_offset[subsector]];" % (label, label))
        c_lines.append("    }")
    c_lines.extend(["    return NULL;", "}", "", "#endif", ""])
    c_path = os.path.join(directory, "generated_bsp_vis_directional.c")
    # Keep the C and assembler basenames different: SGDK maps both source
    # types to the same out/<path>/<basename>.o.
    s_path = os.path.join(directory, "generated_bsp_vis_directional_data.s")
    with open(c_path, "w", newline="\n") as fh:
        fh.write("\n".join(c_lines))
    with open(s_path, "w", newline="\n") as fh:
        fh.write("\n".join(asm_lines))
    print("wrote %s and %s" % (c_path, s_path))


def pvs_for(vm, cache, mapn):
    if mapn in cache:
        return [set(row) for row in cache[mapn]]
    pvs = []
    for leaf in range(vm.leaf_count):
        if vm.leaf_empty[leaf]:
            vis = set(range(vm.leaf_count))
        else:
            vis, _ = vm.leaf_pvs(leaf)
        pvs.append(vis)
    cache[mapn] = [sorted(row) for row in pvs]
    return pvs


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--maps", nargs="+", default=["E1M6", "E1M7"])
    ap.add_argument("--bins", type=int, choices=(4, 8, 16), default=8)
    ap.add_argument("--mode", choices=("interval", "sampled"), default="interval",
                    help="interval is conservative; sampled is an optimistic study")
    ap.add_argument("--emit-dir", default=None,
                    help="write an unreferenced directional C/asm/data prototype")
    ap.add_argument("--wad", default=os.path.join(os.path.dirname(HERE), "DOOM1.WAD"))
    ap.add_argument("--pvs-cache", default=None)
    args = ap.parse_args()
    cache = {}
    if args.pvs_cache and os.path.exists(args.pvs_cache):
        import json
        with open(args.pvs_cache) as fh:
            cache = json.load(fh)
    wad = WadFile(args.wad)
    directional_entries = []
    for mapn in args.maps:
        start = time.time()
        md = load_map(wad, mapn)
        vm = VisMap(md, DEFAULT_EPS)
        pvs = pvs_for(vm, cache, mapn)
        base_programs = []
        for leaf in range(vm.leaf_count):
            base_programs.append(tuple(leaf_program(vm, leaf, pvs[leaf])))
        base_words = sum(len(p) for p in base_programs)
        base_unique = sum(len(p) for p in set(base_programs))
        print("%s: leaves=%d segs=%d baseline words=%d unique=%d" %
              (mapn, vm.leaf_count, len(md.out_segs), base_words, base_unique))
        cell_points = [points(vm.leaf_cell[leaf]) for leaf in range(vm.leaf_count)]
        programs_by_bin = []
        for b in range(args.bins):
            lo = (256 * b) // args.bins
            hi = (256 * (b + 1)) // args.bins
            # Three headings per bin catch the two edges and its center in the
            # optimistic mode.  Interval mode does not sample headings.
            headings = sorted(set((lo, (lo + hi - 1) // 2, hi - 1)))
            programs = []
            kept = 0
            total = 0
            for leaf in range(vm.leaf_count):
                visible = set()
                # PVS entries are subsector IDs.  Expand them to the emitted
                # SEG indices before applying the directional predicate; using
                # leaf IDs here silently removed unrelated segments.
                candidates = []
                for sub in pvs[leaf]:
                    first, count = md.out_ssectors[sub]
                    candidates.extend(range(first, first + count))
                for seg_index in candidates:
                    total += 1
                    if args.mode == "interval":
                        keep = interval_possible(md, vm, leaf, seg_index, b,
                                                 args.bins)
                    else:
                        keep = any(segment_visible(md, seg_index, px, py, angle)
                                   for px, py in cell_points[leaf]
                                   for angle in headings)
                    if keep:
                        visible.add(seg_index)
                kept += len(visible)
                programs.append(tuple(leaf_program(
                    vm, leaf, pvs[leaf],
                    seg_keep=lambda _leaf, k, visible=visible: k in visible)))
            words = sum(len(p) for p in programs)
            unique = sum(len(p) for p in set(programs))
            programs_by_bin.append(programs)
            print("  bin %2d (%3d..%3d): seg refs %d/%d (%.1f%%), words %d/%d (%.1f%%), unique %d" %
                  (b, lo, hi - 1, kept, total, 100.0 * kept / total,
                  words, base_words, 100.0 * words / base_words, unique))
        print("  elapsed %.1fs" % (time.time() - start))
        directional_entries.append((mapn.lower(), vm, programs_by_bin))
    if args.pvs_cache:
        import json
        with open(args.pvs_cache, "w") as fh:
            json.dump(cache, fh)
    if args.emit_dir:
        emit_directional(args.emit_dir, directional_entries, args.bins, args.mode)


if __name__ == "__main__":
    main()
