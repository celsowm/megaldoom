#!/usr/bin/env python3
"""
psg.py - SN76489 PSG (Programmable Sound Generator) driver for Mega Drive VGM.

The Mega Drive's SN76489 has 4 channels:
    Channels 0-2: square wave tone (10-bit frequency, 4-bit volume)
    Channel 3:    noise generator (3-bit type/freq, 4-bit volume)

The PSG uses a serial command protocol over a single data port.  Each write
is one byte:
    First byte (bit 7 = 1): 0x80 | (channel << 5) | (type << 4) | low_nibble
        type 0 = tone/freq, type 1 = volume
    Second byte (bit 7 = 0): high 6 bits of tone frequency (tone channels only)

Volume is inverted: 0 = loudest, 15 = silent.
Frequency: f_out = clock / (32 * (n + 1)), clock = 3579545 Hz.
"""

import math

from .vgm import cmd_psg, SN76489_CLOCK

PSG_CLOCK = SN76489_CLOCK   # 3579545 Hz

NUM_PSG_CHANNELS = 4        # 0-2 tone, 3 noise


def note_to_psg_freq(midi_note, clock=PSG_CLOCK):
    """
    Convert a MIDI note to a PSG frequency value (10-bit, 0-1023).

    PSG frequency: f_out = clock / (32 * (n + 1))
    So: n = (clock / (32 * f)) - 1
    Returns the 10-bit value n, or 1023 (lowest pitch) if out of range.
    """
    freq = 440.0 * (2.0 ** ((midi_note - 69) / 12.0))
    if freq <= 0:
        return 1023
    n = (clock / (32.0 * freq)) - 1.0
    n = int(round(n))
    if n < 0:
        n = 0
    if n > 1023:
        n = 1023
    return n


def velocity_to_psg_volume(velocity):
    """Convert MIDI velocity (0-127) to PSG attenuation (0=loud, 15=silent)."""
    if velocity <= 0:
        return 15   # silent
    # Map 1-127 to 0-14 (leave 15 for true silence).
    vol = 15 - int(round(velocity * 15.0 / 127.0))
    if vol < 0:
        vol = 0
    if vol > 15:
        vol = 15
    return vol


def psg_set_freq(channel, freq_value):
    """Generate VGM commands to set a PSG tone channel frequency (10-bit)."""
    low = freq_value & 0x0F
    high = (freq_value >> 4) & 0x3F
    # First byte: 0x80 | (ch << 5) | 0 (tone type) | low nibble
    # Second byte: 0x00 | high 6 bits
    first = 0x80 | (channel << 5) | low
    second = high & 0x3F
    return [cmd_psg(first), cmd_psg(second)]


def psg_set_volume(channel, volume):
    """Generate VGM command to set a PSG channel volume (0-15)."""
    # 0x80 | (ch << 5) | 0x10 (volume type) | volume
    val = 0x90 | (channel << 5) | (volume & 0x0F)
    return [cmd_psg(val)]


def psg_set_noise(noise_type):
    """
    Set the noise channel (3) type/frequency.
    noise_type bits: bit 2 = white(1)/periodic(0), bits 0-1 = freq mode (0-3).
    """
    # 0x80 | (3 << 5) | 0 (tone type) | (noise_type & 0x07)
    val = 0x80 | (3 << 5) | (noise_type & 0x07)
    return [cmd_psg(val)]
