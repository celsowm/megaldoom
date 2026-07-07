#!/usr/bin/env python3
"""Extract a Doom map's BSP geometry from DOOM1.WAD and emit C arrays for the
megaldoom lean BSP engine.

Doom's prebuilt VERTEXES/SEGS/SSECTORS/NODES map almost 1:1 onto our Bsp*
structs (even the 0x8000 subsector-leaf bit is identical), so we convert them
directly -- no on-device node builder needed.

The renderer is uniform-height: a line is either a full-height solid wall or an
open gap. We therefore emit a SEG only when its linedef is "solid":
  - one-sided (faces the void), OR
  - two-sided but IMPASSABLE-flagged, OR
  - two-sided with a closed opening (min ceiling <= max floor -> shut door / solid).
Everything else (normal doorways, steps, ledges, drop-offs) is skipped -> a
passable gap. Same set drives render and collision, so no clip-through / no
soft-locks.

Usage: python tools/wad-map-extract.py [--map E1M1] [--wad DOOM1.WAD]
"""

import argparse
import math
import os
import struct
from datetime import datetime, timezone

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_WAD = os.path.join(PROJECT_ROOT, "DOOM1.WAD")

# --- Minimal WAD reader (same layout as tools/wad-extract.py) --------------- #
WAD_HEADER_FMT = "<4sii"   # signature, numLumps, dirOffset
LUMP_ENTRY_FMT = "<ii8s"   # filepos, size, name
LUMP_ENTRY_SIZE = struct.calcsize(LUMP_ENTRY_FMT)


class Lump:
    __slots__ = ("name", "filepos", "size")

    def __init__(self, name, filepos, size):
        self.name = name
        self.filepos = filepos
        self.size = size


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


def _is_map_marker(name):
    if len(name) == 4 and name[0] == "E" and name[2] == "M":
        return name[1].isdigit() and name[3].isdigit()
    return name.startswith("MAP") and name[3:].isdigit()


# --- RayTextureId (must match src/raycast.h) -------------------------------- #
TEX_WALL = 0
TEX_DOOR = 1
TEX_LOCKED_DOOR = 2
TEX_EXIT = 3
TEX_BROWN = 4
TEX_GRAY = 5
TEX_METAL = 6
TEX_BRICK = 7
TEX_TECH = 8

# BspSegType (must match src/bsp_map.h)
SEG_WALL = 0
SEG_DOOR = 1
SEG_LOCKED_DOOR = 2
SEG_EXIT = 3

DOOR_SPECIALS = {1, 26, 27, 28, 31, 32, 33, 34, 117, 118}
LOCKED_DOOR_SPECIALS = {26, 27, 28, 32, 33, 34}
EXIT_SPECIALS = {11, 51}

LINE_FLAG_IMPASSABLE = 0x0001


def texture_id_for(name):
    n = name.upper().rstrip("\x00").rstrip()
    if not n or n == "-":
        return None
    table = [
        ("BROWN", TEX_BROWN),
        ("STARTAN", TEX_TECH), ("COMP", TEX_TECH), ("TEK", TEX_TECH),
        ("SLAD", TEX_TECH), ("SILVER", TEX_TECH),
        ("GRAY", TEX_GRAY), ("PLAT", TEX_GRAY), ("LITE", TEX_GRAY),
        ("METAL", TEX_METAL), ("SUPPORT", TEX_METAL), ("SHAWN", TEX_METAL),
        ("BRICK", TEX_BRICK), ("WOOD", TEX_BRICK), ("BIGDOOR", TEX_BRICK),
        ("DOOR", TEX_BRICK), ("STONE", TEX_BRICK),
    ]
    for prefix, tid in table:
        if n.startswith(prefix):
            return tid
    return TEX_GRAY


def reduce_normal(nx, ny):
    g = math.gcd(abs(nx), abs(ny))
    if g > 1:
        nx //= g
        ny //= g
    return nx, ny


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wad", default=DEFAULT_WAD)
    ap.add_argument("--map", default="E1M1")
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    wad = WadFile(args.wad)
    mapn = args.map.upper()
    out_path = args.out or os.path.join(
        PROJECT_ROOT, "src", "generated_%s_map.c" % mapn.lower()
    )

    def need(member):
        data = wad.map_lump(mapn, member)
        if data is None:
            raise SystemExit("Map %s: lump %s not found" % (mapn, member))
        return data

    verts_raw = need("VERTEXES")
    lines_raw = need("LINEDEFS")
    sides_raw = need("SIDEDEFS")
    sectors_raw = need("SECTORS")
    segs_raw = need("SEGS")
    ssectors_raw = need("SSECTORS")
    nodes_raw = need("NODES")
    things_raw = need("THINGS")

    # Doom is y-up; this engine's proven (hand-map) convention is y-down, so we
    # negate Y everywhere (vertices, node partition Y/dY, player start Y/angle)
    # and swap node front/back children to compensate the flipped partition-side
    # test. This keeps E1M1 in the exact convention the renderer was validated
    # against (not mirrored, turning correct).
    verts_up = [struct.unpack_from("<hh", verts_raw, i * 4)
                for i in range(len(verts_raw) // 4)]
    vertices = [(x, -y) for (x, y) in verts_up]

    sectors = []  # (floor, ceil)
    for i in range(len(sectors_raw) // 26):
        f, c = struct.unpack_from("<hh", sectors_raw, i * 26)
        sectors.append((f, c))

    sidedefs = []  # (upper, lower, middle, sector)
    for i in range(len(sides_raw) // 30):
        xoff, yoff, up, lo, mid, sec = struct.unpack_from(
            "<hh8s8s8sH", sides_raw, i * 30)
        sidedefs.append((
            up.split(b"\x00", 1)[0].decode("ascii", "ignore"),
            lo.split(b"\x00", 1)[0].decode("ascii", "ignore"),
            mid.split(b"\x00", 1)[0].decode("ascii", "ignore"),
            sec,
        ))

    linedefs = []
    for i in range(len(lines_raw) // 14):
        v1, v2, flags, special, tag, right, left = struct.unpack_from(
            "<HHHHHHH", lines_raw, i * 14)
        linedefs.append(dict(v1=v1, v2=v2, flags=flags, special=special,
                             right=right, left=left))

    segs = []
    for i in range(len(segs_raw) // 12):
        v1, v2, angle, ld, direction, offset = struct.unpack_from(
            "<HHhHHh", segs_raw, i * 12)
        segs.append(dict(v1=v1, v2=v2, ld=ld, direction=direction))

    ssectors = []
    for i in range(len(ssectors_raw) // 4):
        count, first = struct.unpack_from("<HH", ssectors_raw, i * 4)
        ssectors.append((count, first))

    nodes = []
    for i in range(len(nodes_raw) // 28):
        vals = struct.unpack_from("<hhhhhhhhhhhhHH", nodes_raw, i * 28)
        x, y, dx, dy = vals[0], vals[1], vals[2], vals[3]
        right_child, left_child = vals[12], vals[13]
        # Y-down flip: negate y and dy, and swap children (front=left, back=right)
        # so render_node's `cross >= 0 -> front` still selects Doom's right side.
        nodes.append(dict(x=x, y=-y, dx=dx, dy=-dy,
                          front=left_child, back=right_child))

    # --- Classify each linedef as solid (wall) or open (gap). --------------- #
    def line_solid(ld):
        if ld["left"] == 0xFFFF:
            return True  # one-sided
        if ld["flags"] & LINE_FLAG_IMPASSABLE:
            return True
        rs = sidedefs[ld["right"]][3] if ld["right"] != 0xFFFF else None
        ls = sidedefs[ld["left"]][3] if ld["left"] != 0xFFFF else None
        if rs is None or ls is None:
            return True
        rf, rc = sectors[rs]
        lf, lc = sectors[ls]
        opening = min(rc, lc) - max(rf, lf)
        return opening <= 0

    def seg_type_and_tex(seg):
        ld = linedefs[seg["ld"]]
        # Front sidedef of this seg.
        front_side = ld["right"] if seg["direction"] == 0 else ld["left"]
        if front_side == 0xFFFF:
            front_side = ld["right"]
        seg_type = SEG_WALL
        if ld["special"] in EXIT_SPECIALS:
            return SEG_EXIT, TEX_EXIT
        if ld["special"] in DOOR_SPECIALS:
            if ld["special"] in LOCKED_DOOR_SPECIALS:
                seg_type = SEG_LOCKED_DOOR
            else:
                seg_type = SEG_DOOR
            return seg_type, TEX_DOOR
        # Texture: prefer middle, then lower, then upper.
        tid = TEX_GRAY
        if front_side != 0xFFFF:
            up, lo, mid, _ = sidedefs[front_side]
            for name in (mid, lo, up):
                t = texture_id_for(name)
                if t is not None:
                    tid = t
                    break
        return seg_type, tid

    # --- Rebuild subsectors with only solid segs, preserving grouping. ------ #
    out_segs = []       # dicts with v1,v2,nx,ny,tex,type
    out_ssectors = []   # (first_seg, count)

    def front_normal(seg):
        """Normal pointing into the seg's FRONT sector, in y-down space.

        Deterministic from Doom's linedef direction + seg side (NOT a centroid
        guess): in y-up, the normal into the right sector is (Ly,-Lx); into the
        left sector it is (-Ly,Lx). We then negate the y component to match the
        engine's y-down space.
        """
        ld = linedefs[seg["ld"]]
        lv1 = verts_up[ld["v1"]]
        lv2 = verts_up[ld["v2"]]
        lx = lv2[0] - lv1[0]
        ly = lv2[1] - lv1[1]
        if seg["direction"] == 0:
            nux, nuy = ly, -lx   # into right sector (y-up)
        else:
            nux, nuy = -ly, lx   # into left sector (y-up)
        return reduce_normal(nux, -nuy)  # flip y to y-down

    for (count, first) in ssectors:
        start = len(out_segs)
        for k in range(count):
            seg = segs[first + k]
            if not line_solid(linedefs[seg["ld"]]):
                continue
            ax, ay = vertices[seg["v1"]]
            bx, by = vertices[seg["v2"]]
            if ax == bx and ay == by:
                continue  # degenerate seg
            nx, ny = front_normal(seg)
            if nx == 0 and ny == 0:
                continue  # degenerate linedef
            stype, tid = seg_type_and_tex(seg)
            out_segs.append(dict(v1=seg["v1"], v2=seg["v2"],
                                nx=nx, ny=ny, tex=tid, type=stype))
        out_ssectors.append((start, len(out_segs) - start))

    if len(out_segs) > 1024:
        raise SystemExit("solid seg count %d exceeds BSP_MAX_SEGS (1024)"
                         % len(out_segs))

    # --- Player 1 start from THINGS (type 1). ------------------------------- #
    start_x = start_y = 0
    start_angle_deg = 0
    for i in range(len(things_raw) // 10):
        tx, ty, tang, ttype, tflags = struct.unpack_from("<hhHHH", things_raw, i * 10)
        if ttype == 1:
            start_x, start_y, start_angle_deg = tx, ty, tang
            break
    # Y-down flip: negate the start Y and mirror the angle about the x-axis.
    start_y = -start_y
    start_angle = (256 - (round(start_angle_deg * 256 / 360) & 255)) & 255

    root = (len(nodes) - 1) if nodes else 0x8000

    # --- Emit C. ------------------------------------------------------------ #
    ts = datetime.now(timezone.utc).replace(microsecond=0).isoformat()
    lines = []
    lines.append("// Generated by tools/wad-map-extract.py")
    lines.append("// Source: %s  map: %s" % (os.path.basename(args.wad), mapn))
    lines.append("// Generated at: %s" % ts)
    lines.append("#include \"bsp_map.h\"")
    lines.append("")
    lines.append("#if !BSP_USE_HAND_MAP")
    lines.append("")

    # Vertices (full original array; segs/nodes reference these indices).
    lines.append("const BspVertex bsp_vertices[%d] = {" % len(vertices))
    for (x, y) in vertices:
        lines.append("    {%d, %d}," % (x, y))
    lines.append("};")
    lines.append("")

    lines.append("const BspSeg bsp_segs[%d] = {" % len(out_segs))
    for s in out_segs:
        lines.append("    {%d, %d, %d, %d, %d, %d}," % (
            s["v1"], s["v2"], s["nx"], s["ny"], s["tex"], s["type"]))
    lines.append("};")
    lines.append("")

    lines.append("const BspSubsector bsp_subsectors[%d] = {" % len(out_ssectors))
    for (first, count) in out_ssectors:
        lines.append("    {%d, %d}," % (first, count))
    lines.append("};")
    lines.append("")

    lines.append("const BspNode bsp_nodes[%d] = {" % len(nodes))
    for nd in nodes:
        lines.append("    {%d, %d, %d, %d, %du, %du}," % (
            nd["x"], nd["y"], nd["dx"], nd["dy"], nd["front"], nd["back"]))
    lines.append("};")
    lines.append("")

    lines.append("const u16 bsp_root_node = %du;" % root)
    lines.append("const u16 bsp_seg_count = %du;" % len(out_segs))
    lines.append("const s32 bsp_player_start_x = %d;" % start_x)
    lines.append("const s32 bsp_player_start_y = %d;" % start_y)
    lines.append("const u16 bsp_player_start_angle = %du;" % start_angle)
    lines.append("")
    lines.append("#endif // !BSP_USE_HAND_MAP")
    lines.append("")

    with open(out_path, "w", newline="\n") as fh:
        fh.write("\n".join(lines))

    print("Wrote %s" % out_path)
    print("  vertices : %d" % len(vertices))
    print("  segs     : %d solid (of %d total)" % (len(out_segs), len(segs)))
    print("  subsectors: %d" % len(out_ssectors))
    print("  nodes    : %d (root=%d)" % (len(nodes), root))
    print("  player   : (%d,%d) angle_deg=%d -> %d" % (
        start_x, start_y, start_angle_deg, start_angle))


if __name__ == "__main__":
    main()
