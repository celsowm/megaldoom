#!/usr/bin/env python3
"""
batch_midi2vgm.py - Batch-convert all .mid files in a directory to .vgm files.

Usage:
    python tools/midi2vgm/batch_midi2vgm.py res/music res/music
"""
import os, sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from midi2vgm.converter import convert_file

midi_dir = sys.argv[1] if len(sys.argv) > 1 else "."
vgm_dir = sys.argv[2] if len(sys.argv) > 2 else midi_dir

ok = 0
fail = 0
for f in sorted(os.listdir(midi_dir)):
    if not f.endswith(".mid"):
        continue
    in_path = os.path.join(midi_dir, f)
    out_path = os.path.join(vgm_dir, f.replace(".mid", ".vgm"))
    try:
        size = convert_file(in_path, out_path)
        print("  %-20s -> %d bytes" % (f, size))
        ok += 1
    except Exception as e:
        print("  %-20s -> FAIL: %s" % (f, e))
        fail += 1

print("\nConverted: %d OK, %d failed" % (ok, fail))
