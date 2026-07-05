#!/usr/bin/env python3
"""
genmidi.py - Doom GENMIDI (OPL2) instrument bank -> YM2612 patch translation.

Doom's GENMIDI lump holds 175 OPL2 (Yamaha YM3812) FM voices: 128 General MIDI
melodic instruments (indices 0-127) plus 47 percussion instruments (index
128 + note - 35, for MIDI notes 35-81). These are the instruments the DOS Doom
plays through its OPL FM synth.

The Mega Drive's YM2612 is a close FM cousin of the OPL2, so we translate each
2-operator OPL voice into a 4-operator YM2612 patch in the schema consumed by
ym2612.write_instrument(): {algorithm, feedback, stereo, operators[4]}.

Translation strategy
--------------------
YM2612 algorithm 4 provides two independent 2-op stacks (S1->S2 and S3->S4,
output = S2 + S4). A single OPL voice maps onto the S1->S2 pair (S3/S4 silenced);
a Doom "double voice" instrument (flag bit 2) puts its second OPL voice on the
S3->S4 pair, slightly detuned for a chorus effect.

IMPORTANT: on the YM2612 the four operators are laid out in register-offset order
S1, S3, S2, S4 (offset index 0, 1, 2, 3). ym2612.write_instrument() addresses
operators by that offset index, so operators[] here is ordered [S1, S3, S2, S4].
For algorithm 4 the carriers (S2, S4) are therefore offset indices 2 and 3.

The OPL2 has selectable operator waveforms; the YM2612 has sine only, so the
waveform field is dropped. That is the main unavoidable timbral difference; most
Doom voices use waveform 0 (sine) anyway.
"""

import os
import struct

GENMIDI_MAGIC = b"#OPL_II#"

GENMIDI_FLAG_FIXED = 0x01     # fixed pitch (percussion)
GENMIDI_FLAG_DOUBLE = 0x04    # two-voice instrument

# Operator offset indices into a YM2612 operators[] list (S1, S3, S2, S4 order).
S1, S3, S2, S4 = 0, 1, 2, 3

# YM2612 algorithm 4: two independent 2-op stacks. Carriers are S2 and S4.
ALGORITHM_DUAL_2OP = 4
CARRIER_OFFSETS = (S2, S4)

# YM2612 algorithm 7: all four operators in parallel (every operator is a
# carrier). Used for OPL "additive" (connection=1) voices where the two
# operators sound side by side instead of one modulating the other.
ALGORITHM_ALL_CARRIERS = 7

# Default patch (used when a GENMIDI voice cannot be translated).
DEFAULT_OP = {"mul": 1, "dt": 0, "tl": 32, "ar": 31, "dr": 0, "sr": 0,
              "sl": 0, "rr": 8, "am": 0, "rs": 0, "ssg": 0}


# --------------------------------------------------------------------------- #
# GENMIDI binary parsing
# --------------------------------------------------------------------------- #

class OplOp:
    """A single OPL2 operator (modulator or carrier)."""

    __slots__ = ("am", "vib", "eg_sustain", "ksr", "mult",
                 "ar", "dr", "sl", "rr", "waveform", "ksl", "level")

    def __init__(self, raw):
        tremolo, attack, sustain, waveform, scale, level = raw
        self.am = (tremolo >> 7) & 0x01          # amplitude modulation
        self.vib = (tremolo >> 6) & 0x01         # vibrato
        self.eg_sustain = (tremolo >> 5) & 0x01  # envelope sustain
        self.ksr = (tremolo >> 4) & 0x01         # key scale rate
        self.mult = tremolo & 0x0F               # frequency multiple
        self.ar = (attack >> 4) & 0x0F           # attack rate (4-bit)
        self.dr = attack & 0x0F                  # decay rate (4-bit)
        self.sl = (sustain >> 4) & 0x0F          # sustain level (4-bit)
        self.rr = sustain & 0x0F                 # release rate (4-bit)
        self.waveform = waveform & 0x07          # OPL waveform (dropped on YM2612)
        self.ksl = (scale >> 6) & 0x03           # key scale level
        self.level = level & 0x3F                # output level / attenuation (6-bit)


class OplVoice:
    """An OPL2 two-operator voice: modulator, carrier, feedback, tuning."""

    __slots__ = ("modulator", "carrier", "feedback", "connection",
                 "base_note_offset")

    def __init__(self, data):
        # 16 bytes: mod(6) feedback(1) carrier(6) unused(1) base_note_offset(s16)
        mod = struct.unpack_from("<6B", data, 0)
        fb_conn = data[6]
        car = struct.unpack_from("<6B", data, 7)
        (offset,) = struct.unpack_from("<h", data, 14)
        self.modulator = OplOp(mod)
        self.carrier = OplOp(car)
        self.feedback = (fb_conn >> 1) & 0x07    # feedback (3-bit)
        self.connection = fb_conn & 0x01         # 0=FM (mod->car), 1=additive
        self.base_note_offset = offset


class GenMidiInstrument:
    """A GENMIDI instrument: flags, fixed note, two voices."""

    __slots__ = ("flags", "fine_tuning", "fixed_note", "voice1", "voice2")

    def __init__(self, record):
        self.flags, self.fine_tuning, self.fixed_note = struct.unpack_from(
            "<HBB", record, 0)
        self.voice1 = OplVoice(record[4:20])
        self.voice2 = OplVoice(record[20:36])

    @property
    def fixed_pitch(self):
        return bool(self.flags & GENMIDI_FLAG_FIXED)

    @property
    def double_voice(self):
        return bool(self.flags & GENMIDI_FLAG_DOUBLE)


def parse_genmidi(data):
    """Parse a GENMIDI lump into a list of 175 GenMidiInstrument objects."""
    if data[:8] != GENMIDI_MAGIC:
        raise ValueError("Not a GENMIDI lump: magic=%r" % data[:8])
    count = (len(data) - 8) // (36 + 32)
    instruments = []
    base = 8
    for i in range(count):
        record = data[base + i * 36: base + (i + 1) * 36]
        instruments.append(GenMidiInstrument(record))
    return instruments


# --------------------------------------------------------------------------- #
# OPL2 -> YM2612 operator/voice translation
# --------------------------------------------------------------------------- #

def _widen_rate(rate):
    """Widen a 4-bit OPL rate (0-15) to a 5-bit YM2612 rate (0-31)."""
    if rate <= 0:
        return 0
    return min(31, rate * 2)


def _opl_op_to_ym(op, is_carrier):
    """Translate one OPL operator into a YM2612 operator dict.

    Level: the OPL2 and YM2612 total-level scales BOTH use 0.75 dB per step, so
    the carrier `level` maps 1:1 onto YM2612 TL (same audible attenuation in dB).

    The YM2612 applies FM modulation harder than the OPL2 for the same operator
    level, so a raw 1:1 modulator is too hot and turns harsh/strident, especially
    when the modulator's frequency multiple is high (those inject piercing high
    harmonics). We soften the modulator by a small offset that grows with its
    MULT. This sits between "dull near-sine" (over-attenuated) and "strident"
    (raw 1:1).
    """
    if is_carrier:
        tl = min(127, op.level)
    else:
        tl = min(120, op.level + 5 + 2 * op.mult)
    return {
        "mul": op.mult & 0x0F,
        "dt": 0,                          # OPL has no detune
        "tl": tl,
        "ar": _widen_rate(op.ar),
        "dr": _widen_rate(op.dr),
        # OPL EG-type bit: set = sustained (hold at sustain level, no 2nd decay);
        # clear = percussive (after decaying to SL, keep decaying at RR while the
        # key is held). Map the percussive second-decay to YM2612 D2R using RR.
        "sr": 0 if op.eg_sustain else _widen_rate(op.rr),
        "sl": op.sl & 0x0F,
        "rr": op.rr & 0x0F,
        "am": op.am & 0x01,
        # OPL KSR (key scale rate) is a single bit; approximate with a moderate
        # YM2612 rate-scaling value. OPL KSL (level scaling) has no YM2612 analog.
        "rs": 2 if op.ksr else 0,
        "ssg": 0,
    }


def _silent_op():
    """A fully attenuated YM2612 operator (contributes no sound)."""
    op = dict(DEFAULT_OP)
    op["tl"] = 127
    op["mul"] = 0
    return op


def opl_to_patch(instr, stereo=0xC0):
    """Translate a GenMidiInstrument into a YM2612 patch dict.

    operators[] is ordered [S1, S3, S2, S4] to match ym2612.write_instrument().

    voice1 occupies the S1/S2 pair, voice2 the S3/S4 pair (double-voice only).
    The OPL "connection" bit selects the routing:
      * connection 0 (FM):       modulator -> carrier  (algorithm 4 stack)
      * connection 1 (additive): both operators are carriers (algorithm 7)
    A voice1 additive connection forces algorithm 7 for the whole patch, so the
    second voice is then mapped as parallel carriers too.
    """
    v1 = instr.voice1
    operators = [None, None, None, None]

    if v1.connection == 0:
        algorithm = ALGORITHM_DUAL_2OP
        operators[S1] = _opl_op_to_ym(v1.modulator, is_carrier=False)
        operators[S2] = _opl_op_to_ym(v1.carrier, is_carrier=True)
    else:
        # Additive: both OPL operators sound in parallel (both carriers).
        algorithm = ALGORITHM_ALL_CARRIERS
        operators[S1] = _opl_op_to_ym(v1.modulator, is_carrier=True)
        operators[S2] = _opl_op_to_ym(v1.carrier, is_carrier=True)

    if instr.double_voice:
        v2 = instr.voice2
        # With algorithm 7 the S3/S4 pair is also parallel carriers; with
        # algorithm 4 it is a second FM stack (S3 modulates S4).
        mod_is_carrier = (algorithm == ALGORITHM_ALL_CARRIERS)
        operators[S3] = _opl_op_to_ym(v2.modulator, is_carrier=mod_is_carrier)
        operators[S4] = _opl_op_to_ym(v2.carrier, is_carrier=True)
        # Chorus detune of the second voice: DMX offsets its frequency index by
        # ((fine_tuning / 2) - 64)/32 semitones. We approximate the direction
        # with the YM2612 detune field (1..3 = sharp, 5..7 = flat).
        detune = (instr.fine_tuning // 2) - 64
        dt_val = 1 if detune >= 0 else 5
        operators[S3]["dt"] = dt_val
        operators[S4]["dt"] = dt_val
        feedback = v1.feedback
    else:
        operators[S3] = _silent_op()
        operators[S4] = _silent_op()
        feedback = v1.feedback

    return {
        "algorithm": algorithm,
        "feedback": feedback & 0x07,
        "stereo": stereo,
        "operators": operators,
        # Per-instrument transpose Doom adds to the note before computing pitch.
        "note_offset": v1.base_note_offset,
    }


# --------------------------------------------------------------------------- #
# Patch bank loader
# --------------------------------------------------------------------------- #

DEFAULT_OP2_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "genmidi.op2")

# Percussion instrument indices cover MIDI notes 35-81.
PERCUSSION_BASE_INDEX = 128
PERCUSSION_FIRST_NOTE = 35
PERCUSSION_LAST_NOTE = 81


class GenMidiBank:
    """Translated YM2612 patch bank backed by a parsed GENMIDI lump."""

    def __init__(self, instruments):
        self.instruments = instruments
        self._melodic_cache = {}
        self._perc_cache = {}

    def get_patch(self, program):
        """Return the YM2612 patch for a GM program number (0-127)."""
        program &= 0x7F
        patch = self._melodic_cache.get(program)
        if patch is None:
            patch = opl_to_patch(self.instruments[program])
            self._melodic_cache[program] = patch
        return patch

    def get_percussion(self, note):
        """Return the YM2612 patch for a percussion note (35-81), or None."""
        if note < PERCUSSION_FIRST_NOTE or note > PERCUSSION_LAST_NOTE:
            return None
        patch = self._perc_cache.get(note)
        if patch is None:
            idx = PERCUSSION_BASE_INDEX + (note - PERCUSSION_FIRST_NOTE)
            if idx >= len(self.instruments):
                return None
            patch = opl_to_patch(self.instruments[idx])
            self._perc_cache[note] = patch
        return patch


def load_bank(op2_path=DEFAULT_OP2_PATH):
    """Load and translate the GENMIDI bank. Returns a GenMidiBank, or None if
    the bundled data file is missing."""
    if not os.path.isfile(op2_path):
        return None
    with open(op2_path, "rb") as f:
        data = f.read()
    return GenMidiBank(parse_genmidi(data))
