#!/usr/bin/env python3
"""Test MIDI to VGM conversion."""
import os, sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from midi2vgm.converter import convert_file

midi_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
    "res", "music", "d_e1m1.mid")
vgm_path = sys.argv[2] if len(sys.argv) > 2 else midi_path.replace(".mid", ".vgm")

print("Converting: %s -> %s" % (midi_path, vgm_path))
size = convert_file(midi_path, vgm_path)
print("VGM size: %d bytes" % size)

# Verify VGM header.
import struct
with open(vgm_path, "rb") as f:
    d = f.read()
print("Magic: %s" % d[:4])
ver = struct.unpack_from("<I", d, 8)[0]
sn = struct.unpack_from("<I", d, 0x0C)[0]
ym = struct.unpack_from("<I", d, 0x30)[0]
doff = struct.unpack_from("<I", d, 0x34)[0]
total = struct.unpack_from("<I", d, 0x18)[0]
rate = struct.unpack_from("<I", d, 0x24)[0]
print("Version: 0x%08x" % ver)
print("SN76489 clock: %d" % sn)
print("YM2612 clock: %d" % ym)
print("Data offset (rel 0x34): %d -> data at 0x%x" % (doff, 0x34 + doff))
print("Total samples: %d (%.1f sec)" % (total, total / 44100.0))
print("Rate: %d Hz" % rate)
print("File size: %d" % len(d))

# Scan data for command types.
data_start = 0x34 + doff
pos = data_start
cmds = {"ym_p0": 0, "ym_p1": 0, "psg": 0, "wait": 0, "end": 0, "other": 0}
while pos < len(d):
    cmd = d[pos]
    if cmd == 0x52:
        cmds["ym_p0"] += 1; pos += 3
    elif cmd == 0x53:
        cmds["ym_p1"] += 1; pos += 3
    elif cmd == 0x50:
        cmds["psg"] += 1; pos += 2
    elif cmd == 0x61:
        cmds["wait"] += 1; pos += 3
    elif cmd == 0x62:
        cmds["wait"] += 1; pos += 1
    elif 0x70 <= cmd <= 0x7F:
        cmds["wait"] += 1; pos += 1
    elif cmd == 0x66:
        cmds["end"] += 1; pos += 1
        break
    else:
        cmds["other"] += 1; pos += 1

print("Commands: %s" % cmds)
