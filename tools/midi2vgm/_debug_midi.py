#!/usr/bin/env python3
"""Debug: parse a MIDI file and report structure."""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from midi2vgm.midi_parser import parse_midi

path = sys.argv[1]
with open(path, "rb") as f:
    data = f.read()

print("File: %s (%d bytes)" % (path, len(data)))
t0 = time.time()
midi = parse_midi(data)
t1 = time.time()
print("Parse time: %.3fs" % (t1 - t0))
print("Division: %d" % midi.division)
print("Events: %d" % len(midi.events))
print("Tempo changes: %d" % len(midi.tempo_changes))

# Count event types
types = {}
for ev in midi.events:
    types[ev.kind] = types.get(ev.kind, 0) + 1
print("Event types: %s" % types)

# Check channels
chans = set()
for ev in midi.events:
    if ev.channel is not None:
        chans.add(ev.channel)
print("Channels: %s" % sorted(chans))

# Check max tick
max_tick = max(ev.tick for ev in midi.events)
print("Max tick: %d" % max_tick)

# Time tick_to_sample for last event
t2 = time.time()
sample = midi.tick_to_sample(max_tick)
t3 = time.time()
print("tick_to_sample(max_tick): %d samples (%.1f sec) [%.3fs]" % (
    sample, sample / 44100.0, t3 - t2))

# Time tick_to_sample for all events
t4 = time.time()
for ev in midi.events:
    midi.tick_to_sample(ev.tick)
t5 = time.time()
print("tick_to_sample all events: %.3fs" % (t5 - t4))
