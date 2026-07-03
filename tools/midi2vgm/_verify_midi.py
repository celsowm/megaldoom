#!/usr/bin/env python3
"""Verify a MIDI file: parse and report event statistics."""
import struct, sys

d = open(sys.argv[1], 'rb').read()

# MThd
sig = d[:4]
length, fmt, ntrk, div = struct.unpack('>IHHH', d[4:14])
print(f"MThd: sig={sig} len={length} format={fmt} ntracks={ntrk} division={div}")

# MTrk
trk_sig = d[14:18]
trk_len = struct.unpack('>I', d[18:22])[0]
print(f"MTrk: sig={trk_sig} track_len={trk_len} file_size={len(d)} expected={22+trk_len}")

# Parse events
pos = 22
notes_on = notes_off = prog_chg = ctrl_chg = pitch_bend = sysex = meta = 0
total = 0
channels = set()
programs = set()
ctrls = set()

while pos < 22 + trk_len:
    # VLQ delta
    delta = 0
    while True:
        b = d[pos]; pos += 1
        delta = (delta << 7) | (b & 0x7F)
        if not (b & 0x80): break
    # Status byte
    status = d[pos]; pos += 1
    if status == 0xFF:
        mt = d[pos]; pos += 1
        ml = d[pos]; pos += 1
        pos += ml
        meta += 1; total += 1
        continue
    high = status >> 4; ch = status & 0x0F
    channels.add(ch)
    if high == 0x9:
        note = d[pos]; vel = d[pos+1]; pos += 2
        notes_on += 1
    elif high == 0x8:
        pos += 2; notes_off += 1
    elif high == 0xC:
        prog = d[pos]; pos += 1; prog_chg += 1; programs.add(prog)
    elif high == 0xB:
        cc = d[pos]; val = d[pos+1]; pos += 2; ctrl_chg += 1; ctrls.add(cc)
    elif high == 0xE:
        pos += 2; pitch_bend += 1
    elif high == 0xF:
        # sysex - read until F7
        while d[pos] != 0xF7: pos += 1
        pos += 1; sysex += 1
    else:
        pos += 2
    total += 1

print(f"Events: total={total} note_on={notes_on} note_off={notes_off}")
print(f"        prog_chg={prog_chg} ctrl_chg={ctrl_chg} pitch_bend={pitch_bend}")
print(f"        sysex={sysex} meta={meta}")
print(f"Channels used: {sorted(channels)}")
print(f"Drum channel 9 present: {9 in channels}")
print(f"Programs: {sorted(programs)}")
print(f"CC numbers: {sorted(ctrls)}")
