#!/usr/bin/env python3
"""
vgm.py - VGM (Video Game Music) file format writer for Mega Drive.

Produces VGM files that SGDK's xgm2tool can convert to XGM2 music blobs.

VGM is a logged-command format: it records a stream of sound chip register
writes with timing information (in 44100 Hz "samples").  The xgm2tool reads
the data-offset field at file position 0x34 (relative to 0x34) to locate the
command stream, and converts register writes + waits into compact XGM2 data.

VGM command reference (subset relevant to Mega Drive):
    0x50 dd           SN76489 / PSG write (1 data byte)
    0x52 aa dd        YM2612 port 0 register write (reg, value)
    0x53 aa dd        YM2612 port 1 register write (reg, value)
    0x61 lo hi        Wait N samples (16-bit LE, 0-65535)
    0x62              Wait 735 samples  (1 NTSC frame at 60 Hz)
    0x70..0x7F        Wait (cmd & 0x0F)+1 samples  (1-16 samples)
    0x66              End of sound data

Mega Drive clocks:
    YM2612:  7670454 Hz (NTSC) / 7600489 Hz (PAL)
    SN76489: 3579545 Hz
    VGM sample rate: 44100 Hz
"""

import struct

VGM_SAMPLE_RATE = 44100
NTSC_FRAME_SAMPLES = 735       # 44100 / 60
PAL_FRAME_SAMPLES = 882        # 44100 / 50

YM2612_CLOCK_NTSC = 7670454
YM2612_CLOCK_PAL = 7600489
SN76489_CLOCK = 3579545

# VGM version 1.50; header = 64 bytes; data at 0x40.
VGM_VERSION = 0x00000150
VGM_HEADER_SIZE = 64
VGM_DATA_OFFSET_VALUE = 0x0C   # relative to field at 0x34 -> data at 0x40

CMD_PSG_WRITE = 0x50
CMD_YM2612_PORT0 = 0x52
CMD_YM2612_PORT1 = 0x53
CMD_WAIT_SAMPLES = 0x61
CMD_WAIT_NTSC_FRAME = 0x62
CMD_END = 0x66

SN76489_FEEDBACK = 0x0009
SN76489_SR_WIDTH = 0x0010


# -- Command builders ------------------------------------------------------- #

def cmd_psg(data):
    """SN76489 PSG write: 0x50 dd."""
    return bytes([CMD_PSG_WRITE, data & 0xFF])


def cmd_ym2612(port, reg, value):
    """YM2612 register write: 0x52/0x53 aa dd."""
    cmd = CMD_YM2612_PORT0 if port == 0 else CMD_YM2612_PORT1
    return bytes([cmd, reg & 0xFF, value & 0xFF])


def cmd_wait_n(n):
    """Wait N samples. Uses 0x70-0x7F for 1-16, 0x62 for 735, 0x61 otherwise."""
    if n <= 0:
        return b""
    if n <= 16:
        return bytes([0x70 | (n - 1)])
    if n == NTSC_FRAME_SAMPLES:
        return bytes([CMD_WAIT_NTSC_FRAME])
    if n <= 65535:
        return bytes([CMD_WAIT_SAMPLES, n & 0xFF, (n >> 8) & 0xFF])
    # Split long waits into multiple 0x61 commands.
    result = bytearray()
    remaining = n
    while remaining > 0:
        chunk = min(remaining, 65535)
        result += cmd_wait_n(chunk)
        remaining -= chunk
    return bytes(result)


def cmd_end():
    """End of sound data: 0x66."""
    return bytes([CMD_END])


# -- VGM Writer -------------------------------------------------------------- #

class VgmWriter:
    """
    Accumulates VGM commands with timing and serializes a complete VGM file.

    Commands are added with an associated VGM sample time (44100 Hz clock).
    The writer emits wait commands between consecutive command groups to
    advance the playback position, then writes the full file with a proper
    header at the end.
    """

    def __init__(self, ntsc=True):
        self.ntsc = ntsc
        self._events = []       # list of (time_samples, command_bytes)
        self._total_samples = 0

    def add_command(self, time_samples, command_bytes):
        """Add a VGM command at the given sample time (44100 Hz)."""
        if not command_bytes:
            return
        self._events.append((time_samples, command_bytes))
        if time_samples > self._total_samples:
            self._total_samples = time_samples

    def add_commands(self, time_samples, commands_list):
        """Add multiple commands at the same sample time."""
        for cmd in commands_list:
            self.add_command(time_samples, cmd)

    def build(self, loop_offset_samples=None, loop_length_samples=None):
        """
        Build the complete VGM byte string.

        Args:
            loop_offset_samples: sample position to loop back to (None=no loop).
            loop_length_samples: loop length in samples (for header field).
        """
        # Stable sort by time preserves insertion order for simultaneous events.
        events = sorted(self._events, key=lambda e: e[0])

        # Build the data stream: commands with wait deltas between them.
        data = bytearray()
        current_time = 0
        for time_samples, cmd_bytes in events:
            delta = time_samples - current_time
            if delta > 0:
                data += cmd_wait_n(delta)
                current_time = time_samples
            data += cmd_bytes
        data += cmd_end()

        total_samples = self._total_samples

        # Loop offset (relative to field at 0x1C).
        loop_offset_field = 0
        loop_samples_field = 0
        if loop_offset_samples is not None and loop_length_samples is not None:
            loop_byte_off = self._time_to_byte_offset(events, loop_offset_samples)
            if loop_byte_off is not None:
                loop_offset_field = (VGM_HEADER_SIZE + loop_byte_off) - 0x1C
            loop_samples_field = loop_length_samples

        # Build the 64-byte header.
        header = bytearray(VGM_HEADER_SIZE)
        header[0:4] = b"Vgm "
        total_file_size = VGM_HEADER_SIZE + len(data)
        struct.pack_into("<I", header, 0x04, total_file_size - 4)  # eof
        struct.pack_into("<I", header, 0x08, VGM_VERSION)
        struct.pack_into("<I", header, 0x0C, SN76489_CLOCK)
        struct.pack_into("<I", header, 0x18, total_samples)
        struct.pack_into("<I", header, 0x1C, loop_offset_field)
        struct.pack_into("<I", header, 0x20, loop_samples_field)
        rate = 50 if not self.ntsc else 60
        struct.pack_into("<I", header, 0x24, rate)
        struct.pack_into("<H", header, 0x28, SN76489_FEEDBACK)
        struct.pack_into("<H", header, 0x2A, SN76489_SR_WIDTH)
        ym_clock = YM2612_CLOCK_NTSC if self.ntsc else YM2612_CLOCK_PAL
        struct.pack_into("<I", header, 0x30, ym_clock)
        # 0x34: data offset (relative to 0x34) — where xgm2tool reads it.
        struct.pack_into("<I", header, 0x34, VGM_DATA_OFFSET_VALUE)
        # 0x38: standard VGM data offset (relative to 0x38) for spec compliance.
        struct.pack_into("<I", header, 0x38, 0x08)

        return bytes(header) + bytes(data)

    @property
    def total_samples(self):
        return self._total_samples

    @staticmethod
    def _time_to_byte_offset(events, target_time):
        """Find the byte offset in the data stream for a given sample time."""
        offset = 0
        current_time = 0
        for time_samples, cmd_bytes in events:
            delta = time_samples - current_time
            if delta > 0:
                offset += len(cmd_wait_n(delta))
                current_time = time_samples
            if time_samples >= target_time:
                return offset
            offset += len(cmd_bytes)
        return None


def write_vgm(path, vgm_bytes):
    """Write VGM bytes to a file."""
    with open(path, "wb") as f:
        f.write(vgm_bytes)
