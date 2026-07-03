#!/usr/bin/env python3
"""
converter.py - MIDI to VGM converter for Sega Mega Drive.

This is the main module that ties together the 5 pipeline stages:
    1. MIDI parser  (midi_parser.py)  - parse SMF into events
    2. Timeline      (built-in)        - convert ticks to VGM samples
    3. MD arranger   (built-in)        - polyphonic voice allocation
    4. YM2612/PSG    (ym2612.py/psg.py) - generate chip register writes
    5. VGM writer    (vgm.py)          - serialize to VGM file

Mega Drive channel allocation:
    FM channels 0-5 : melodic notes, dynamically allocated across a 6-voice
                      pool so chords and every melodic channel can play. The
                      instrument patches come from Doom's own GENMIDI (OPL2)
                      bank translated to the YM2612 (see genmidi.py); if that
                      data file is missing, the hand patches in fm_patches.py
                      are used as a fallback.
    PSG channel 3   : MIDI drum channel (channel 9) as noise.

Usage:
    from midi2vgm.converter import convert_midi_to_vgm
    vgm_bytes = convert_midi_to_vgm(midi_data)
"""

from .midi_parser import parse_midi
from .vgm import VgmWriter, NTSC_FRAME_SAMPLES
from .ym2612 import (note_to_fnum, write_instrument, write_frequency,
                     write_key_on, write_key_off, write_total_level,
                     write_stereo, NUM_FM_CHANNELS)
from .psg import (note_to_psg_freq, velocity_to_psg_volume,
                  psg_set_freq, psg_set_volume, psg_set_noise)
from . import fm_patches
from . import genmidi

# MIDI drum channel (General MIDI percussion).
DRUM_CHANNEL = 9

# YM2612 FM voice pool: all 6 channels are available for melodic notes.
NUM_VOICES = 6

# Carrier operator offsets per algorithm (register-offset order S1, S3, S2, S4).
# GENMIDI patches always use algorithm 4 (two 2-op stacks; carriers S2, S4).
CARRIER_SLOTS = {
    0: [3], 1: [3], 2: [3], 3: [3],
    4: [2, 3], 5: [1, 2, 3], 6: [2, 3], 7: [0, 1, 2, 3],
}

# Pitch-bend range in semitones (MIDI default is +/- 2).
PITCH_BEND_SEMITONES = 2.0

# Instrument source: prefer Doom's GENMIDI bank, fall back to hand patches.
_BANK = genmidi.load_bank()


def _patch_for(program):
    """Return the YM2612 patch for a GM program number."""
    if _BANK is not None:
        return _BANK.get_patch(program)
    return fm_patches.get_patch(program)


class ChannelState:
    """Tracks the state of a single MIDI channel during conversion."""

    def __init__(self):
        self.program = 0            # current GM program number
        self.volume = 100           # CC7 channel volume (0-127)
        self.expression = 127       # CC11 expression (0-127)
        self.pan = 64               # CC10 pan (0-127, 64=center)
        self.pitch_bend = 8192      # pitch bend value (0-16383, 8192=center)


class Voice:
    """One YM2612 FM channel in the dynamic voice pool."""

    def __init__(self, fm_chan):
        self.fm_chan = fm_chan
        self.midi_channel = None    # MIDI channel currently owning this voice
        self.note = -1              # MIDI note currently sounding
        self.age = 0                # allocation order (for LRU stealing)
        self.resident_program = -1  # GM program of the patch loaded on the chip
        self.resident_stereo = -1   # stereo bits last written


def _pan_to_stereo(pan):
    """Convert MIDI pan (0-127) to YM2612 stereo bits."""
    if pan < 43:
        return 0x80                 # Left only
    if pan > 84:
        return 0x40                 # Right only
    return 0xC0                     # Center (L+R)


def _carrier_tl_adjust(volume, expression, velocity):
    """Compute the TL value to ADD to carrier operators (positive = quieter).

    Combines CC7 channel volume, CC11 expression and note velocity into a single
    attenuation offset applied on top of the instrument's base carrier level.
    """
    effective = (velocity * volume * expression) // (127 * 127)
    if effective <= 0:
        return 96                   # effectively silent
    return int((127 - effective) * 0.4)


# Drum note -> PSG noise type mapping.
# Noise type: bit2=white(1)/periodic(0), bits0-1=freq(0=high,3=low).
DRUM_MAP = {
    35: 0x07, 36: 0x07,   # Bass drums (white, low)
    38: 0x07, 40: 0x07,   # Snare drums (white, low)
    42: 0x00,             # Closed Hi-Hat (periodic, high)
    46: 0x04,             # Open Hi-Hat (white, high)
    49: 0x04, 52: 0x07, 57: 0x04,  # Crashes (white)
    51: 0x00, 59: 0x00,   # Ride cymbals (periodic, high)
    41: 0x06, 43: 0x06, 45: 0x05, 47: 0x05, 48: 0x05, 50: 0x04,  # Toms
}
DEFAULT_DRUM = 0x04  # white noise, high


def _handle_drum_on(writer, time, note, velocity):
    """Process a note_on event on the drum (PSG noise) channel."""
    noise_type = DRUM_MAP.get(note, DEFAULT_DRUM)
    vol = velocity_to_psg_volume(velocity)
    writer.add_commands(time, psg_set_noise(noise_type))
    writer.add_commands(time, psg_set_volume(3, vol))


def _handle_drum_off(writer, time, note):
    """Process a note_off event on the drum channel (silence PSG noise)."""
    writer.add_commands(time, psg_set_volume(3, 15))

class VoicePool:
    """Dynamic allocator for the 6 YM2612 FM voices.

    Notes from any melodic MIDI channel compete for the pool. Free voices are
    used first; when all are busy, the oldest sounding voice is stolen (LRU).
    """

    def __init__(self, writer, states):
        self.writer = writer
        self.states = states
        self.voices = [Voice(i) for i in range(NUM_VOICES)]
        self._clock = 0

    def _pick(self, program):
        """Choose a voice for a new note. Prefer a free voice already holding
        the requested patch, then any free voice, then steal the oldest."""
        free = [v for v in self.voices if v.midi_channel is None]
        if free:
            for v in free:
                if v.resident_program == program:
                    return v, False
            return free[0], False
        # All busy: steal the least-recently allocated voice.
        victim = min(self.voices, key=lambda v: v.age)
        return victim, True

    def note_on(self, time, channel, note, velocity):
        state = self.states[channel]
        voice, stolen = self._pick(state.program)
        if stolen:
            self.writer.add_commands(time, write_key_off(voice.fm_chan))

        patch = _patch_for(state.program)

        # (Re)load the instrument only when the resident patch differs.
        if voice.resident_program != state.program:
            self.writer.add_commands(time, write_instrument(voice.fm_chan, patch))
            voice.resident_program = state.program
            voice.resident_stereo = -1  # write_instrument reset stereo

        stereo = _pan_to_stereo(state.pan)
        if voice.resident_stereo != stereo:
            self.writer.add_command(time, write_stereo(voice.fm_chan, stereo)[0])
            voice.resident_stereo = stereo

        # Pitch: MIDI note + instrument transpose + pitch bend (semitones).
        bend = (state.pitch_bend - 8192) / 8192.0 * PITCH_BEND_SEMITONES
        adjusted = note + patch.get("note_offset", 0) + bend
        fnum, block = note_to_fnum(int(round(adjusted)))
        self.writer.add_commands(time, write_frequency(voice.fm_chan, fnum, block))

        # Velocity / volume / expression -> carrier attenuation.
        tl_adj = _carrier_tl_adjust(state.volume, state.expression, velocity)
        for slot in CARRIER_SLOTS.get(patch["algorithm"], [3]):
            base_tl = patch["operators"][slot]["tl"]
            new_tl = max(0, min(127, base_tl + tl_adj))
            self.writer.add_commands(
                time, write_total_level(voice.fm_chan, slot, new_tl))

        self.writer.add_commands(time, write_key_on(voice.fm_chan))
        self._clock += 1
        voice.midi_channel = channel
        voice.note = note
        voice.age = self._clock

    def note_off(self, time, channel, note):
        for voice in self.voices:
            if voice.midi_channel == channel and voice.note == note:
                self.writer.add_commands(time, write_key_off(voice.fm_chan))
                voice.midi_channel = None
                voice.note = -1
                return

    def repan(self, time, channel):
        """Re-apply a channel's pan to all voices it currently owns."""
        stereo = _pan_to_stereo(self.states[channel].pan)
        for voice in self.voices:
            if voice.midi_channel == channel and voice.resident_stereo != stereo:
                self.writer.add_command(
                    time, write_stereo(voice.fm_chan, stereo)[0])
                voice.resident_stereo = stereo


def convert_midi_to_vgm(midi_data, ntsc=True, loop=True):
    """Convert MIDI byte data to VGM byte data for Mega Drive.

    If ``loop`` is set, the VGM loops back to the start after the last event,
    matching how Doom plays its background music.
    """
    midi = parse_midi(midi_data)
    writer = VgmWriter(ntsc=ntsc)

    # Per-MIDI-channel state (program, volume, expression, pan, pitch bend).
    states = {}
    for ev in midi.events:
        if ev.channel is not None and ev.channel not in states:
            states[ev.channel] = ChannelState()

    pool = VoicePool(writer, states)

    for ev in midi.events:
        time = midi.tick_to_sample(ev.tick)
        state = states.get(ev.channel)

        if ev.kind == "program_change" and state:
            state.program = ev.data["program"]

        elif ev.kind == "control_change" and state:
            ctrl, val = ev.data["controller"], ev.data["value"]
            if ctrl == 7:
                state.volume = val
            elif ctrl == 11:
                state.expression = val
            elif ctrl == 10:
                state.pan = val
                if ev.channel != DRUM_CHANNEL:
                    pool.repan(time, ev.channel)

        elif ev.kind == "pitch_bend" and state:
            state.pitch_bend = ev.data["value"]

        elif ev.kind == "note_on":
            note, vel = ev.data["note"], ev.data["velocity"]
            if ev.channel == DRUM_CHANNEL:
                _handle_drum_on(writer, time, note, vel)
            elif state is not None:
                pool.note_on(time, ev.channel, note, vel)

        elif ev.kind == "note_off":
            note = ev.data["note"]
            if ev.channel == DRUM_CHANNEL:
                _handle_drum_off(writer, time, note)
            elif state is not None:
                pool.note_off(time, ev.channel, note)

    if loop:
        return writer.build(loop_offset_samples=0,
                            loop_length_samples=writer.total_samples)
    return writer.build()


# -- File-level convenience and CLI ------------------------------------------ #

def convert_file(midi_path, vgm_path, ntsc=True, loop=True):
    """Convert a .mid file to a .vgm file. Returns output size in bytes."""
    with open(midi_path, "rb") as f:
        midi_data = f.read()
    vgm_data = convert_midi_to_vgm(midi_data, ntsc=ntsc, loop=loop)
    with open(vgm_path, "wb") as f:
        f.write(vgm_data)
    return len(vgm_data)

