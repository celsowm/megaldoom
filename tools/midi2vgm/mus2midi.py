#!/usr/bin/env python3
"""
mus2midi.py - Convert Doom MUS format to Standard MIDI File (SMF).

MUS is id Software's compact MIDI variant used for Doom's music. This module
converts it to a standard MIDI file that downstream tools (midi2vgm) can parse.

The conversion follows the reference implementation in Chocolate Doom's
mus2mid.c (Ben Ryves, 2006):

  - Output: SMF type 0 (single track), division = 70 PPQN.
  - With the default MIDI tempo of 500000 us/quarter (120 BPM), 70 PPQN gives
    exactly 140 ticks/second, matching the MUS player's 140 Hz timer. MUS
    delays therefore map 1:1 to MIDI delta times.
  - MUS controller 0 = MIDI Program Change (NOT CC 0).
  - MUS controller 3 = CC 7 (Volume), 4 = CC 10 (Pan), 5 = CC 11 (Expression),
    8 = CC 64 (Sustain), etc. (see CONTROLLER_MAP below).
  - MUS channel 15 (percussion) maps to MIDI channel 9 (General MIDI drums).
    Other MUS channels are dynamically assigned to MIDI channels in order of
    first use, skipping channel 9 to avoid the drums channel.
  - Note velocity is tracked per-channel (not per-note). A play-note event with
    the volume-change flag updates the channel velocity; subsequent notes on
    that channel use the cached velocity.

Usage:
    python tools/midi2vgm/mus2midi.py input.mus output.mid
    python tools/midi2vgm/mus2midi.py res/music/mus/d_e1m1.mus res/music/d_e1m1.mid
"""

import argparse
import os
import struct
import sys

# --------------------------------------------------------------------------- #
# Constants
# --------------------------------------------------------------------------- #

MUS_MAGIC = b"MUS\x1a"

# MUS header: ID(4) + scoreLen(2) + scoreStart(2) + channels(2)
#             + sec_channels(2) + instrCnt(2) + pad(2) = 16 bytes.
MUS_HEADER_FMT = "<4sHHHHHH"
MUS_HEADER_SIZE = struct.calcsize(MUS_HEADER_FMT)

# MUS event types (bits 6-4 of the event descriptor byte).
MUS_RELEASE_KEY = 0   # Note Off  - 1 data byte (note number)
MUS_PRESS_KEY = 1     # Note On   - 1 data byte (bit 7=vol-change, bits 6-0=note)
                      #              if vol-change: +1 byte (velocity 0-127)
MUS_PITCH_WHEEL = 2   # Pitch Bend - 1 data byte (0-255, 128=center)
MUS_SYSTEM_EVENT = 3  # System     - 1 data byte (event number 10-14)
MUS_CONTROLLER = 4    # Controller - 2 data bytes (controller number, value)
MUS_SCORE_END = 6     # End of score - no data bytes (byte 0x60, NOT 0x50)

# MUS controller numbers 1-9 map to MIDI CC numbers via this table.
# Controller 0 is special: it becomes a MIDI Program Change, not a CC.
CONTROLLER_MAP = [
    0x00,   # 0: (unused - handled as program change)
    0x20,   # 1: CC 32  (Bank Select LSB)
    0x01,   # 2: CC 1   (Modulation Wheel)
    0x07,   # 3: CC 7   (Channel Volume)
    0x0A,   # 4: CC 10  (Pan)
    0x0B,   # 5: CC 11  (Expression)
    0x5B,   # 6: CC 91  (Reverb)
    0x5D,   # 7: CC 93  (Chorus)
    0x40,   # 8: CC 64  (Sustain Pedal)
    0x43,   # 9: CC 67  (Soft Pedal)
    0x78,   # 10: CC 120 (All Sound Off)      - system event
    0x7B,   # 11: CC 123 (All Notes Off)      - system event
    0x7E,   # 12: CC 126 (Mono Mode)          - system event
    0x7F,   # 13: CC 127 (Poly Mode)          - system event
    0x79,   # 14: CC 121 (Reset All Controllers) - system event
]

# System event sysex messages (MUS system events 10-12).
SYSTEM_EVENT_SYSEX = {
    10: bytes([0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7]),
    11: bytes([0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7]),
    12: bytes([0xF0, 0x7E, 0x7F, 0x09, 0x03, 0xF7]),
}

# MIDI event status bytes.
MIDI_NOTE_OFF = 0x80
MIDI_NOTE_ON = 0x90
MIDI_CONTROL_CHANGE = 0xB0
MIDI_PROGRAM_CHANGE = 0xC0
MIDI_PITCH_BEND = 0xE0

# MIDI meta events.
META_END_OF_TRACK = b"\xFF\x2F\x00"
META_TEMPO = 0x51

# MUS percussion channel (15) maps to MIDI channel 9 (General MIDI drums).
MUS_PERCUSSION_CHAN = 15
MIDI_PERCUSSION_CHAN = 9
NUM_CHANNELS = 16

# MIDI division: 70 PPQN. With default tempo 500000 us/qn (120 BPM),
# this gives 140 ticks/second = MUS timer rate.
MIDI_DIVISION = 70
DEFAULT_TEMPO = 500000
DEFAULT_VELOCITY = 100


# --------------------------------------------------------------------------- #
# MIDI writer helpers
# --------------------------------------------------------------------------- #

def encode_vlq(value):
    """Encode a non-negative integer as a MIDI variable-length quantity."""
    if value < 0:
        raise ValueError("VLQ values must be non-negative, got %d" % value)
    if value == 0:
        return b"\x00"
    parts = []
    while value > 0:
        parts.append(value & 0x7F)
        value >>= 7
    parts.reverse()
    for i in range(len(parts) - 1):
        parts[i] |= 0x80
    return bytes(parts)


def tempo_meta(tempo_us_per_qn):
    """Build a MIDI tempo meta event: FF 51 03 tttttt (big-endian 3 bytes)."""
    t = tempo_us_per_qn
    return bytes([0xFF, META_TEMPO, 0x03,
                  (t >> 16) & 0xFF, (t >> 8) & 0xFF, t & 0xFF])


# --------------------------------------------------------------------------- #
# MUS parser and converter
# --------------------------------------------------------------------------- #

class MusToMidiConverter:
    """Convert a MUS byte stream into a Standard MIDI File (type 0)."""

    def __init__(self, mus_data):
        self.data = mus_data
        self.pos = 0

        # Dynamic MUS->MIDI channel mapping (filled on first use).
        self.channel_map = [-1] * NUM_CHANNELS
        self.next_midi_chan = 0

        # Per-channel velocity cache (updated by play-note volume-change flag).
        self.channel_velocity = [DEFAULT_VELOCITY] * NUM_CHANNELS

        # MIDI event buffer: list of (delta_time, event_bytes).
        self.events = []
        self.queued_time = 0

    def _read_byte(self):
        if self.pos >= len(self.data):
            raise EOFError("Unexpected end of MUS data at position %d" % self.pos)
        b = self.data[self.pos]
        self.pos += 1
        return b

    def _map_channel(self, mus_channel):
        """Map a MUS channel to a MIDI channel, allocating on first use."""
        if self.channel_map[mus_channel] != -1:
            return self.channel_map[mus_channel]
        if mus_channel == MUS_PERCUSSION_CHAN:
            midi_chan = MIDI_PERCUSSION_CHAN
        else:
            midi_chan = self.next_midi_chan
            self.next_midi_chan += 1
            if self.next_midi_chan == MIDI_PERCUSSION_CHAN:
                self.next_midi_chan += 1
        self.channel_map[mus_channel] = midi_chan
        return midi_chan

    def _emit(self, event_bytes):
        """Emit a MIDI event with the current queued delta time, then reset it."""
        self.events.append((self.queued_time, event_bytes))
        self.queued_time = 0

    def _read_delay(self):
        """Read a VLQ-encoded delay from the MUS stream and add to queued_time."""
        delay = 0
        while True:
            b = self._read_byte()
            delay = delay * 128 + (b & 0x7F)
            if not (b & 0x80):
                break
        self.queued_time += delay

    def convert(self):
        """Parse the MUS data and return a complete SMF type-0 byte string."""
        if len(self.data) < MUS_HEADER_SIZE:
            raise ValueError("MUS data too small for header: %d bytes" % len(self.data))

        (mus_id, score_len, score_start, primary_channels,
         sec_channels, instr_count, pad) = struct.unpack_from(
            MUS_HEADER_FMT, self.data, 0)

        if mus_id != MUS_MAGIC:
            raise ValueError("Not a MUS file: magic=%r" % mus_id)

        # Position the read cursor at the start of the event stream.
        self.pos = score_start
        score_end_pos = score_start + score_len

        # --- Parse event stream ---
        hit_score_end = False
        while not hit_score_end and self.pos < score_end_pos:
            # Read a group of simultaneous events. The group ends when an
            # event has bit 7 set (the "last" flag).
            while True:
                event_desc = self._read_byte()
                last_flag = bool(event_desc & 0x80)
                event_type = (event_desc >> 4) & 0x07
                mus_channel = event_desc & 0x0F
                midi_channel = self._map_channel(mus_channel)

                if event_type == MUS_RELEASE_KEY:
                    note = self._read_byte() & 0x7F
                    self._emit(bytes([MIDI_NOTE_OFF | midi_channel, note, 0]))

                elif event_type == MUS_PRESS_KEY:
                    key_byte = self._read_byte()
                    has_volume = bool(key_byte & 0x80)
                    note = key_byte & 0x7F
                    if has_volume:
                        vel = self._read_byte() & 0x7F
                        self.channel_velocity[mus_channel] = vel if vel > 0 else 1
                    velocity = self.channel_velocity[mus_channel]
                    self._emit(bytes([MIDI_NOTE_ON | midi_channel, note, velocity]))

                elif event_type == MUS_PITCH_WHEEL:
                    bend = self._read_byte()
                    midi_bend = bend << 6  # 0->0, 128->8192, 255->16320
                    lsb = midi_bend & 0x7F
                    msb = (midi_bend >> 7) & 0x7F
                    self._emit(bytes([MIDI_PITCH_BEND | midi_channel, lsb, msb]))

                elif event_type == MUS_SYSTEM_EVENT:
                    sys_event = self._read_byte()
                    if sys_event in SYSTEM_EVENT_SYSEX:
                        self._emit(SYSTEM_EVENT_SYSEX[sys_event])

                elif event_type == MUS_CONTROLLER:
                    ctrl_num = self._read_byte()
                    ctrl_val = self._read_byte()
                    if ctrl_num == 0:
                        program = ctrl_val & 0x7F
                        self._emit(bytes([MIDI_PROGRAM_CHANGE | midi_channel, program]))
                    elif 1 <= ctrl_num <= 9:
                        midi_cc = CONTROLLER_MAP[ctrl_num]
                        self._emit(bytes([MIDI_CONTROL_CHANGE | midi_channel,
                                          midi_cc, ctrl_val & 0x7F]))

                elif event_type == MUS_SCORE_END:
                    hit_score_end = True
                    break

                else:
                    raise ValueError("Unknown MUS event type %d at pos %d"
                                     % (event_type, self.pos - 1))

                if last_flag:
                    break

            if not hit_score_end:
                self._read_delay()

        return self._build_smf()

    def _build_smf(self):
        """Assemble the SMF type-0 byte string from collected events."""
        track = bytearray()

        # Tempo meta event at delta=0.
        track += encode_vlq(0)
        track += tempo_meta(DEFAULT_TEMPO)

        # All MIDI events with their delta times.
        for delta, event_bytes in self.events:
            track += encode_vlq(delta)
            track += event_bytes

        # End of track meta event at delta=0.
        track += encode_vlq(0)
        track += META_END_OF_TRACK

        # SMF header: MThd + length(4) + format(2) + ntracks(2) + division(2).
        header = b"MThd" + struct.pack(">IHHH", 6, 0, 1, MIDI_DIVISION)
        # Track header: MTrk + length(4, big-endian) + track data.
        track_header = b"MTrk" + struct.pack(">I", len(track))
        return header + track_header + bytes(track)


# --------------------------------------------------------------------------- #
# Public API
# --------------------------------------------------------------------------- #

def mus_to_midi(mus_data):
    """Convert MUS bytes to SMF type-0 bytes. Raises ValueError on bad input."""
    return MusToMidiConverter(mus_data).convert()


def convert_file(mus_path, midi_path):
    """Convert a .mus file to a .mid file. Returns the output size in bytes."""
    with open(mus_path, "rb") as f:
        mus_data = f.read()
    midi_data = mus_to_midi(mus_data)
    with open(midi_path, "wb") as f:
        f.write(midi_data)
    return len(midi_data)


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #

def main():
    parser = argparse.ArgumentParser(
        description="Convert Doom MUS format to Standard MIDI File (SMF type 0)."
    )
    parser.add_argument("input", help="Input .mus file path")
    parser.add_argument("output", help="Output .mid file path")
    args = parser.parse_args()

    mus_path = os.path.abspath(args.input)
    midi_path = os.path.abspath(args.output)

    if not os.path.isfile(mus_path):
        print("Error: MUS file not found: %s" % mus_path, file=sys.stderr)
        return 1

    try:
        size = convert_file(mus_path, midi_path)
        print("Converted: %s -> %s (%d bytes)" % (mus_path, midi_path, size))
    except (ValueError, EOFError) as exc:
        print("Error: %s" % exc, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
