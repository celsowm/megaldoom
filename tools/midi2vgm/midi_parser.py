#!/usr/bin/env python3
"""
midi_parser.py - Minimal Standard MIDI File (SMF) parser.

Reads a type-0 (single-track) MIDI file and produces a sorted list of events
with absolute tick times.  This is Stage 1 of the MIDI->VGM pipeline.

Supported events:
    note_on(note, velocity, channel, tick)
    note_off(note, velocity, channel, tick)
    program_change(program, channel, tick)
    control_change(controller, value, channel, tick)
    pitch_bend(value, channel, tick)
    tempo(us_per_quarter, tick)
    end_of_track(tick)

Meta events other than tempo and end-of-track are skipped.  Channel-mode
messages (CC 120-127) are skipped.  Running status is handled.
"""

import struct

# MIDI event status bytes.
NOTE_OFF = 0x80
NOTE_ON = 0x90
AFTER_TOUCH = 0xA0
CONTROL_CHANGE = 0xB0
PROGRAM_CHANGE = 0xC0
CHANNEL_PRESSURE = 0xD0
PITCH_BEND = 0xE0
META_EVENT = 0xFF
SYSEX = 0xF0

# Data byte counts per status (high nibble).
DATA_BYTES = {
    NOTE_OFF: 2, NOTE_ON: 2, AFTER_TOUCH: 2,
    CONTROL_CHANGE: 2, PROGRAM_CHANGE: 1,
    CHANNEL_PRESSURE: 1, PITCH_BEND: 2,
}


class MidiEvent:
    """A single MIDI event with an absolute tick time."""

    __slots__ = ("tick", "kind", "channel", "data")

    def __init__(self, tick, kind, channel, data):
        self.tick = tick
        self.kind = kind          # 'note_on', 'note_off', etc.
        self.channel = channel    # 0-15 (or None for meta events)
        self.data = data          # dict of event-specific fields

    def __repr__(self):
        return "MidiEvent(tick=%d, kind=%s, ch=%s, data=%s)" % (
            self.tick, self.kind, self.channel, self.data)


class MidiFile:
    """Parsed SMF file: division, tempo map, sorted event list."""

    def __init__(self, division, events):
        self.division = division        # ticks per quarter note (PPQN)
        self.events = events            # sorted by tick
        self.tempo_changes = []         # list of (tick, us_per_quarter)
        self._build_tempo_map()

    def _build_tempo_map(self):
        """Extract tempo changes from the event list."""
        for ev in self.events:
            if ev.kind == "tempo":
                self.tempo_changes.append((ev.tick, ev.data["us_per_quarter"]))
        if not self.tempo_changes:
            self.tempo_changes.append((0, 500000))  # default 120 BPM

    def tick_to_sample(self, tick, sample_rate=44100):
        """Convert an absolute MIDI tick to VGM samples (44100 Hz).

        Uses the tempo map to compute the exact time in microseconds,
        then converts to samples.  division is in PPQN (ticks per quarter).
        """
        if tick <= 0:
            return 0
        total_us = 0
        prev_tick = 0
        tempo = self.tempo_changes[0][1] if self.tempo_changes else 500000

        for change_tick, change_tempo in self.tempo_changes:
            if change_tick >= tick:
                break
            # Ticks from prev_tick to change_tick at current tempo.
            ticks_here = change_tick - prev_tick
            total_us += ticks_here * tempo // self.division
            prev_tick = change_tick
            tempo = change_tempo

        # Remaining ticks from prev_tick to target.
        remaining = tick - prev_tick
        total_us += remaining * tempo // self.division

        return (total_us * sample_rate) // 1000000


def _read_vlq(data, pos):
    """Read a MIDI variable-length quantity. Returns (value, new_pos)."""
    value = 0
    while True:
        b = data[pos]
        pos += 1
        value = (value << 7) | (b & 0x7F)
        if not (b & 0x80):
            break
    return value, pos


def parse_midi(data):
    """
    Parse SMF byte data into a MidiFile.

    Handles type-0 (single track) and type-1 (multi-track, merged) files.
    Returns a MidiFile with division and a sorted event list.
    """
    pos = 0

    # SMF header: MThd + length(4) + format(2) + ntracks(2) + division(2)
    if data[:4] != b"MThd":
        raise ValueError("Not a MIDI file: missing MThd")
    hdr_len = struct.unpack_from(">I", data, 4)[0]
    fmt, ntrk, division = struct.unpack_from(">HHH", data, 8)
    pos = 8 + hdr_len

    all_events = []

    for _track in range(ntrk):
        if data[pos:pos + 4] != b"MTrk":
            # Skip unknown chunks.
            chunk_id = data[pos:pos + 4]
            chunk_len = struct.unpack_from(">I", data, pos + 4)[0]
            pos += 8 + chunk_len
            continue

        track_len = struct.unpack_from(">I", data, pos + 4)[0]
        track_start = pos + 8
        track_end = track_start + track_len
        pos = track_start

        tick = 0
        running_status = None

        while pos < track_end:
            delta, pos = _read_vlq(data, pos)
            tick += delta

            status = data[pos]
            if status < 0x80:
                # Running status: reuse previous status byte.
                status = running_status
            else:
                pos += 1
                running_status = status

            if status == META_EVENT:
                meta_type = data[pos]
                pos += 1
                meta_len, pos = _read_vlq(data, pos)
                meta_data = data[pos:pos + meta_len]
                pos += meta_len

                if meta_type == 0x51:  # Set tempo
                    us_per_qn = (meta_data[0] << 16) | (meta_data[1] << 8) | meta_data[2]
                    all_events.append(MidiEvent(tick, "tempo", None,
                                                {"us_per_quarter": us_per_qn}))
                elif meta_type == 0x2F:  # End of track
                    all_events.append(MidiEvent(tick, "end_of_track", None, {}))

            elif status == SYSEX:
                # Skip sysex (read until 0xF7).
                end = pos
                while data[end] != 0xF7:
                    end += 1
                pos = end + 1

            else:
                high = status & 0xF0
                channel = status & 0x0F
                nbytes = DATA_BYTES.get(high, 0)

                if high == NOTE_ON:
                    note, vel = data[pos], data[pos + 1]
                    pos += 2
                    if vel == 0:
                        all_events.append(MidiEvent(
                            tick, "note_off", channel,
                            {"note": note, "velocity": vel}))
                    else:
                        all_events.append(MidiEvent(
                            tick, "note_on", channel,
                            {"note": note, "velocity": vel}))

                elif high == NOTE_OFF:
                    note, vel = data[pos], data[pos + 1]
                    pos += 2
                    all_events.append(MidiEvent(
                        tick, "note_off", channel,
                        {"note": note, "velocity": vel}))

                elif high == PROGRAM_CHANGE:
                    program = data[pos]
                    pos += 1
                    all_events.append(MidiEvent(
                        tick, "program_change", channel,
                        {"program": program}))

                elif high == CONTROL_CHANGE:
                    ctrl, val = data[pos], data[pos + 1]
                    pos += 2
                    if ctrl < 120:  # skip channel-mode messages
                        all_events.append(MidiEvent(
                            tick, "control_change", channel,
                            {"controller": ctrl, "value": val}))

                elif high == PITCH_BEND:
                    lsb, msb = data[pos], data[pos + 1]
                    pos += 2
                    bend = (msb << 7) | lsb
                    all_events.append(MidiEvent(
                        tick, "pitch_bend", channel,
                        {"value": bend}))

                else:
                    pos += nbytes

    all_events.sort(key=lambda e: (e.tick, 0 if e.kind == "note_off" else 1))
    return MidiFile(division, all_events)

