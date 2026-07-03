#!/usr/bin/env python3
"""
extract-mus.py - Extract MUS music lumps from a Doom WAD file.

Doom stores its music in id Software's MUS format (a compact MIDI variant).
The lumps are named D_* (D_E1M1, D_INTER, D_INTRO, D_VICTOR, D_INTROA, ...).
Sound patches like DPBODY/DPPAUSE start with "DP" and are correctly excluded
by the "D_" prefix filter.

The MUS files are written to res/music/mus/ for downstream conversion by
mus2midi.py (MUS -> SMF) and then midi2vgm (SMF -> VGM).

Usage:
    python tools/extract-mus.py [--wad DOOM1.WAD] [--out res/music/mus]
"""

import argparse
import os
import struct
import sys

# Project root is the parent of the tools/ directory.
PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_WAD = os.path.join(PROJECT_ROOT, "DOOM1.WAD")
DEFAULT_OUT = os.path.join(PROJECT_ROOT, "res", "music", "mus")

# WAD header: signature (4s), numLumps (i), dirOffset (i) — all little-endian.
WAD_HEADER_FMT = "<4sii"
WAD_HEADER_SIZE = struct.calcsize(WAD_HEADER_FMT)
# Each directory entry: filepos (i), size (i), name (8s) — 16 bytes.
LUMP_ENTRY_FMT = "<ii8s"
LUMP_ENTRY_SIZE = struct.calcsize(LUMP_ENTRY_FMT)

# MUS magic: "MUS" followed by 0x1A (EOF marker).
MUS_MAGIC = b"MUS\x1a"


def read_wad_directory(wad_path):
    """Read the WAD file and return (signature, [(name, filepos, size), ...])."""
    with open(wad_path, "rb") as f:
        data = f.read()

    if len(data) < WAD_HEADER_SIZE:
        raise ValueError("File too small to be a WAD: %d bytes" % len(data))

    signature, num_lumps, dir_offset = struct.unpack_from(WAD_HEADER_FMT, data, 0)
    signature_str = signature.decode("ascii", errors="replace").rstrip("\x00")

    if signature_str not in ("IWAD", "PWAD"):
        raise ValueError("Not a WAD file (signature: %r)" % signature)

    lumps = []
    for i in range(num_lumps):
        entry_offset = dir_offset + i * LUMP_ENTRY_SIZE
        filepos, size, raw_name = struct.unpack_from(LUMP_ENTRY_FMT, data, entry_offset)
        # Lump names are 8 bytes, null-padded, uppercase ASCII.
        name = raw_name.split(b"\x00")[0].decode("ascii", errors="replace")
        lumps.append((name, filepos, size))

    return data, signature_str, lumps


def extract_mus_lumps(wad_path, out_dir):
    """Extract all D_* music lumps that have the MUS magic signature."""
    data, signature, lumps = read_wad_directory(wad_path)
    print("Reading WAD: %s" % wad_path)
    print("  signature=%s  lumps=%d" % (signature, len(lumps)))

    os.makedirs(out_dir, exist_ok=True)
    print("Writing MUS files to: %s" % out_dir)

    count = 0
    skipped = 0
    for name, filepos, size in lumps:
        # Music lumps start with "D_". Sound patches (DPBODY, DPPAUSE, ...)
        # start with "DP" and are excluded by this filter.
        if not name.startswith("D_"):
            continue

        # Skip zero-size marker lumps.
        if size == 0:
            skipped += 1
            continue

        lump_data = data[filepos:filepos + size]

        # Verify the MUS magic signature.
        if len(lump_data) < 4 or lump_data[:4] != MUS_MAGIC:
            print("  ! %s: not a MUS file (magic=%r), skipping" % (name, lump_data[:4]))
            skipped += 1
            continue

        # Parse the MUS header for reporting.
        # Header: ID(4) + scoreLen(2) + scoreStart(2) + channels(2)
        #         + sec_channels(2) + instrCnt(2) + pad(2) = 16 bytes.
        if len(lump_data) >= 16:
            (mus_id, score_len, score_start, channels,
             sec_channels, instr_cnt, pad) = struct.unpack_from("<4sHHHHHH", lump_data, 0)
            print("  %-10s  size=%-6d  scoreLen=%-6d  scoreStart=%-4d  "
                  "channels=%d  instrCnt=%d"
                  % (name, size, score_len, score_start, channels, instr_cnt))

        out_path = os.path.join(out_dir, name.lower() + ".mus")
        with open(out_path, "wb") as f:
            f.write(lump_data)
        count += 1

    print("-" * 52)
    print("Extracted: %d MUS files" % count)
    if skipped:
        print("Skipped:   %d (non-music or zero-size lumps)" % skipped)
    return count


def main():
    parser = argparse.ArgumentParser(
        description="Extract MUS music lumps from a Doom WAD file."
    )
    parser.add_argument(
        "--wad", default=DEFAULT_WAD,
        help="Path to the WAD file (default: DOOM1.WAD in project root)."
    )
    parser.add_argument(
        "--out", default=DEFAULT_OUT,
        help="Output directory (default: res/music/mus)."
    )
    args = parser.parse_args()

    wad_path = os.path.abspath(args.wad)
    if not os.path.isfile(wad_path):
        print("Error: WAD file not found: %s" % wad_path, file=sys.stderr)
        return 1

    extract_mus_lumps(wad_path, os.path.abspath(args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
