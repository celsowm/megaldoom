#!/usr/bin/env python3
"""
extract-sfx.py - Extract Doom sound-effect lumps from a WAD file and convert
                 them to standard WAV files for SGDK's XGM2 PCM driver.

Doom stores its sound effects in id Software's DMX format. The lumps are named
DS* (DSPISTOL, DSSHOTGN, DSPOPAIN, DSPLDETH, ...). Music lumps start with "D_"
and are correctly excluded by the "DS" prefix filter.

Each DMX lump is laid out as:

    offset  size  field
    0       2     format version  (always 3 for standard Doom sounds)
    2       2     sample rate     (typically 11025 Hz)
    4       2     number of samples
    6       2     reserved / pad  (0)
    8       N     N bytes of 8-bit *unsigned* PCM (128 == silence)

The PCM payload is wrapped into a standard mono 8-bit unsigned WAV so that
SGDK's rescomp can convert it for the XGM2 driver (which resamples to a fixed
13300 Hz and converts to 8-bit signed). Writing a plain 8-bit unsigned WAV is
the correct input because Doom's samples are already 8-bit unsigned, so no
sample-level transform is required here -- only the WAV container.

The WAV files are written to res/sound/ for inclusion via res/resources.res:

    WAV sfx_pistol "sound/dspistol.wav" XGM2

Usage:
    python tools/extract-sfx.py [--wad DOOM1.WAD] [--out res/sound]
"""

import argparse
import os
import struct
import sys

# Project root is the parent of the tools/ directory.
PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_WAD = os.path.join(PROJECT_ROOT, "DOOM1.WAD")
DEFAULT_OUT = os.path.join(PROJECT_ROOT, "res", "sound")

# WAD header: signature (4s), numLumps (i), dirOffset (i) -- all little-endian.
WAD_HEADER_FMT = "<4sii"
WAD_HEADER_SIZE = struct.calcsize(WAD_HEADER_FMT)
# Each directory entry: filepos (i), size (i), name (8s) -- 16 bytes.
LUMP_ENTRY_FMT = "<ii8s"
LUMP_ENTRY_SIZE = struct.calcsize(LUMP_ENTRY_FMT)

# DMX sound header (8 bytes, little-endian): version, rate, nSamples, pad.
DMX_HEADER_FMT = "<HHHH"
DMX_HEADER_SIZE = struct.calcsize(DMX_HEADER_FMT)
DMX_VERSION = 3

# Curated set of gameplay-relevant SFX. Kept lean to control ROM size while
# covering the events MegalDoom currently emits (fire, enemy hit/kill, player
# pain/death, door action, item pickup). Add more here as new events are wired.
DEFAULT_SFX = (
    "DSPISTOL",   # pistol fire
    "DSPOPAIN",   # enemy pain (hit)
    "DSPODTH1",   # enemy death (kill)
    "DSPLPAIN",   # player pain
    "DSPLDETH",   # player death
    "DSSTNMOV",   # door / platform move
    "DSITEMUP",   # item pickup
    "DSBAREXP",   # exploding barrel
    "DSSHOTGN",   # shotgun fire
    "DSPUNCH",    # fist swing
    "DSSAWFUL",   # chainsaw swing
    "DSWPNUP",    # weapon pickup
)
# The chaingun deliberately reuses DSPISTOL, as Doom does.


def read_wad_directory(wad_path):
    """Read the WAD file and return (raw_bytes, signature, [(name, filepos, size), ...])."""
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


def write_wav(path, pcm_bytes, sample_rate):
    """Write a mono 8-bit unsigned PCM payload as a standard WAV file."""
    n = len(pcm_bytes)
    # 8-bit, mono, PCM: byte_rate = sample_rate, block_align = 1.
    fmt_chunk = struct.pack(
        "<HHIIHH",
        1,                  # audioFormat = PCM
        1,                  # numChannels = mono
        sample_rate,
        sample_rate,        # byte_rate = rate * channels * (bits/8)
        1,                  # block_align = channels * (bits/8)
        8,                  # bits per sample
    )

    riff_size = 4 + (8 + len(fmt_chunk)) + (8 + n)  # "WAVE" + fmt + data
    with open(path, "wb") as f:
        f.write(b"RIFF")
        f.write(struct.pack("<I", riff_size))
        f.write(b"WAVE")
        f.write(b"fmt ")
        f.write(struct.pack("<I", len(fmt_chunk)))
        f.write(fmt_chunk)
        f.write(b"data")
        f.write(struct.pack("<I", n))
        f.write(pcm_bytes)


def extract_sfx_lumps(wad_path, out_dir, names):
    """Extract the requested DS* lumps as WAV files."""
    data, signature, lumps = read_wad_directory(wad_path)
    print("Reading WAD: %s" % wad_path)
    print("  signature=%s  lumps=%d" % (signature, len(lumps)))

    os.makedirs(out_dir, exist_ok=True)
    print("Writing WAV files to: %s" % out_dir)

    # Index lumps by name so we can look up the curated set directly.
    by_name = {}
    for name, filepos, size in lumps:
        by_name[name] = (filepos, size)

    count = 0
    skipped = 0
    for want in names:
        if want not in by_name:
            print("  ! %s: lump not found in WAD, skipping" % want)
            skipped += 1
            continue

        filepos, size = by_name[want]
        if size < DMX_HEADER_SIZE:
            print("  ! %s: too small for DMX header (%d bytes), skipping" % (want, size))
            skipped += 1
            continue

        lump_data = data[filepos:filepos + size]
        version, sample_rate, num_samples, pad = struct.unpack_from(
            DMX_HEADER_FMT, lump_data, 0
        )

        if version != DMX_VERSION:
            print("  ! %s: unexpected DMX version %d (expected %d), skipping"
                  % (want, version, DMX_VERSION))
            skipped += 1
            continue

        pcm = lump_data[DMX_HEADER_SIZE:DMX_HEADER_SIZE + num_samples]
        if len(pcm) != num_samples:
            print("  ! %s: truncated PCM (%d of %d samples), skipping"
                  % (want, len(pcm), num_samples))
            skipped += 1
            continue

        out_path = os.path.join(out_dir, want.lower() + ".wav")
        write_wav(out_path, pcm, sample_rate)
        print("  %-10s  size=%-6d  rate=%-5d  samples=%-6d  -> %s"
              % (want, size, sample_rate, num_samples,
                 os.path.relpath(out_path, PROJECT_ROOT)))
        count += 1

    print("-" * 52)
    print("Extracted: %d WAV files" % count)
    if skipped:
        print("Skipped:   %d (missing or invalid lumps)" % skipped)
    return count


def main():
    parser = argparse.ArgumentParser(
        description="Extract Doom DS* sound-effect lumps to WAV for SGDK XGM2."
    )
    parser.add_argument(
        "--wad", default=DEFAULT_WAD,
        help="Path to the WAD file (default: DOOM1.WAD in project root)."
    )
    parser.add_argument(
        "--out", default=DEFAULT_OUT,
        help="Output directory (default: res/sound)."
    )
    parser.add_argument(
        "--names", nargs="*", default=list(DEFAULT_SFX),
        help="DS lump names to extract (default: curated gameplay set)."
    )
    args = parser.parse_args()

    wad_path = os.path.abspath(args.wad)
    if not os.path.isfile(wad_path):
        print("Error: WAD file not found: %s" % wad_path, file=sys.stderr)
        return 1

    extract_sfx_lumps(wad_path, os.path.abspath(args.out),
                      [n.upper() for n in args.names])
    return 0


if __name__ == "__main__":
    sys.exit(main())
