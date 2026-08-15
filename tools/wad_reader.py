#!/usr/bin/env python3
"""Minimal Doom WAD directory reader (same layout as tools/wad-extract.py).

Knows nothing about map semantics or asset conversion -- just how to open a
WAD file, list its lumps, and slice a named lump's bytes out, including the
"find member lumps following a map marker" convention Doom map lumps use.
"""

import struct

WAD_HEADER_FMT = "<4sii"   # signature, numLumps, dirOffset
LUMP_ENTRY_FMT = "<ii8s"   # filepos, size, name
LUMP_ENTRY_SIZE = struct.calcsize(LUMP_ENTRY_FMT)


class Lump:
    __slots__ = ("name", "filepos", "size")

    def __init__(self, name, filepos, size):
        self.name = name
        self.filepos = filepos
        self.size = size


def _is_map_marker(name):
    if len(name) == 4 and name[0] == "E" and name[2] == "M":
        return name[1].isdigit() and name[3].isdigit()
    return name.startswith("MAP") and name[3:].isdigit()


class WadFile:
    def __init__(self, path):
        with open(path, "rb") as fh:
            self.data = fh.read()
        sig, num, diroff = struct.unpack_from(WAD_HEADER_FMT, self.data, 0)
        sig = sig.decode("ascii", "ignore")
        if sig not in ("IWAD", "PWAD"):
            raise ValueError("Not a WAD file (signature=%r)." % sig)
        self.lumps = []
        for i in range(num):
            off = diroff + i * LUMP_ENTRY_SIZE
            filepos, size, raw = struct.unpack_from(LUMP_ENTRY_FMT, self.data, off)
            name = raw.split(b"\x00", 1)[0].decode("ascii", "ignore").upper()
            self.lumps.append(Lump(name, filepos, size))

    def lump_bytes(self, lump):
        return self.data[lump.filepos:lump.filepos + lump.size]

    def map_lump(self, map_name, member):
        """Return bytes of `member` belonging to the map marker `map_name`.

        Map member lumps immediately follow the marker in directory order.
        """
        for i, lump in enumerate(self.lumps):
            if lump.name == map_name:
                for j in range(i + 1, min(i + 12, len(self.lumps))):
                    if self.lumps[j].name == member:
                        return self.lump_bytes(self.lumps[j])
                    # Stop if we hit the next map marker.
                    if _is_map_marker(self.lumps[j].name):
                        break
                return None
        return None
