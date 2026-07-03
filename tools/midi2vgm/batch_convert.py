#!/usr/bin/env python3
"""Batch-convert all .mus files in a directory to .mid files."""
import os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mus2midi import convert_file

mus_dir = sys.argv[1] if len(sys.argv) > 1 else "."
out_dir = sys.argv[2] if len(sys.argv) > 2 else mus_dir

ok = 0
fail = 0
for f in sorted(os.listdir(mus_dir)):
    if not f.endswith(".mus"):
        continue
    in_path = os.path.join(mus_dir, f)
    out_path = os.path.join(out_dir, f.replace(".mus", ".mid"))
    try:
        size = convert_file(in_path, out_path)
        print("  %-20s -> %d bytes" % (f, size))
        ok += 1
    except Exception as e:
        print("  %-20s -> FAIL: %s" % (f, e))
        fail += 1

print("\nConverted: %d OK, %d failed" % (ok, fail))
