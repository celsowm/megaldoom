#!/usr/bin/env python3
"""
fm_patches.py - YM2612 FM instrument patch library.

Each patch is a dict:
    algorithm : 0-7  (operator routing)
    feedback  : 0-7  (OP1 self-modulation)
    stereo    : 0xC0 (L+R), 0x80 (L), 0x40 (R)
    operators : [op0, op1, op2, op3]  (slot order: 0=OP1,1=OP2,2=OP3,3=OP4)

Each operator dict:
    mul  : 0-15  frequency multiple
    dt   : 0-7   detune (0-3 positive, 4-7 negative)
    tl   : 0-127 total level (0=loudest, 127=silent)
    ar   : 0-31  attack rate
    dr   : 0-31  decay rate (D1R)
    sr   : 0-31  sustain rate (D2R)
    sl   : 0-15  sustain level (D1L)
    rr   : 0-15  release rate
    am   : 0-1   amplitude modulation enable
    rs   : 0-3   rate scaling
    ssg  : 0-15  SSG-EG (0=off)
"""


def _op(mul=1, dt=0, tl=0, ar=31, dr=0, sr=0, sl=0, rr=15, am=0, rs=0, ssg=0):
    return {"mul": mul, "dt": dt, "tl": tl, "ar": ar, "dr": dr,
            "sr": sr, "sl": sl, "rr": rr, "am": am, "rs": rs, "ssg": ssg}


def _patch(algo, fb, ops, stereo=0xC0):
    return {"algorithm": algo, "feedback": fb,
            "stereo": stereo, "operators": ops}


# Default fallback instrument (warm synth pad, algorithm 7 = all parallel).
DEFAULT_PATCH = _patch(7, 4, [
    _op(mul=1, tl=22, ar=31, dr=10, sr=5, sl=5, rr=8, rs=1),
    _op(mul=1, tl=22, ar=31, dr=10, sr=5, sl=5, rr=8, rs=1),
    _op(mul=1, tl=22, ar=31, dr=10, sr=5, sl=5, rr=8, rs=1),
    _op(mul=1, tl=10, ar=31, dr=10, sr=5, sl=5, rr=8, rs=1),
])

# Acoustic Grand Piano (algo 4: OP1->OP2, OP3->OP4).
PIANO = _patch(4, 5, [
    _op(mul=1, dt=2, tl=18, ar=31, dr=10, sr=5, sl=12, rr=8, rs=2),
    _op(mul=4, dt=0, tl=10, ar=31, dr=8, sr=5, sl=8, rr=8, rs=1),
    _op(mul=1, dt=2, tl=20, ar=31, dr=10, sr=5, sl=12, rr=8, rs=2),
    _op(mul=4, dt=0, tl=8, ar=31, dr=8, sr=5, sl=8, rr=8, rs=1),
])

# Electric Bass (pick) (algo 4, punchy attack).
ELECTRIC_BASS = _patch(4, 0, [
    _op(mul=1, tl=20, ar=31, dr=8, sr=3, sl=2, rr=8),
    _op(mul=2, tl=14, ar=31, dr=8, sr=3, sl=2, rr=8),
    _op(mul=1, tl=24, ar=31, dr=8, sr=3, sl=2, rr=8),
    _op(mul=1, tl=10, ar=31, dr=8, sr=3, sl=2, rr=8),
])

# Overdriven Guitar (algo 0: series OP1->OP2->OP3->OP4).
OVERDRIVEN_GUITAR = _patch(0, 0, [
    _op(mul=1, tl=24, ar=31, dr=7, sr=5, sl=1, rr=7),
    _op(mul=1, tl=20, ar=31, dr=7, sr=5, sl=1, rr=7),
    _op(mul=1, tl=18, ar=31, dr=7, sr=5, sl=1, rr=7),
    _op(mul=1, tl=14, ar=31, dr=7, sr=5, sl=1, rr=7),
])

# Distortion Guitar (algo 0, more harmonics via feedback).
DISTORTION_GUITAR = _patch(0, 2, [
    _op(mul=1, tl=28, ar=31, dr=6, sr=4, sl=1, rr=6),
    _op(mul=2, tl=20, ar=31, dr=6, sr=4, sl=1, rr=6),
    _op(mul=1, tl=22, ar=31, dr=6, sr=4, sl=1, rr=6),
    _op(mul=1, tl=12, ar=31, dr=6, sr=4, sl=1, rr=6),
])

# Acoustic Guitar (nylon) (algo 3: (OP1+OP2)->OP3->OP4).
NYLON_GUITAR = _patch(3, 5, [
    _op(mul=1, tl=24, ar=31, dr=12, sr=5, sl=8, rr=10),
    _op(mul=2, tl=18, ar=31, dr=12, sr=5, sl=8, rr=10),
    _op(mul=1, tl=20, ar=31, dr=10, sr=5, sl=6, rr=10),
    _op(mul=1, tl=8, ar=31, dr=10, sr=5, sl=6, rr=10),
])

# String Ensemble (algo 2: OP1->(OP2+OP3)->OP4).
STRINGS = _patch(2, 4, [
    _op(mul=1, dt=1, tl=20, ar=20, dr=10, sr=5, sl=10, rr=8),
    _op(mul=1, dt=0, tl=16, ar=20, dr=10, sr=5, sl=10, rr=8),
    _op(mul=2, dt=0, tl=18, ar=20, dr=10, sr=5, sl=10, rr=8),
    _op(mul=1, dt=0, tl=8, ar=20, dr=10, sr=5, sl=10, rr=8),
])

# Flute (algo 5: OP1 modulates OP2,OP3,OP4).
FLUTE = _patch(5, 3, [
    _op(mul=1, tl=24, ar=28, dr=8, sr=3, sl=4, rr=10),
    _op(mul=1, tl=8, ar=28, dr=8, sr=3, sl=4, rr=10),
    _op(mul=1, tl=16, ar=28, dr=8, sr=3, sl=4, rr=10),
    _op(mul=1, tl=6, ar=28, dr=8, sr=3, sl=4, rr=10),
])

# Trumpet (algo 0, bright brass).
TRUMPET = _patch(0, 0, [
    _op(mul=1, tl=22, ar=28, dr=8, sr=6, sl=6, rr=8),
    _op(mul=2, tl=18, ar=28, dr=8, sr=6, sl=6, rr=8),
    _op(mul=1, tl=16, ar=28, dr=8, sr=6, sl=6, rr=8),
    _op(mul=1, tl=8, ar=28, dr=8, sr=6, sl=6, rr=8),
])

# Synth Pad (algo 7, soft sustained).
SYNTH_PAD = _patch(7, 4, [
    _op(mul=2, tl=24, ar=15, dr=10, sr=5, sl=8, rr=6),
    _op(mul=1, tl=24, ar=15, dr=10, sr=5, sl=8, rr=6),
    _op(mul=2, tl=22, ar=15, dr=10, sr=5, sl=8, rr=6),
    _op(mul=1, tl=12, ar=15, dr=10, sr=5, sl=8, rr=6),
])


# -- GM program number to YM2612 patch mapping ------------------------------- #

# Maps General MIDI program numbers (0-127) to YM2612 patches.
# Programs not in this table fall back to DEFAULT_PATCH.
PROGRAM_MAP = {
    0: PIANO,                         # Acoustic Grand Piano
    1: PIANO,                         # Bright Acoustic Piano
    2: PIANO,                         # Electric Grand Piano
    3: PIANO,                         # Honky-tonk Piano
    4: PIANO,                         # Electric Piano 1
    5: PIANO,                         # Electric Piano 2
    6: PIANO,                         # Harpsichord
    7: PIANO,                         # Clavinet
    24: NYLON_GUITAR,                 # Acoustic Guitar (nylon)
    25: NYLON_GUITAR,                 # Acoustic Guitar (steel)
    26: NYLON_GUITAR,                 # Electric Guitar (jazz)
    27: OVERDRIVEN_GUITAR,            # Electric Guitar (clean)
    28: OVERDRIVEN_GUITAR,            # Electric Guitar (muted)
    29: OVERDRIVEN_GUITAR,            # Overdriven Guitar
    30: DISTORTION_GUITAR,            # Distortion Guitar
    31: DISTORTION_GUITAR,            # Guitar harmonics
    32: ELECTRIC_BASS,                # Acoustic Bass
    33: ELECTRIC_BASS,                # Electric Bass (finger)
    34: ELECTRIC_BASS,                # Electric Bass (pick)
    35: ELECTRIC_BASS,                # Fretless Bass
    36: ELECTRIC_BASS,                # Slap Bass 1
    37: ELECTRIC_BASS,                # Slap Bass 2
    38: ELECTRIC_BASS,                # Synth Bass 1
    39: ELECTRIC_BASS,                # Synth Bass 2
    48: STRINGS,                      # String Ensemble 1
    49: STRINGS,                      # String Ensemble 2
    50: STRINGS,                      # Synth Strings 1
    51: STRINGS,                      # Synth Strings 2
    56: TRUMPET,                      # Trumpet
    57: TRUMPET,                      # Trombone
    58: TRUMPET,                      # Tuba
    59: TRUMPET,                      # Muted Trumpet
    60: TRUMPET,                      # French Horn
    61: TRUMPET,                      # Brass Section
    62: TRUMPET,                      # Synth Brass 1
    63: TRUMPET,                      # Synth Brass 2
    73: FLUTE,                        # Flute
    74: FLUTE,                        # Recorder
    75: FLUTE,                        # Pan Flute
    76: FLUTE,                        # Blown Bottle
    77: FLUTE,                        # Shakuhachi
    78: FLUTE,                        # Whistle
    79: FLUTE,                        # Ocarina
    88: SYNTH_PAD,                    # New Age Pad
    89: SYNTH_PAD,                    # Warm Pad
    90: SYNTH_PAD,                    # Polysynth
    91: SYNTH_PAD,                    # Choir
    92: SYNTH_PAD,                    # Bowed Pad
    93: SYNTH_PAD,                    # Metallic Pad
    94: SYNTH_PAD,                    # Halo Pad
    95: SYNTH_PAD,                    # Sweep Pad
}


def get_patch(program):
    """Return the YM2612 patch for a GM program number, or DEFAULT_PATCH."""
    return PROGRAM_MAP.get(program, DEFAULT_PATCH)