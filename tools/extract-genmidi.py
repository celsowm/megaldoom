#!/usr/bin/env python3
"""
extract-genmidi.py - Extract the GENMIDI lump from a Doom WAD file.

GENMIDI holds Doom's OPL2 (Yamaha YM3812) FM instrument bank: 175 voices
(128 General MIDI melodic + 47 percussion) used for the game's iconic DOS FM
sound. The lump begins with the magic header "#OPL_II#".

The Mega Drive's YM2612 is a close FM cousin of the OPL2, so these instrument
definitions can be translated to YM2612 patches (see tools/midi2vgm/genmidi.py)
for a far more authentic sound than hand-guessed patches.

The lump is written next to the converter (tools/midi2vgm/genmidi.op2) so the
MIDI->VGM conversion is self-contained and does not need the WAD at runtime.

Usage:
    python tools/extract-genmidi.py [--wad DOOM1.WAD] [--out tools/midi2vgm/genmidi.op2]
"""

import argparse
import os
import struct
import sys

# Project root is the parent of the tools/ directory.
PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_WAD = os.path.join(PROJECT_ROOT, "DOOM1.WAD")
DEFAULT_OUT = os.path.join(PROJECT_ROOT, "tools", "midi2vgm", "genmidi.op2")

# WAD header: signature (4s), numLumps (i), dirOffset (i) — all little-endian.
WAD_HEADER_FMT = "<4sii"
WAD_HEADER_SIZE = struct.calcsize(WAD_HEADER_FMT)
# Each directory entry: filepos (i), size (i), name (8s) — 16 bytes.
LUMP_ENTRY_FMT = "<ii8s"
LUMP_ENTRY_SIZE = struct.calcsize(LUMP_ENTRY_FMT)

GENMIDI_MAGIC = b"#OPL_II#"


def read_wad_directory(wad_path):
    """Read the WAD file and return (data, signature, [(name, filepos, size), ...])."""
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
        name = raw_name.split(b"\x00")[0].decode("ascii", errors="replace").upper()
        lumps.append((name, filepos, size))

    return data, signature_str, lumps


def extract_genmidi(wad_path, out_path):
    """Extract the GENMIDI lump and write it to out_path. Returns its size."""
    data, signature, lumps = read_wad_directory(wad_path)
    print("Reading WAD: %s" % wad_path)
    print("  signature=%s  lumps=%d" % (signature, len(lumps)))

    for name, filepos, size in lumps:
        if name != "GENMIDI":
            continue
        lump = data[filepos:filepos + size]
        if lump[:8] != GENMIDI_MAGIC:
            raise ValueError("GENMIDI lump has bad magic: %r" % lump[:8])
        # 8-byte header + N*36 records + N*32 names.
        n = (size - 8) // (36 + 32)
        print("  GENMIDI  size=%d  instruments=%d  magic=%s"
              % (size, n, lump[:8].decode("ascii", "ignore")))
        os.makedirs(os.path.dirname(out_path), exist_ok=True)
        with open(out_path, "wb") as f:
            f.write(lump)
        print("Wrote: %s (%d bytes)" % (out_path, size))
        return size

    raise ValueError("GENMIDI lump not found in %s" % wad_path)


def main():
    parser = argparse.ArgumentParser(
        description="Extract the GENMIDI OPL2 instrument bank from a Doom WAD."
    )
    parser.add_argument("--wad", default=DEFAULT_WAD,
                        help="Path to the WAD file (default: DOOM1.WAD in project root).")
    parser.add_argument("--out", default=DEFAULT_OUT,
                        help="Output path (default: tools/midi2vgm/genmidi.op2).")
    args = parser.parse_args()

    wad_path = os.path.abspath(args.wad)
    if not os.path.isfile(wad_path):
        print("Error: WAD file not found: %s" % wad_path, file=sys.stderr)
        return 1

    try:
        extract_genmidi(wad_path, os.path.abspath(args.out))
    except ValueError as exc:
        print("Error: %s" % exc, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
