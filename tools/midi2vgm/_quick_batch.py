#!/usr/bin/env python3
"""Quick batch: convert specified MIDI files to VGM."""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from midi2vgm.converter import convert_file

base = r"C:\Users\celso\Documents\projetos\megaldoom\res\music"
files = sys.argv[1:] if len(sys.argv) > 1 else [
    "d_e1m1", "d_e1m2", "d_e1m3", "d_e1m4", "d_e1m5",
]

ok = fail = 0
for name in files:
    mp = os.path.join(base, name + ".mid")
    vp = os.path.join(base, name + ".vgm")
    t0 = time.time()
    try:
        sz = convert_file(mp, vp)
        dt = time.time() - t0
        print("  %-14s -> %6d bytes  (%.1fs)" % (name, sz, dt))
        ok += 1
    except Exception as e:
        print("  %-14s -> FAIL: %s" % (name, e))
        fail += 1

print("\n%d OK, %d failed" % (ok, fail))
