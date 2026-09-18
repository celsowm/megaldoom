#!/usr/bin/env python3
"""Generate one wall scaler routine per projected sample height (Dread-style).

The generic wall post in renderer_hotpath.s spends ~56 cycles per byte walking
the vertical DDA: read the sample, add tex_y, mask, indexed fetch, store,
advance, DBRA. For a centred column every one of those values is known offline
once the sample height S is known, because the rows a column draws are exactly
the first wall_h entries of MEGALDOOM_WALL_TEX_Y_BY_HEIGHT[S] (the table
already encodes the viewport clip). So each S gets a routine that is nothing
but its stores:

    move.b  ty_{n-1}(a5),4*(n-1)(a6)
    ...
    move.b  ty_0(a5),0(a6)
    rts

emitted as raw 6-byte instruction words (the assembler must not shrink the
zero displacements) in REVERSE row order, so a column of wall_h <= n rows
enters at `end - 6 * wall_h` and executes exactly rows wall_h-1 .. 0. That is
~20 cycles per byte, no loop, no bounds, and one routine serves every viewport
height. a5 is the packed column (plus tex_y when the offset cannot wrap), a6
the byte of the column's first wall row; neither is modified.

n = min(S, 120), the DEFAULT viewport's rows. The table is as wide as the
TALLEST viewport (128) and bakes its centring clip, so each routine starts at
MEGALDOOM_WALL_CLIP_DELTA[0][S] -- the 120-row viewport's offset into that row.
A 128-row viewport's clipped columns carry a different delta and are excluded by
renderer_pack.c's eligibility test, keeping the generic post.

Outputs:
  src/renderer/generated_wall_scalers.s  routines + megaldoom_wall_scaler_ends
  src/renderer/generated_wall_scalers.h  MEGALDOOM_WALL_SCALER_MAX_TY[S] (the
                                         largest masked sample a routine reads,
                                         for the runtime's no-wrap test)
"""

import argparse
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "src" / "renderer" / "generated_renderer_assets.h"
ABI = ROOT / "src" / "renderer" / "renderer_pack_abi.h"
TABLE = "MEGALDOOM_WALL_TEX_Y_BY_HEIGHT"
TEX_MASK = 127
# move.b d16(a5),d16(a6): MOVE (0001) byte, dst (d16,An) reg 6, src (d16,An) reg 5.
MOVE_B_D16A5_D16A6 = 0x1D6D
RTS = 0x4E75


def load_row_bytes():
    """The byte stride between screen rows of one packed column.

    Baked into every displacement below, so it is read from the ABI header
    rather than assumed; the emitted .s also asserts it at assembly time.
    """
    match = re.search(r"#define\s+PACK_TILE_ROW_BYTES\s+(\d+)", ABI.read_text())
    if not match:
        raise SystemExit("could not find PACK_TILE_ROW_BYTES in %s" % ABI)
    return int(match.group(1))


def load_clip_delta(text):
    """Row 0 of MEGALDOOM_WALL_CLIP_DELTA: the DEFAULT (120-row) viewport.

    The DDA table's centring clip is baked for the tallest viewport, so a
    120-row viewport starts `delta` rows further into the same row. The routines
    below bake THIS delta, because the 120-row presets are the default and the
    ones a close wall is normally drawn at; renderer_pack.c only lets a column
    use a routine when its own clip delta matches what was baked here.
    """
    match = re.search(r"MEGALDOOM_WALL_CLIP_DELTA\[2\]\[\d+\]\s*=\s*\{\s*\{(.*?)\},", text, re.S)
    if not match:
        raise SystemExit("could not find MEGALDOOM_WALL_CLIP_DELTA in %s" % ASSETS)
    return [int(v) for v in re.findall(r"\d+", match.group(1))]


def load_table(text):
    match = re.search(r"%s\[(\d+)\]\[(\d+)\]\s*=\s*\{(.*?)\n\};" % TABLE, text, re.S)
    if not match:
        raise SystemExit("could not find %s in %s" % (TABLE, ASSETS))
    heights, rows = int(match.group(1)), int(match.group(2))
    values = [int(v) for v in re.findall(r"\d+", match.group(3))]
    if len(values) != heights * rows:
        raise SystemExit("%s: expected %d values, found %d" %
                         (TABLE, heights * rows, len(values)))
    return heights, rows, [values[s * rows:(s + 1) * rows] for s in range(heights)]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--asm-out", default=str(ROOT / "src" / "renderer" / "generated_wall_scalers.s"))
    ap.add_argument("--header-out", default=str(ROOT / "src" / "renderer" / "generated_wall_scalers.h"))
    ap.add_argument("--negative-control-corrupt-height", type=int, default=None,
                    help="NEGATIVE CONTROL: emit a wrong row for this sample height "
                         "(0 = every height), which tools/check-asm-diff.ps1 must catch")
    args = ap.parse_args()

    row_bytes = load_row_bytes()
    assets = ASSETS.read_text()
    heights, rows, table = load_table(assets)
    clip_delta = load_clip_delta(assets)
    # A routine covers at most the DEFAULT viewport's rows. The table is as wide
    # as the tallest viewport, but a 120-row preset never draws more than 120
    # rows, and a 128-row preset's taller columns carry a different clip delta
    # and are excluded by the eligibility test instead.
    scaler_rows = 120
    asm = [
        "/* Generated by tools/gen_wall_scalers.py from %s -- do not edit." % TABLE,
        " * One routine per sample height S; see the generator for the contract. */",
        '#include "renderer_pack_abi.h"',
        "",
        "/* The row displacements below were baked with this stride. */",
        "    .if PACK_TILE_ROW_BYTES != %d" % row_bytes,
        "    .error \"PACK_TILE_ROW_BYTES changed; rerun tools/gen_wall_scalers.py\"",
        "    .endif",
        "",
        "    .section .text.megaldoom_wall_scalers,\"ax\"",
        "    .align  2",
    ]
    max_ty = [0] * heights
    total_rows = 0
    for s in range(1, heights):
        n = min(s, scaler_rows)
        base = clip_delta[s]
        if base + n > rows:
            raise SystemExit("S=%d: clip delta %d + %d rows exceeds the table's %d" %
                             (s, base, n, rows))
        raw = [table[s][base + i] for i in range(n)]
        # renderer_pack.c folds tex_y into the column pointer and, when
        # tex_y + a sample reaches 128, splits the column at the ONE row where
        # that first happens (a binary search, then two passes of this
        # routine). That is only exact while each row's samples are raw
        # (unmasked), below the texture height and non-decreasing.
        if max(raw) > TEX_MASK or any(b < a for a, b in zip(raw, raw[1:])):
            raise SystemExit("S=%d: samples wrap or decrease; the runtime's "
                             "single-wrap split in renderer_pack.c is invalid" % s)
        samples = [value & TEX_MASK for value in raw]
        if args.negative_control_corrupt_height in (s, 0):
            samples[n // 2] = (samples[n // 2] + 1) & TEX_MASK
        max_ty[s] = max(samples)
        asm.append("    /* S = %d, %d rows */" % (s, n))
        for i in reversed(range(n)):
            asm.append("    .word 0x%04X,%d,%d" % (MOVE_B_D16A5_D16A6, samples[i], row_bytes * i))
        asm.append("megaldoom_wall_scaler_end_%d:" % s)
        asm.append("    .word 0x%04X" % RTS)
        total_rows += n
    asm.extend([
        "",
        "    .section .rodata.megaldoom_wall_scaler_ends,\"a\"",
        "    .align  2",
        "    .globl  megaldoom_wall_scaler_ends",
        "/* Indexed by S; entry 0 is unused (S = 0 means no routine). */",
        "megaldoom_wall_scaler_ends:",
        "    .long   0",
    ])
    for s in range(1, heights):
        asm.append("    .long   megaldoom_wall_scaler_end_%d" % s)
    asm.extend(["", ""])
    Path(args.asm_out).write_text("\n".join(asm), newline="\n")

    header = [
        "#ifndef MEGALDOOM_GENERATED_WALL_SCALERS_H",
        "#define MEGALDOOM_GENERATED_WALL_SCALERS_H",
        "",
        "// Generated by tools/gen_wall_scalers.py -- do not edit.",
        "// %d routines, %d rows, %d bytes of code." % (heights - 1, total_rows,
                                                        6 * total_rows + 2 * (heights - 1)),
        "#define MEGALDOOM_WALL_SCALER_HEIGHTS %d" % heights,
        "#define MEGALDOOM_WALL_SCALER_ROWS %d" % scaler_rows,
        "// Largest masked DDA sample routine S reads: tex_y may be folded into the",
        "// column pointer only while tex_y + this < 128 (no vertical wrap).",
        "static const u8 MEGALDOOM_WALL_SCALER_MAX_TY[MEGALDOOM_WALL_SCALER_HEIGHTS] = {",
    ]
    for i in range(0, heights, 20):
        header.append("    " + ", ".join(str(v) for v in max_ty[i:i + 20]) + ",")
    header.extend(["};", "", "#endif", ""])
    Path(args.header_out).write_text("\n".join(header), newline="\n")
    print("wrote %d routines, %d rows, %d bytes" % (heights - 1, total_rows,
                                                    6 * total_rows + 2 * (heights - 1)))


if __name__ == "__main__":
    main()
