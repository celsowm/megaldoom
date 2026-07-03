#!/usr/bin/env python3
"""
ym2612.py - YM2612 FM synthesizer driver for Mega Drive VGM generation.

The YM2612 has 6 FM channels.  Channels 0-2 are on port 0, channels 3-5 on
port 1.  Each channel has 4 operators (slots) whose parameters share a common
register layout: register = base + (slot * 4) + channel_in_port.

Key register summary:
    0x28        Key ON/OFF (port 0 only). Value = 0xF0+chan (on), 0x00+chan (off)
    0x30-0x3F   DT/MUL  (detune + frequency multiple) per operator
    0x40-0x4F   TL      (total level / output attenuation) per operator
    0x50-0x5F   RS/AR   (rate scaling + attack rate) per operator
    0x60-0x6F   AM/D1R  (amp modulation + first decay rate) per operator
    0x70-0x7F   D2R     (secondary decay rate) per operator
    0x80-0x8F   D1L/RR  (sustain level + release rate) per operator
    0x90-0x9F   SSG-EG  per operator
    0xA0-0xA2   FNUM low  (channels 0-2 on their port)
    0xA4-0xA6   FNUM high + block (channels 0-2 on their port)
    0xB0-0xB2   Feedback / Algorithm (channels 0-2 on their port)
    0xB4-0xB6   Stereo L/R + AMS/PMS (channels 0-2 on their port)

Frequency formula:
    FNUM = (f * 2^(19 + block)) / YM2612_clock
    Choose block so that 1 <= FNUM <= 2047 (maximise FNUM for resolution).
"""

import math

from .vgm import cmd_ym2612, YM2612_CLOCK_NTSC

YM2612_CLOCK = YM2612_CLOCK_NTSC   # assume NTSC

# Operator slot indices: slot 0=OP1, 1=OP2, 2=OP3, 3=OP4
# Register address = base + (slot * 4) + channel_in_port (0-2)
NUM_FM_CHANNELS = 6
NUM_OPERATORS = 4


# -- Register address helpers ------------------------------------------------ #

def _port_and_chan(fm_channel):
    """Return (port, channel_in_port) for a given FM channel (0-5)."""
    if fm_channel < 3:
        return 0, fm_channel
    return 1, fm_channel - 3


def _key_chan_code(fm_channel):
    """Channel code for register 0x28: 0,1,2 for ch 0-2; 4,5,6 for ch 3-5."""
    return fm_channel if fm_channel < 3 else fm_channel + 1


def _op_reg(base, fm_channel, slot):
    """Register address for an operator parameter."""
    port, chan = _port_and_chan(fm_channel)
    return base + (slot * 4) + chan


# -- Frequency calculation --------------------------------------------------- #

def note_to_fnum(midi_note, clock=YM2612_CLOCK):
    """
    Convert a MIDI note number to (FNUM, block) for the YM2612.

    Returns (fnum, block) where fnum is 0-2047 and block is 0-7.
    The block (octave) is chosen to maximise FNUM within the 11-bit range.
    """
    # MIDI note to frequency: f = 440 * 2^((note - 69) / 12)
    freq = 440.0 * (2.0 ** ((midi_note - 69) / 12.0))
    return freq_to_fnum(freq, clock)


def freq_to_fnum(freq, clock=YM2612_CLOCK):
    """Convert a frequency in Hz to (FNUM, block) for the YM2612."""
    if freq <= 0:
        return 0, 0
    # FNUM = freq * 2^(19 + block) / clock
    # We want the largest block where FNUM <= 2047.
    # block_max = log2(2047 * clock / freq) - 19
    block = int(math.floor(math.log2(2047.0 * clock / freq) - 19))
    if block < 0:
        block = 0
    if block > 7:
        block = 7
    fnum = int(round(freq * (2 ** (19 + block)) / clock))
    if fnum > 2047:
        block += 1
        if block > 7:
            block = 7
        fnum = int(round(freq * (2 ** (19 + block)) / clock))
        if fnum > 2047:
            fnum = 2047
    if fnum < 0:
        fnum = 0
    return fnum, block


# -- Register write generation ------------------------------------------------ #

def write_instrument(fm_channel, patch):
    """
    Generate VGM commands to load an FM instrument patch onto a channel.

    A patch is a dict with keys: 'algorithm', 'feedback', 'operators'.
    operators is a list of 4 dicts with keys:
        mul, dt, tl, ar, dr (d1r), sr (d2r), sl (d1l), rr, am, rs, ssg
    """
    cmds = []
    port, chan = _port_and_chan(fm_channel)

    # Feedback / Algorithm: reg 0xB0 + chan
    fb_algo = ((patch.get("feedback", 0) & 0x07) << 3) | (patch.get("algorithm", 0) & 0x07)
    cmds.append(cmd_ym2612(port, 0xB0 + chan, fb_algo))

    # Stereo: reg 0xB4 + chan — enable both L and R by default (0xC0).
    # Pan can be overridden by the arranger; default is centred (L+R).
    stereo = patch.get("stereo", 0xC0)
    cmds.append(cmd_ym2612(port, 0xB4 + chan, stereo & 0xFF))

    # Per-operator registers.
    for slot in range(NUM_OPERATORS):
        op = patch["operators"][slot]
        ch_off = (slot * 4) + chan

        # 0x30: DT/MUL  (detune 3 bits | multiple 4 bits)
        dt = op.get("dt", 0) & 0x07
        mul = op.get("mul", 1) & 0x0F
        cmds.append(cmd_ym2612(port, 0x30 + ch_off, (dt << 4) | mul))

        # 0x40: TL (total level, 7 bits)
        tl = op.get("tl", 0) & 0x7F
        cmds.append(cmd_ym2612(port, 0x40 + ch_off, tl))

        # 0x50: RS/AR (rate scaling 2 bits | attack rate 5 bits)
        rs = op.get("rs", 0) & 0x03
        ar = op.get("ar", 31) & 0x1F
        cmds.append(cmd_ym2612(port, 0x50 + ch_off, (rs << 6) | ar))

        # 0x60: AM/D1R (am enable 1 bit | first decay rate 5 bits)
        am = op.get("am", 0) & 0x01
        d1r = op.get("dr", 0) & 0x1F
        cmds.append(cmd_ym2612(port, 0x60 + ch_off, (am << 7) | d1r))

        # 0x70: D2R (secondary decay / sustain rate, 5 bits)
        d2r = op.get("sr", 0) & 0x1F
        cmds.append(cmd_ym2612(port, 0x70 + ch_off, d2r))

        # 0x80: D1L/RR (sustain level 4 bits | release rate 4 bits)
        d1l = op.get("sl", 0) & 0x0F
        rr = op.get("rr", 15) & 0x0F
        cmds.append(cmd_ym2612(port, 0x80 + ch_off, (d1l << 4) | rr))

        # 0x90: SSG-EG (4 bits, 0 = off)
        ssg = op.get("ssg", 0) & 0x0F
        cmds.append(cmd_ym2612(port, 0x90 + ch_off, ssg))

    return cmds


def write_frequency(fm_channel, fnum, block):
    """Generate VGM commands to set channel frequency (FNUM + block).

    The high register (0xA4+ch) MUST be written before the low (0xA0+ch).
    """
    port, chan = _port_and_chan(fm_channel)
    high = ((block & 0x07) << 3) | ((fnum >> 8) & 0x07)
    low = fnum & 0xFF
    # High byte first, then low byte — YM2612 latches on low write.
    return [cmd_ym2612(port, 0xA4 + chan, high),
            cmd_ym2612(port, 0xA0 + chan, low)]


def write_key_on(fm_channel):
    """Key ON: reg 0x28, value 0xF0 + channel_code (all 4 operators)."""
    code = _key_chan_code(fm_channel)
    return [cmd_ym2612(0, 0x28, 0xF0 | code)]


def write_key_off(fm_channel):
    """Key OFF: reg 0x28, value 0x00 + channel_code."""
    code = _key_chan_code(fm_channel)
    return [cmd_ym2612(0, 0x28, 0x00 | code)]


def write_total_level(fm_channel, slot, tl):
    """Update a single operator's total level (volume adjustment)."""
    port, chan = _port_and_chan(fm_channel)
    reg = 0x40 + (slot * 4) + chan
    return [cmd_ym2612(port, reg, tl & 0x7F)]


def write_stereo(fm_channel, stereo):
    """Update stereo panning: 0xC0 = L+R, 0x80 = L only, 0x40 = R only."""
    port, chan = _port_and_chan(fm_channel)
    return [cmd_ym2612(port, 0xB4 + chan, stereo & 0xFF)]

