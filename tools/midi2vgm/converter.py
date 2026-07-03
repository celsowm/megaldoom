#!/usr/bin/env python3
"""
converter.py - MIDI to VGM converter for Sega Mega Drive.

This is the main module that ties together the 5 pipeline stages:
    1. MIDI parser  (midi_parser.py)  - parse SMF into events
    2. Timeline      (built-in)        - convert ticks to VGM samples
    3. MD arranger   (built-in)        - map MIDI channels to FM/PSG channels
    4. YM2612/PSG    (ym2612.py/psg.py) - generate chip register writes
    5. VGM writer    (vgm.py)          - serialize to VGM file

Mega Drive channel allocation:
    FM channels 0-4 : melodic MIDI channels (first 5 non-drum channels)
    PSG channel 3   : MIDI drum channel (channel 9) as noise

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
from .fm_patches import get_patch

# MIDI drum channel (General MIDI percussion).
DRUM_CHANNEL = 9
MAX_FM_CHANNELS = 5    # Use FM 0-4 for melodic; channel 5 reserved for PCM/DAC.

# Carrier slots per algorithm (for volume scaling).
CARRIER_SLOTS = {
    0: [3], 1: [3], 2: [3], 3: [3],
    4: [1, 3], 5: [1, 2, 3], 6: [1, 3], 7: [0, 1, 2, 3],
}

# Minimum note duration in VGM samples (avoid sub-frame notes being lost).
MIN_NOTE_SAMPLES = NTSC_FRAME_SAMPLES  # 735 samples = 1 frame


class ChannelState:
    """Tracks the state of a single MIDI channel during conversion."""

    def __init__(self):
        self.program = 0            # current GM program number
        self.volume = 100           # CC7 channel volume (0-127)
        self.pan = 64               # CC10 pan (0-127, 64=center)
        self.pitch_bend = 8192      # pitch bend value (0-16383, 8192=center)
        self.patch_loaded = False   # whether FM patch has been written
        self.current_note = -1      # MIDI note currently playing (-1=none)
        self.patch = None           # cached FM patch dict


def _pan_to_stereo(pan):
    """Convert MIDI pan (0-127) to YM2612 stereo bits."""
    if pan < 43:
        return 0x80                 # Left only
    if pan > 84:
        return 0x40                 # Right only
    return 0xC0                     # Center (L+R)


def _volume_to_tl_adjust(volume, velocity):
    """Compute TL adjustment for carrier operators from CC7 volume + velocity.

    Returns a value to ADD to the base TL (positive = quieter).
    """
    effective = (velocity * volume) // 127
    if effective <= 0:
        return 80                   # effectively silent
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
    """Process a note_off event on the drum channel (silence PSG)."""

def convert_midi_to_vgm(midi_data, ntsc=True):
    """Convert MIDI byte data to VGM byte data for Mega Drive."""
    midi = parse_midi(midi_data)
    writer = VgmWriter(ntsc=ntsc)

    # -- Stage 3: Channel mapping --
    midi_ch_to_fm = {}
    fm_order = []
    states = {}

    for ev in midi.events:
        if ev.channel is None or ev.channel in states:
            continue
        states[ev.channel] = ChannelState()
        if ev.channel != DRUM_CHANNEL and ev.channel not in midi_ch_to_fm:
            if len(fm_order) < MAX_FM_CHANNELS:
                midi_ch_to_fm[ev.channel] = len(fm_order)
                fm_order.append(ev.channel)

    # -- Process events --
    for ev in midi.events:
        time = midi.tick_to_sample(ev.tick)
        state = states.get(ev.channel)

        if ev.kind == "program_change" and state:
            state.program = ev.data["program"]
            state.patch_loaded = False

        elif ev.kind == "control_change" and state:
            ctrl, val = ev.data["controller"], ev.data["value"]
            if ctrl == 7:
                state.volume = val
            elif ctrl == 10:
                state.pan = val
                if ev.channel in midi_ch_to_fm and state.patch_loaded:
                    fm = midi_ch_to_fm[ev.channel]
                    writer.add_command(
                        time, write_stereo(fm, _pan_to_stereo(val))[0])

        elif ev.kind == "pitch_bend" and state:
            state.pitch_bend = ev.data["value"]

        elif ev.kind == "note_on":
            note, vel = ev.data["note"], ev.data["velocity"]
            if ev.channel == DRUM_CHANNEL:
                _handle_drum_on(writer, time, note, vel)
            elif ev.channel in midi_ch_to_fm:
                _handle_fm_on(writer, time, note, vel,
                              midi_ch_to_fm[ev.channel], state)

        elif ev.kind == "note_off":
            note = ev.data["note"]
            if ev.channel == DRUM_CHANNEL:
                _handle_drum_off(writer, time, note)
            elif ev.channel in midi_ch_to_fm:
                _handle_fm_off(writer, time, note,
                               midi_ch_to_fm[ev.channel], state)

    return writer.build()

def _handle_fm_on(writer, time, note, velocity, fm_chan, state):
    """Process a note_on event on an FM channel."""
    if state.current_note >= 0:
        writer.add_commands(time, write_key_off(fm_chan))

    if not state.patch_loaded:
        state.patch = get_patch(state.program)
        writer.add_commands(time, write_instrument(fm_chan, state.patch))
        writer.add_command(
            time, write_stereo(fm_chan, _pan_to_stereo(state.pan))[0])
        state.patch_loaded = True

    bend = (state.pitch_bend - 8192) / 8192.0 * 2.0
    adjusted = note + bend
    fnum, block = note_to_fnum(int(round(adjusted)))
    writer.add_commands(time, write_frequency(fm_chan, fnum, block))

    tl_adj = _volume_to_tl_adjust(state.volume, velocity)
    algo = state.patch["algorithm"]
    for slot in CARRIER_SLOTS.get(algo, [3]):
        base_tl = state.patch["operators"][slot]["tl"]
        new_tl = max(0, min(127, base_tl + tl_adj))
        writer.add_commands(time, write_total_level(fm_chan, slot, new_tl))

    writer.add_commands(time, write_key_on(fm_chan))
    state.current_note = note


def _handle_fm_off(writer, time, note, fm_chan, state):
    """Process a note_off event on an FM channel."""
    if state.current_note == note:
        writer.add_commands(time, write_key_off(fm_chan))


# -- File-level convenience and CLI ------------------------------------------ #

def convert_file(midi_path, vgm_path, ntsc=True):
    """Convert a .mid file to a .vgm file. Returns output size in bytes."""
    with open(midi_path, "rb") as f:
        midi_data = f.read()
    vgm_data = convert_midi_to_vgm(midi_data, ntsc=ntsc)
    with open(vgm_path, "wb") as f:
        f.write(vgm_data)
    return len(vgm_data)

