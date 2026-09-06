#!/usr/bin/env python3
"""Parse one Doom map's WAD lumps and flatten them into MegalDoom's one-level
BSP contract: solid segs, grouped doors, curated THINGS, and a certified
navigation proof.

The runtime is deliberately one level high. Structural walls, grouped doors,
switches, colored locks and exits survive; height-only stairs, lifts, ledges
and drop-offs normally become open passages. A structurally validated offline
recipe may transfer the material of an erased height feature onto an existing
solid room perimeter. It never emits geometry or changes collision/LOS.
Heights and recipe metadata never enter the runtime. Before either
generated file is replaced, load_map()'s offline navigation proof must find a
medium-skill single-player route to an exit.

Knows nothing about C code generation or texture/palette baking -- its output
(MapData) is plain parsed/derived data that bsp_emit.py turns into C source
and world_assets.py's texture_usage/sectors inputs are read from.
"""

from array import array
from collections import Counter
from dataclasses import dataclass
import hashlib
import math
import struct

import raycast_constants

from flat_map_recipes import resolve_flat_material_transfers
from world_assets import FALLBACK_TEXTURE

# Flat runtime contract (must match src/bsp/bsp_map.h / billboard_internal.h).
SEG_WALL = 0
SEG_DOOR = 1
SEG_EXIT = 2
SEG_SWITCH = 3
SEG_TRIGGER = 4
SEG_WINDOW = 5
SEG_SKY_WALL = 6
SEG_FLAG_DIRECT_USE = 0x01
SEG_FLAG_PLAIN_DOOR = 0x02

# A WINDOW is a linedef this converter ALREADY emits as a solid wall -- two
# sided, flagged impassable, no special -- whose two sectors form a real
# vertical opening: one side's floor is higher AND its ceiling lower, i.e. a
# recess you can see over but not walk through. Doom's E1M1 windows are exactly
# that shape, and today they render as blank walls.
#
# Reclassifying one does NOT add, move or remove a SEG: the emitted geometry,
# the blockmap and the navigation certificate are byte-identical, and the
# runtime still treats a window as solid for collision and line of sight. Only
# the type byte and the two band bytes below change, so this is not the
# forbidden "promote a portal band to a flat wall" transform -- nothing that
# was passable becomes solid, and nothing solid moves.

KEY_NONE = 0
KEY_BLUE = 1
KEY_YELLOW = 2
KEY_RED = 4
DOOR_GROUP_NONE = 0xFF
MAX_DOOR_GROUPS = 64
# Doom's collision diameter is 32 map units; the radius is therefore 16.  Keep
# this synchronized with PLAYER_COLLISION_RADIUS in src/raycast.h.
PLAYER_RADIUS = 16
NAV_STEP = 16
PICKUP_RADIUS = 128
USE_RADIUS = 256
BILLBOARD_OBJECT_COUNT = 112
# The one constant slab height every wall projects from. A sky sector taller
# than this already renders its walls SHORTER than they really are, so the sky
# above them survives on its own; only a sector below it needs the sky-wall
# rule (see sky_wall_sector).
WORLD_WALL_HEIGHT = raycast_constants.define("RAY_WORLD_WALL_HEIGHT")
DOOM_THING_SKILL_MEDIUM = 0x0002
DOOM_THING_NOT_SINGLE_PLAYER = 0x0010

DIRECT_DOOR_SPECIALS = {1, 26, 27, 28, 31, 32, 33, 34, 117, 118}
REMOTE_DOOR_SPECIALS = {2, 3, 4, 16, 29, 42, 46, 61, 63, 75, 76, 86, 90, 103}
LOCKED_DOOR_KEYS = {
    26: KEY_BLUE, 32: KEY_BLUE,
    27: KEY_YELLOW, 34: KEY_YELLOW,
    28: KEY_RED, 33: KEY_RED,
}
EXIT_SPECIALS = {11, 51}
TELEPORT_SPECIALS = {39, 97, 125, 126}
BOSS_TRIGGER_MAPS = {"E1M8", "E2M8", "E3M8", "E4M6", "E4M8"}

LINE_FLAG_IMPASSABLE = 0x0001
LINE_FLAG_SECRET = 0x0020
LINE_FLAG_DONTDRAW = 0x0080

# Compact automap visual categories (must match BspAutomapLineKind).  These are
# derived from the original linedefs, never from split BSP SEGs.
AUTOMAP_LINE_SOLID = 0
AUTOMAP_LINE_FLOOR = 1
AUTOMAP_LINE_CEILING = 2
AUTOMAP_LINE_SPECIAL = 3

# The THING types map_thing_type() in src/billboard/billboard.c recognises. Used
# only for reporting and for the progression certificate's object budget; every
# THING is emitted to the map regardless. Keep in sync with that switch.
RUNTIME_THING_TYPES = {5, 6, 9, 13, 2001, 2002, 2005, 2007, 2008, 2011, 2012,
                       2014, 2015, 2018, 2019, 2035, 2048, 2049, 3001, 3004}
KEY_THING_MASK = {5: KEY_BLUE, 6: KEY_YELLOW, 13: KEY_RED}
BLOCKING_THING_RADIUS = {2035: 20}


@dataclass
class MapData:
    """Everything bsp_emit.py and the wad-map-extract.py report need from a
    parsed and flattened Doom map."""
    mapn: str
    vertices: list
    sectors: list
    linedefs: list
    automap_lines: list
    linedef_automap_indices: list
    source_seg_count: int
    out_segs: list
    out_ssectors: list
    out_ssector_sectors: list
    nodes: list
    out_things: list
    supported_things: int
    start_x: int
    start_y: int
    start_angle: int
    start_angle_deg: int
    texture_usage: Counter
    next_door_group: int
    door_face_counts: Counter
    fallback_door_faces: int
    wad_sha256: str
    baseline_seg_count: int
    curated_material_linedefs: list
    curated_material_segs: int
    curated_material_reports: list
    required_key_mask: int
    certificate: dict


def clean_name(raw):
    if isinstance(raw, bytes):
        raw = raw.split(b"\x00", 1)[0].decode("ascii", "ignore")
    return raw.upper().rstrip("\x00").rstrip()


def reduce_normal(nx, ny):
    g = math.gcd(abs(nx), abs(ny))
    if g > 1:
        nx //= g
        ny //= g
    return nx, ny


def point_segment_dist2(ax, ay, bx, by, px, py):
    abx = bx - ax
    aby = by - ay
    apx = px - ax
    apy = py - ay
    ab2 = abx * abx + aby * aby
    if ab2 <= 0:
        cx, cy = ax, ay
    else:
        dot = apx * abx + apy * aby
        if dot <= 0:
            cx, cy = ax, ay
        elif dot >= ab2:
            cx, cy = bx, by
        else:
            cx = ax + (abx * dot) // ab2
            cy = ay + (aby * dot) // ab2
    dx = px - cx
    dy = py - cy
    return dx * dx + dy * dy


def runtime_things(things):
    result = []
    for thing in things:
        x, y, thing_type, angle, flags = thing
        if not (flags & DOOM_THING_SKILL_MEDIUM) or flags & DOOM_THING_NOT_SINGLE_PLAYER:
            continue
        if thing_type not in RUNTIME_THING_TYPES:
            continue
        if len(result) >= BILLBOARD_OBJECT_COUNT:
            break
        result.append(thing)
    return result


def validate_spatial_grid(vertices, segs, grid_min_x, grid_min_y, grid_w,
                          grid_h, grid_cell, grid_cells, start_x, start_y):
    """Prove indexed queries match exhaustive queries on deterministic samples."""
    def cell_coord(value, origin):
        return (value - origin) // grid_cell

    def circle_candidates(x, y, radius):
        result = set()
        cx0 = max(0, cell_coord(x - radius, grid_min_x))
        cy0 = max(0, cell_coord(y - radius, grid_min_y))
        cx1 = min(grid_w - 1, cell_coord(x + radius, grid_min_x))
        cy1 = min(grid_h - 1, cell_coord(y + radius, grid_min_y))
        for cy in range(cy0, cy1 + 1):
            for cx in range(cx0, cx1 + 1):
                result.update(grid_cells[cy * grid_w + cx])
        return result

    def los_candidates(x0, y0, x1, y1):
        cx, cy = cell_coord(x0, grid_min_x), cell_coord(y0, grid_min_y)
        ex, ey = cell_coord(x1, grid_min_x), cell_coord(y1, grid_min_y)
        sx = 1 if ex > cx else (-1 if ex < cx else 0)
        sy = 1 if ey > cy else (-1 if ey < cy else 0)
        ray_dx, ray_dy = abs(x1 - x0), abs(y1 - y0)
        result = set()
        guard = 0
        while True:
            guard += 1
            if guard > (grid_w + grid_h + 8):
                raise SystemExit("blockmap LOS traversal did not converge for ray %r" %
                                 ((x0, y0, x1, y1),))
            if 0 <= cx < grid_w and 0 <= cy < grid_h:
                result.update(grid_cells[cy * grid_w + cx])
            if cx == ex and cy == ey:
                return result
            if sx == 0:
                cy += sy
                continue
            if sy == 0:
                cx += sx
                continue
            if cx == ex:
                cy += sy
                continue
            if cy == ey:
                cx += sx
                continue
            xb = grid_min_x + ((cx + 1 if sx > 0 else cx) * grid_cell)
            yb = grid_min_y + ((cy + 1 if sy > 0 else cy) * grid_cell)
            xdist = xb - x0 if sx > 0 else x0 - xb
            ydist = yb - y0 if sy > 0 else y0 - yb
            lhs, rhs = xdist * ray_dy, ydist * ray_dx
            if lhs == rhs:
                cx, cy = cx + sx, cy + sy
            elif lhs < rhs:
                cx += sx
            else:
                cy += sy

    def cross(ax, ay, bx, by, cx, cy):
        return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax)

    def strict_intersection(x0, y0, x1, y1, ax, ay, bx, by):
        d1 = cross(x0, y0, x1, y1, ax, ay)
        d2 = cross(x0, y0, x1, y1, bx, by)
        d3 = cross(ax, ay, bx, by, x0, y0)
        d4 = cross(ax, ay, bx, by, x1, y1)
        return (((d1 > 0 and d2 < 0) or (d1 < 0 and d2 > 0)) and
                ((d3 > 0 and d4 < 0) or (d3 < 0 and d4 > 0)))

    # Circle broad phase: any segment whose expanded AABB contains the sample
    # point must be present. Include route start, cell boundaries and wall ends.
    points = [(start_x, start_y)]
    for x, y in vertices:
        points.extend(((x, y), (x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)))
    for cy in range(grid_h + 1):
        for cx in range(grid_w + 1):
            points.append((grid_min_x + cx * grid_cell,
                           grid_min_y + cy * grid_cell))
    for radius in (24, 32):
        for x, y in points:
            candidates = circle_candidates(x, y, radius)
            for i, seg in enumerate(segs):
                ax, ay = vertices[seg["v1"]]
                bx, by = vertices[seg["v2"]]
                if (x + radius >= min(ax, bx) and x - radius <= max(ax, bx) and
                        y + radius >= min(ay, by) and y - radius <= max(ay, by) and
                        i not in candidates):
                    raise SystemExit("blockmap circle validation failed at segment %d" % i)

    # LOS broad phase: a segment can intersect a query only if both AABBs
    # overlap. Exercise horizontal, vertical, diagonal, zero-length and the
    # player-start-to-wall route family.
    rays = [(start_x, start_y, start_x, start_y)]
    bounds = (grid_min_x, grid_min_y,
              grid_min_x + grid_w * grid_cell - 1,
              grid_min_y + grid_h * grid_cell - 1)
    rays.extend(((bounds[0], start_y, bounds[2], start_y),
                 (start_x, bounds[1], start_x, bounds[3]),
                 (bounds[0], bounds[1], bounds[2], bounds[3]),
                 (bounds[0], bounds[3], bounds[2], bounds[1])))
    rays.extend((start_x, start_y, x, y) for x, y in vertices)
    for x0, y0, x1, y1 in rays:
        candidates = los_candidates(x0, y0, x1, y1)
        rminx, rmaxx = min(x0, x1), max(x0, x1)
        rminy, rmaxy = min(y0, y1), max(y0, y1)
        for i, seg in enumerate(segs):
            ax, ay = vertices[seg["v1"]]
            bx, by = vertices[seg["v2"]]
            if (rmaxx >= min(ax, bx) and rminx <= max(ax, bx) and
                    rmaxy >= min(ay, by) and rminy <= max(ay, by) and
                    strict_intersection(x0, y0, x1, y1, ax, ay, bx, by) and
                    i not in candidates):
                raise SystemExit("blockmap LOS validation failed at segment %d ray=(%d,%d)-(%d,%d) seg=(%d,%d)-(%d,%d)" %
                                 (i, x0, y0, x1, y1, ax, ay, bx, by))

    return len(points) * 2 + len(rays)


def certify_flat_progression(vertices, segs, things, start_x, start_y,
                             capture_route=False):
    """Prove a concrete medium/single-player route through the emitted flat map."""
    collision_radius = PLAYER_RADIUS
    exits = [(index, seg) for index, seg in enumerate(segs)
             if seg["type"] == SEG_EXIT]
    if not exits:
        raise ValueError("no supported exit linedef (special 11/51) survives flattening")

    active_things = runtime_things(things)
    source_key_mask = KEY_NONE
    medium_key_mask = KEY_NONE
    for _, _, thing_type, _, flags in things:
        key_bit = KEY_THING_MASK.get(thing_type, KEY_NONE)
        source_key_mask |= key_bit
        if ((flags & DOOM_THING_SKILL_MEDIUM) and
                not (flags & DOOM_THING_NOT_SINGLE_PLAYER)):
            medium_key_mask |= key_bit
    keys = [(x, y, KEY_THING_MASK[thing_type])
            for x, y, thing_type, _, _ in active_things
            if thing_type in KEY_THING_MASK]
    blockers = [(x, y, BLOCKING_THING_RADIUS[thing_type])
                for x, y, thing_type, _, _ in active_things
                if thing_type in BLOCKING_THING_RADIUS]
    available_key_mask = KEY_NONE
    for _, _, key_bit in keys:
        available_key_mask |= key_bit
    interactions = [seg for seg in segs
                    if seg["type"] in (SEG_SWITCH, SEG_TRIGGER) or
                    (seg["type"] == SEG_DOOR and
                     seg.get("flags", 0) & SEG_FLAG_DIRECT_USE)]

    min_x = (min(x for x, _ in vertices) // NAV_STEP) * NAV_STEP
    min_y = (min(y for _, y in vertices) // NAV_STEP) * NAV_STEP
    max_x = ((max(x for x, _ in vertices) + NAV_STEP - 1) // NAV_STEP) * NAV_STEP
    max_y = ((max(y for _, y in vertices) + NAV_STEP - 1) // NAV_STEP) * NAV_STEP
    width = ((max_x - min_x) // NAV_STEP) + 1
    height = ((max_y - min_y) // NAV_STEP) + 1

    broad_cell = 256
    broad_w = ((max_x - min_x) // broad_cell) + 1
    broad_h = ((max_y - min_y) // broad_cell) + 1
    broad = [[] for _ in range(broad_w * broad_h)]
    for index, seg in enumerate(segs):
        ax, ay = vertices[seg["v1"]]
        bx, by = vertices[seg["v2"]]
        cx0 = max(0, (min(ax, bx) - collision_radius - min_x) // broad_cell)
        cx1 = min(broad_w - 1, (max(ax, bx) + collision_radius - min_x) // broad_cell)
        cy0 = max(0, (min(ay, by) - collision_radius - min_y) // broad_cell)
        cy1 = min(broad_h - 1, (max(ay, by) + collision_radius - min_y) // broad_cell)
        for cy in range(cy0, cy1 + 1):
            for cx in range(cx0, cx1 + 1):
                broad[cy * broad_w + cx].append(index)

    grid_collision_state = bytearray(width * height)  # 0 unknown, 1 clear, 2 static wall
    grid_door_groups = array("Q", [0]) * (width * height)
    group_keys = {}
    for seg in segs:
        if seg["type"] == SEG_DOOR:
            group_keys[seg["door_group"]] = seg["required_key"]

    def collision_components(px, py):
        if px < min_x or px > max_x or py < min_y or py > max_y:
            return True, 0
        cx = min(broad_w - 1, max(0, (px - min_x) // broad_cell))
        cy = min(broad_h - 1, max(0, (py - min_y) // broad_cell))
        door_groups = 0
        for index in broad[cy * broad_w + cx]:
            seg = segs[index]
            if seg["type"] == SEG_TRIGGER:
                continue
            ax, ay = vertices[seg["v1"]]
            bx, by = vertices[seg["v2"]]
            if point_segment_dist2(ax, ay, bx, by, px, py) < collision_radius ** 2:
                if seg["type"] == SEG_DOOR:
                    door_groups |= 1 << seg["door_group"]
                else:
                    return True, door_groups
        for ox, oy, radius in blockers:
            dx = px - ox
            dy = py - oy
            if dx * dx + dy * dy < (collision_radius + radius) ** 2:
                return True, door_groups
        return False, door_groups

    def blocked(px, py, key_mask, opened_groups):
        aligned = ((px - min_x) % NAV_STEP == 0 and
                   (py - min_y) % NAV_STEP == 0 and
                   min_x <= px <= max_x and min_y <= py <= max_y)
        if aligned:
            index = ((py - min_y) // NAV_STEP) * width + \
                ((px - min_x) // NAV_STEP)
            if grid_collision_state[index] == 0:
                static, door_groups = collision_components(px, py)
                grid_collision_state[index] = 2 if static else 1
                grid_door_groups[index] = door_groups
            if grid_collision_state[index] == 2:
                return True
            door_groups = grid_door_groups[index]
        else:
            static, door_groups = collision_components(px, py)
            if static:
                return True

        if not door_groups:
            return False
        key_allowed = 0
        for group, required in group_keys.items():
            if required == KEY_NONE or key_mask & required == required:
                key_allowed |= 1 << group
        allowed = opened_groups & key_allowed
        return bool(door_groups & ~allowed)

    def exit_reached(px, py):
        for exit_index, seg in exits:
            ax, ay = vertices[seg["v1"]]
            bx, by = vertices[seg["v2"]]
            # The regular certificate proves progression at runtime reach. A
            # captured E2E route continues to a close certified standoff so a
            # nearer door cannot win bsp_use_in_front's probe ordering.
            reach = 64 if capture_route else USE_RADIUS
            if point_segment_dist2(ax, ay, bx, by, px, py) <= reach ** 2:
                return exit_index
        return None

    seed_x = min(max_x, max(min_x, round((start_x - min_x) / NAV_STEP) * NAV_STEP + min_x))
    seed_y = min(max_y, max(min_y, round((start_y - min_y) / NAV_STEP) * NAV_STEP + min_y))
    if (blocked(start_x, start_y, KEY_NONE, 0) or
            blocked(seed_x, seed_y, KEY_NONE, 0)):
        raise ValueError("player start is blocked in emitted flat geometry")

    cell_count = width * height
    visited = [bytearray(cell_count) for _ in range(8)]
    # Optional predecessor graph for the E2E route generator.  The regular
    # certificate still uses its compact epoch queue; refs are allocated only
    # on demand so release asset conversion retains its established footprint.
    route_refs = [array("i", [-1]) * cell_count for _ in range(8)] if capture_route else None
    route_nodes = []
    visited_counts = [0] * 8
    opened_by_key = [0] * 8
    queue = array("I")
    queue_head = 0
    epochs = [0] * 8

    def queue_entry(index, key_mask):
        return ((epochs[key_mask] * 8 + key_mask) * cell_count) + index

    seed = ((seed_y - min_y) // NAV_STEP) * width + ((seed_x - min_x) // NAV_STEP)
    visited[KEY_NONE][seed] = 1
    visited_counts[KEY_NONE] = 1
    queue.append(queue_entry(seed, KEY_NONE))
    if capture_route:
        route_nodes.append((-1, seed_x, seed_y, "start", None))
        route_refs[KEY_NONE][seed] = 0
    reached_masks = set()
    reached_open_groups = 0
    directions = ((NAV_STEP, 0), (-NAV_STEP, 0), (0, NAV_STEP), (0, -NAV_STEP),
                  (NAV_STEP, NAV_STEP), (NAV_STEP, -NAV_STEP),
                  (-NAV_STEP, NAV_STEP), (-NAV_STEP, -NAV_STEP))
    # The captured host route follows the same four-neighbour movement that
    # the controller can realize without cutting a collision corner. The
    # release certificate keeps its diagonal reachability proof unchanged.
    route_directions = directions[:4] if capture_route else directions

    while queue_head < len(queue):
        encoded = queue[queue_head]
        queue_head += 1
        index = encoded % cell_count
        state = encoded // cell_count
        key_mask = state & 7
        if (state >> 3) != epochs[key_mask]:
            continue
        px = min_x + (index % width) * NAV_STEP
        py = min_y + (index // width) * NAV_STEP
        route_ref = route_refs[key_mask][index] if capture_route else -1
        opened_groups = opened_by_key[key_mask]
        reached_masks.add(key_mask)
        reached_open_groups |= opened_groups
        exit_index = exit_reached(px, py)
        if exit_index is not None:
            route = None
            if capture_route:
                route = []
                while route_ref >= 0:
                    node = route_nodes[route_ref]
                    route.append(dict(x=node[1], y=node[2], action=node[3], detail=node[4]))
                    route_ref = node[0]
                route.reverse()
            return {
                "reachable": True,
                "key_mask": key_mask,
                "available_keys": available_key_mask,
                "reached_masks": sorted(reached_masks),
                "opened_groups": opened_groups,
                "exit_index": exit_index,
                "states": sum(visited_counts),
                "route": route,
            }

        collected = key_mask
        for key_x, key_y, key_bit in keys:
            dx = px - key_x
            dy = py - key_y
            if dx * dx + dy * dy <= PICKUP_RADIUS ** 2:
                collected |= key_bit
        if collected != key_mask:
            index = ((py - min_y) // NAV_STEP) * width + ((px - min_x) // NAV_STEP)
            groups_changed = ((opened_by_key[collected] | opened_groups) !=
                              opened_by_key[collected])
            opened_by_key[collected] |= opened_groups
            if groups_changed:
                # More-open geometry is monotonic and movement is reversible.
                # Restart this mask from the concrete current position instead
                # of duplicating its entire reached component in the queue.
                epochs[collected] += 1
                visited[collected] = bytearray(cell_count)
                visited_counts[collected] = 0
            if not visited[collected][index]:
                visited[collected][index] = 1
                visited_counts[collected] += 1
                queue.append(queue_entry(index, collected))
                if capture_route:
                    route_nodes.append((route_ref, px, py, "key", collected ^ key_mask))
                    route_refs[collected][index] = len(route_nodes) - 1

        # Remote switches are monotonic for the proof: opening a group can only
        # add routes, and the runtime can reproduce the same sequence with use.
        newly_opened = opened_groups
        opened_interaction = None
        for interaction in interactions:
            required = interaction["required_key"]
            if required != KEY_NONE and key_mask & required != required:
                continue
            ax, ay = vertices[interaction["v1"]]
            bx, by = vertices[interaction["v2"]]
            if point_segment_dist2(ax, ay, bx, by, px, py) <= USE_RADIUS ** 2:
                newly_opened |= 1 << interaction["door_group"]
                opened_interaction = interaction
        if newly_opened != opened_groups:
            opened_by_key[key_mask] = newly_opened
            reached_open_groups |= newly_opened
            # Opening only removes collision. The current position remains a
            # valid seed for the whole previously reached undirected component,
            # so restart this mask compactly instead of enqueuing every cell.
            epochs[key_mask] += 1
            current_index = index
            visited[key_mask] = bytearray(cell_count)
            visited[key_mask][current_index] = 1
            visited_counts[key_mask] = 1
            queue.append(queue_entry(current_index, key_mask))
            if capture_route:
                route_nodes.append((route_ref, px, py, "use", opened_interaction))
                route_refs[key_mask] = array("i", [-1]) * cell_count
                route_refs[key_mask][current_index] = len(route_nodes) - 1
                route_ref = len(route_nodes) - 1
            opened_groups = newly_opened

        for dx, dy in route_directions:
            nx, ny = px + dx, py + dy
            if blocked(nx, ny, key_mask, opened_groups):
                continue
            if dx and dy:
                if (blocked(px + dx, py, key_mask, opened_groups) or
                        blocked(px, py + dy, key_mask, opened_groups)):
                    continue
            index = ((ny - min_y) // NAV_STEP) * width + ((nx - min_x) // NAV_STEP)
            if visited[key_mask][index]:
                continue
            visited[key_mask][index] = 1
            visited_counts[key_mask] += 1
            queue.append(queue_entry(index, key_mask))
            if capture_route:
                route_nodes.append((route_ref, nx, ny, "move", None))
                route_refs[key_mask][index] = len(route_nodes) - 1

        # Drop processed storage periodically; array('I') keeps the frontier at
        # four bytes per cell instead of Python tuple/object overhead.
        if queue_head >= 262144 and queue_head * 2 >= len(queue):
            queue = array("I", queue[queue_head:])
            queue_head = 0

    required = 0
    for seg in segs:
        required |= seg.get("required_key", KEY_NONE)
    missing = required & ~available_key_mask
    detail = "exit unreachable after %d navigation states; reached key masks=%s" % (
        sum(visited_counts), sorted(reached_masks))
    if reached_open_groups:
        detail += "; opened door groups=0x%X" % reached_open_groups
    if missing:
        detail += "; missing required key mask=0x%02X" % missing
        filtered = missing & source_key_mask & ~available_key_mask
        if filtered:
            reasons = []
            if filtered & ~medium_key_mask:
                reasons.append("skill/single-player filter")
            if filtered & medium_key_mask:
                reasons.append("runtime object cap")
            detail += " (discarded by %s)" % " and ".join(reasons)
    raise ValueError(detail)


def load_map(wad, mapn, apply_recipes=True, apply_windows=True,
             apply_sky_walls=True, apply_plain_doors=True,
             apply_automap=True):
    """Flatten one Doom map.

    apply_windows=False is the negative control for the window
    reclassification: it must produce the SAME segs in the same order with the
    same geometry, differing only in the type byte. tools/test-sector-map.py
    relies on that to prove windows never move geometry.

    apply_sky_walls=False is the same negative control for the sky-wall
    reclassification below: a one-sided line is already forced solid by
    line_solid_without_recipe unconditionally, so this can only ever change a
    seg's type byte and the otherwise-unused door_group metadata byte, never
    its geometry.

    apply_plain_doors=False disables only the visual classification of Doom
    SECRET-flagged physical door groups. It is a negative control proving the
    shipped rule changes one BspSeg flag bit and nothing structural.

    apply_automap=False is a conversion control: it omits only the derived
    original-linedef display table and its SEG mapping. All playable map data
    must remain identical.
    """
    """Parse `mapn`'s lumps out of `wad` and flatten them into a MapData."""

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

    sectors = []
    for i in range(len(sectors_raw) // 26):
        floor_h, ceiling_h, floor_name, ceiling_name, light, special, tag = struct.unpack_from(
            "<hh8s8shhh", sectors_raw, i * 26)
        sectors.append(dict(
            floor=floor_h,
            ceiling=ceiling_h,
            floor_name=clean_name(floor_name),
            ceiling_name=clean_name(ceiling_name),
            light=max(0, min(255, light)), special=special, tag=tag,
        ))

    sidedefs = []
    for i in range(len(sides_raw) // 30):
        xoff, yoff, up, lo, mid, sec = struct.unpack_from(
            "<hh8s8s8sH", sides_raw, i * 30)
        sidedefs.append(dict(xoff=xoff, yoff=yoff, upper=clean_name(up),
                             lower=clean_name(lo), middle=clean_name(mid), sector=sec))

    linedefs = []
    for i in range(len(lines_raw) // 14):
        v1, v2, flags, special, tag, right, left = struct.unpack_from(
            "<HHHHHHH", lines_raw, i * 14)
        linedefs.append(dict(v1=v1, v2=v2, flags=flags, special=special,
                             tag=tag, right=right, left=left))

    # Keep one visual record per original WAD linedef.  The flat BSP may split
    # one linedef into several SEGs, but the automap must never show those
    # implementation seams.  Priority follows Doom's automap semantics: a
    # special remains visually distinct even when it is also a solid door.
    automap_lines = []
    linedef_automap_indices = [0xFFFF] * len(linedefs)
    for line_id, ld in enumerate(linedefs) if apply_automap else ():
        if ld["flags"] & LINE_FLAG_DONTDRAW:
            continue
        front_sector = sidedefs[ld["right"]]["sector"] if ld["right"] != 0xFFFF else 0xFF
        back_sector = sidedefs[ld["left"]]["sector"] if ld["left"] != 0xFFFF else 0xFF
        kind = None
        if front_sector == 0xFF or back_sector == 0xFF or \
                (ld["flags"] & LINE_FLAG_SECRET):
            kind = AUTOMAP_LINE_SOLID
        elif ld["special"] != 0:
            kind = AUTOMAP_LINE_SPECIAL
        else:
            front = sectors[front_sector]
            back = sectors[back_sector]
            if front["floor"] != back["floor"]:
                kind = AUTOMAP_LINE_FLOOR
            elif front["ceiling"] != back["ceiling"]:
                kind = AUTOMAP_LINE_CEILING
        if kind is None:
            continue
        linedef_automap_indices[line_id] = len(automap_lines)
        automap_lines.append(dict(
            v1=ld["v1"], v2=ld["v2"],
            front_sector=front_sector, back_sector=back_sector,
            kind=kind, flags=0,
        ))

    material_transfers = resolve_flat_material_transfers(
        mapn, vertices, linedefs, sidedefs, sectors) if apply_recipes else {}

    segs = []
    for i in range(len(segs_raw) // 12):
        v1, v2, angle, ld, direction, offset = struct.unpack_from(
            "<HHhHHh", segs_raw, i * 12)
        segs.append(dict(v1=v1, v2=v2, ld=ld, direction=direction, offset=offset))

    ssectors = []
    for i in range(len(ssectors_raw) // 4):
        count, first = struct.unpack_from("<HH", ssectors_raw, i * 4)
        ssectors.append((count, first))

    nodes = []

    def flip_bbox(raw):
        """Convert Doom (top,bottom,left,right) y-up bbox to engine y-down."""
        top, bottom, left, right = raw
        xs = (left, right)
        ys = (-top, -bottom)
        return (min(xs), min(ys), max(xs), max(ys))

    for i in range(len(nodes_raw) // 28):
        vals = struct.unpack_from("<hhhhhhhhhhhhHH", nodes_raw, i * 28)
        x, y, dx, dy = vals[0], vals[1], vals[2], vals[3]
        right_box = flip_bbox(vals[4:8])
        left_box = flip_bbox(vals[8:12])
        right_child, left_child = vals[12], vals[13]
        # Y-down flip: negate y and dy, and swap children (front=left, back=right)
        # so render_node's `cross >= 0 -> front` still selects Doom's right side.
        # Boxes follow their children through the same swap.
        nodes.append(dict(x=x, y=-y, dx=dx, dy=-dy,
                          front_box=left_box, back_box=right_box,
                          front=left_child, back=right_child))

    def line_sector_ids(ld):
        result = []
        for side_id in (ld["right"], ld["left"]):
            if side_id != 0xFFFF:
                result.append(sidedefs[side_id]["sector"])
        return result

    def line_opening(ld):
        sector_ids = line_sector_ids(ld)
        if len(sector_ids) != 2:
            return None
        first, second = (sectors[index] for index in sector_ids)
        return min(first["ceiling"], second["ceiling"]) - \
            max(first["floor"], second["floor"])

    # Door actions identify physical door sectors before heights are discarded.
    # All sectors targeted by one remote tag deliberately share one runtime
    # group: Doom opens them together, so every emitted face must change state
    # together as well. Direct doors use the narrow/closed adjacent sector.
    remote_tags = sorted({ld["tag"] for ld in linedefs
                          if ld["special"] in REMOTE_DOOR_SPECIALS and ld["tag"]})
    sector_door_group = {}
    remote_tag_group = {}
    next_door_group = 0
    for tag in remote_tags:
        tagged = [index for index, sector in enumerate(sectors)
                  if sector["tag"] == tag]
        existing = sorted({sector_door_group[index] for index in tagged
                           if index in sector_door_group})
        group = existing[0] if existing else next_door_group
        if not existing:
            next_door_group += 1
        remote_tag_group[tag] = group
        for sector_id in tagged:
            sector_door_group[sector_id] = group

    direct_line_sector = {}
    for line_id, ld in enumerate(linedefs):
        if ld["special"] not in DIRECT_DOOR_SPECIALS:
            continue
        candidates = line_sector_ids(ld)
        if not candidates:
            continue
        sector_id = min(candidates, key=lambda index:
            (sectors[index]["ceiling"] - sectors[index]["floor"], index))
        direct_line_sector[line_id] = sector_id
        if sector_id not in sector_door_group:
            sector_door_group[sector_id] = next_door_group
            next_door_group += 1

    line_door_group = {}
    for line_id, ld in enumerate(linedefs):
        candidates = [sector_id for sector_id in line_sector_ids(ld)
                      if sector_id in sector_door_group]
        group = None
        if candidates and (line_opening(ld) is not None and line_opening(ld) <= 0):
            sector_id = min(candidates, key=lambda index:
                (sectors[index]["ceiling"] - sectors[index]["floor"], index))
            group = sector_door_group[sector_id]
        elif line_id in direct_line_sector:
            group = sector_door_group[direct_line_sector[line_id]]
        if group is not None:
            line_door_group[line_id] = group

    # Doom's SECRET linedef flag is the visual tell for a camouflaged door: it
    # stays a wall material instead of acquiring an obvious door frame. The
    # flag is not guaranteed to be repeated on both physical faces, so promote
    # it to group metadata and stamp every emitted BSP face consistently.
    plain_door_groups = {
        group for line_id, group in line_door_group.items()
        if apply_plain_doors and
        (linedefs[line_id]["flags"] & LINE_FLAG_SECRET)
    }

    remote_line_group = {
        line_id: remote_tag_group[ld["tag"]]
        for line_id, ld in enumerate(linedefs)
        if ld["special"] in REMOTE_DOOR_SPECIALS and ld["tag"] in remote_tag_group
    }

    if next_door_group > MAX_DOOR_GROUPS:
        raise SystemExit("door group count %d exceeds BSP_MAX_DOORS (%d)" %
                         (next_door_group, MAX_DOOR_GROUPS))

    group_required_key = [KEY_NONE] * next_door_group
    for line_id, group in line_door_group.items():
        group_required_key[group] |= LOCKED_DOOR_KEYS.get(
            linedefs[line_id]["special"], KEY_NONE)

    def window_recess(line_id, ld):
        """Return (room_sector, recess_sector) when this line is a window.

        A window is a line the flattener already keeps solid (two-sided and
        impassable, carrying no special), whose back side is a recess: floor
        strictly higher AND ceiling strictly lower than the room's. That
        excludes same-sector midtexture decorations -- E1M1's BRNBIG* panels in
        sector 72 have identical heights on both sides -- and excludes ordinary
        height transitions, which the flattener erases instead.
        """
        if ld["left"] == 0xFFFF or not (ld["flags"] & LINE_FLAG_IMPASSABLE):
            return None
        if (ld["special"] in EXIT_SPECIALS or line_id in line_door_group or
                line_id in remote_line_group):
            return None
        sector_ids = line_sector_ids(ld)
        if len(sector_ids) != 2 or sector_ids[0] == sector_ids[1]:
            return None
        for room_id, recess_id in (sector_ids, sector_ids[::-1]):
            room, recess = sectors[room_id], sectors[recess_id]
            if recess["floor"] > room["floor"] and recess["ceiling"] < room["ceiling"]:
                return room_id, recess_id
        return None

    window_lines = {
        line_id: recess
        for line_id, ld in enumerate(linedefs)
        if apply_windows and (recess := window_recess(line_id, ld)) is not None
    }

    def sky_wall_sector(line_id, ld):
        """One-sided line bounding a LOW F_SKY1-ceilinged sector: a parapet.

        This engine has no per-wall height (every solid wall projects from one
        constant, RAY_WORLD_WALL_HEIGHT), so a real Doom low parapet with sky
        visible above it cannot be represented -- these lines would otherwise
        render as a full-height opaque wall and leave almost no sky visible
        from inside the sector they bound. A one-sided line is already forced
        solid unconditionally (see line_solid_without_recipe), so recognising
        it here only changes how it is drawn, never the geometry.

        The sky ceiling alone is NOT enough to recognise one. An open-air
        courtyard is an F_SKY1 sector too, and the buildings standing in it
        present their exterior faces to it as ordinary one-sided lines --
        E1M1's start room is exactly that, and blanking its east wall left the
        two window recesses in it hanging in mid-air with no building around
        them. What separates the two cases is the sector's own height: at or
        above WORLD_WALL_HEIGHT the slab is already no taller than the real
        wall, so it hides no sky that Doom itself would have shown and the
        wall has to be drawn. Only below it does the slab overshoot the real
        parapet and bury the sky the player is meant to see over.
        """
        if ld["left"] != 0xFFFF or ld["right"] == 0xFFFF:
            return None
        sector_id = sidedefs[ld["right"]]["sector"]
        sector = sectors[sector_id]
        if sector["ceiling_name"] != "F_SKY1":
            return None
        if sector["ceiling"] - sector["floor"] >= WORLD_WALL_HEIGHT:
            return None
        return sector_id

    sky_wall_lines = {
        line_id: sector_id
        for line_id, ld in enumerate(linedefs)
        if apply_sky_walls and
        (sector_id := sky_wall_sector(line_id, ld)) is not None
    }

    def window_band(seg):
        """Q8 band of the drawn slab that the opening occupies, from its top.

        The runtime draws every wall as one 128-unit slab centred on the
        viewport, so the opening cannot be projected -- it is expressed as a
        fraction of the room's own floor-to-ceiling span and applied to the
        slab. The fractions are computed against the seg's OWN front sector, so
        the two faces of one window each get the band a viewer on that side
        would see.
        """
        ld = linedefs[seg["ld"]]
        _, recess_id = window_lines[seg["ld"]]
        front_side = front_side_for(seg)
        room_id = sidedefs[front_side]["sector"] if front_side != 0xFFFF else None
        if room_id is None or room_id == recess_id:
            # Viewed from inside the recess: fall back to the recorded room.
            room_id = window_lines[seg["ld"]][0]
        room, recess = sectors[room_id], sectors[recess_id]
        span = room["ceiling"] - room["floor"]
        if span <= 0:
            return None
        sill = recess["floor"] - room["floor"]
        head = recess["ceiling"] - room["floor"]
        # Clamp into the room: a recess taller than its room would otherwise
        # produce a band outside the slab.
        sill = max(0, min(span, sill))
        head = max(0, min(span, head))
        if head <= sill:
            return None
        band_top = ((span - head) * 256) // span
        band_bottom = ((span - sill) * 256) // span
        band_top = max(0, min(255, band_top))
        band_bottom = max(0, min(255, band_bottom))
        if band_bottom <= band_top:
            return None
        return band_top, band_bottom

    # --- Classify each linedef in the flattened world. ---------------------- #
    def line_solid_without_recipe(line_id, ld):
        if ld["left"] == 0xFFFF:
            return True
        if ld["flags"] & LINE_FLAG_IMPASSABLE:
            return True
        if (ld["special"] in EXIT_SPECIALS or line_id in line_door_group or
                line_id in remote_line_group):
            return True
        # Every remaining two-sided height transition becomes an open gap. This
        # deliberately removes stairs, lifts, ledges and closed height tricks.
        return False

    def front_side_for(seg):
        ld = linedefs[seg["ld"]]
        front_side = ld["right"] if seg["direction"] == 0 else ld["left"]
        if front_side == 0xFFFF:
            front_side = ld["right"]
        return front_side

    def back_side_for(seg):
        ld = linedefs[seg["ld"]]
        return ld["left"] if seg["direction"] == 0 else ld["right"]

    def front_normal(seg):
        """Normal pointing into the SEG front sector in engine y-down space."""
        ld = linedefs[seg["ld"]]
        lv1 = verts_up[ld["v1"]]
        lv2 = verts_up[ld["v2"]]
        lx = lv2[0] - lv1[0]
        ly = lv2[1] - lv1[1]
        if seg["direction"] == 0:
            nux, nuy = ly, -lx
        else:
            nux, nuy = -ly, lx
        return reduce_normal(nux, -nuy)

    def seg_type_and_visual(seg):
        ld = linedefs[seg["ld"]]
        front_side = front_side_for(seg)
        back_side = back_side_for(seg)
        transfer = material_transfers.get(seg["ld"])
        seg_type = SEG_WALL
        door_group = DOOR_GROUP_NONE
        required_key = KEY_NONE
        flags = 0
        if ld["special"] in EXIT_SPECIALS:
            seg_type = SEG_EXIT
        elif seg["ld"] in remote_line_group:
            structurally_solid = (ld["left"] == 0xFFFF or
                                  bool(ld["flags"] & LINE_FLAG_IMPASSABLE))
            seg_type = SEG_SWITCH if structurally_solid else SEG_TRIGGER
            door_group = remote_line_group[seg["ld"]]
            required_key = group_required_key[door_group]
        elif seg["ld"] in line_door_group:
            seg_type = SEG_DOOR
            door_group = line_door_group[seg["ld"]]
            required_key = group_required_key[door_group]
            if door_group in plain_door_groups:
                flags |= SEG_FLAG_PLAIN_DOOR
            if ld["special"] in DIRECT_DOOR_SPECIALS:
                flags |= SEG_FLAG_DIRECT_USE
        elif seg["ld"] in window_lines:
            band = window_band(seg)
            if band is not None:
                # door_group / required_key are meaningless for a non-door seg,
                # so the band rides in them rather than widening BspSeg past 16
                # bytes -- see the note in src/bsp/bsp_map.h. A window whose
                # band degenerates stays a plain WALL, exactly as today.
                seg_type = SEG_WINDOW
                door_group, required_key = band
        elif seg["ld"] in sky_wall_lines:
            seg_type = SEG_SKY_WALL
            sector = sectors[sky_wall_lines[seg["ld"]]]
            wall_height = max(0, min(
                WORLD_WALL_HEIGHT, sector["ceiling"] - sector["floor"]))
            # Reuse a non-door field for the sky band above the low wall. Q8
            # keeps the runtime independent of sector heights while preserving
            # the exact 80/120-unit WAD spans (96/16 respectively for a
            # 128-unit engine slab).
            door_group = ((WORLD_WALL_HEIGHT - wall_height) * 256) // \
                WORLD_WALL_HEIGHT

        name = FALLBACK_TEXTURE
        xoff = seg["offset"]
        yoff = 0
        if front_side != 0xFFFF:
            side = sidedefs[front_side]
            xoff += side["xoff"]
            yoff = side["yoff"]
            side_ids = [front_side]
            if back_side != 0xFFFF:
                side_ids.append(back_side)
            fields = ("middle", "upper", "lower") if seg_type in (SEG_DOOR, SEG_SWITCH) \
                else ("middle", "lower", "upper")
            for side_id in side_ids:
                candidate_side = sidedefs[side_id]
                for field in fields:
                    candidate = candidate_side[field]
                    if candidate and candidate != "-":
                        name = candidate
                        break
                if name != FALLBACK_TEXTURE:
                    break
        if transfer is not None:
            # The destination is already a solid one-sided wall. Preserve its
            # offsets and replace only the material; source line 50 remains an
            # open height transition and emits no SEG.
            target_side = sidedefs[transfer.target_side_id]
            name = transfer.recipe.source.texture_name
            xoff = seg["offset"] + target_side["xoff"]
            yoff = target_side["yoff"]
        return seg_type, name, xoff, yoff, door_group, required_key, flags

    # --- Rebuild subsectors with only flat solid/interactive segs. ---------- #
    out_segs = []       # dicts with geometry, exact texture name, offsets and type
    out_ssectors = []   # (first_seg, count)
    out_ssector_sectors = []
    texture_usage = Counter()

    for (count, first) in ssectors:
        source_seg = segs[first] if count else None
        source_side = front_side_for(source_seg) if source_seg is not None else 0xFFFF
        sector_id = sidedefs[source_side]["sector"] if source_side != 0xFFFF else 0
        out_ssector_sectors.append(sector_id)
        start = len(out_segs)
        for k in range(count):
            seg = segs[first + k]
            if not line_solid_without_recipe(seg["ld"], linedefs[seg["ld"]]):
                continue
            ax, ay = vertices[seg["v1"]]
            bx, by = vertices[seg["v2"]]
            if ax == bx and ay == by:
                continue  # degenerate seg
            nx, ny = front_normal(seg)
            if nx == 0 and ny == 0:
                continue  # degenerate linedef
            stype, texture_name, tex_u_offset, tex_v_offset, door_group, required_key, flags = \
                seg_type_and_visual(seg)
            texture_usage[texture_name] += 1
            out_segs.append(dict(v1=seg["v1"], v2=seg["v2"],
                                nx=nx, ny=ny, texture_name=texture_name,
                                source_linedef=seg["ld"],
                                curated_material=seg["ld"] in material_transfers,
                                tex_u_offset=tex_u_offset, tex_v_offset=tex_v_offset,
                                type=stype, door_group=door_group,
                                required_key=required_key, flags=flags))
        out_ssectors.append((start, len(out_segs) - start))

    if len(out_segs) > 2048:
        raise SystemExit("solid seg count %d exceeds BSP_MAX_SEGS (2048)"
                         % len(out_segs))
    if len(nodes) > 640:
        raise SystemExit("node count %d exceeds BSP_MAX_NODES (640)" % len(nodes))

    # --- THINGS / Player 1 start. ------------------------------------------- #
    # Convert every THING into engine y-down coordinates.  The runtime owns the
    # deliberately-curated mapping to billboards/items, so unsupported types can
    # be reported and ignored without throwing away source-map fidelity.
    out_things = []
    start_x = start_y = 0
    start_angle_deg = 0
    for i in range(len(things_raw) // 10):
        tx, ty, tang, ttype, tflags = struct.unpack_from("<hhHHH", things_raw, i * 10)
        out_things.append((tx, -ty, ttype, (256 - (round(tang * 256 / 360) & 255)) & 255, tflags))
        if ttype == 1 and start_x == 0 and start_y == 0:
            start_x, start_y, start_angle_deg = tx, ty, tang
    # Y-down flip: negate the start Y and mirror the angle about the x-axis.
    start_y = -start_y
    start_angle = (256 - (round(start_angle_deg * 256 / 360) & 255)) & 255
    supported_things = sum(1 for _, _, thing_type, _, _ in out_things
                           if thing_type in RUNTIME_THING_TYPES)

    try:
        certificate = certify_flat_progression(
            vertices, out_segs, out_things, start_x, start_y)
    except ValueError as error:
        unsupported = sorted({ld["special"] for ld in linedefs
                              if ld["special"] in TELEPORT_SPECIALS})
        mechanics = []
        if unsupported:
            mechanics.append("teleport specials %s" % unsupported)
        if mapn in BOSS_TRIGGER_MAPS:
            mechanics.append("boss-death trigger")
        suffix = "; unsupported mandatory mechanics present: %s" % \
            ", ".join(mechanics) if mechanics else ""
        raise SystemExit("Map %s cannot be certified: %s%s" %
                         (mapn, error, suffix))

    # Resolve any blank face from another face in the same physical door group.
    group_textures = {}
    for seg in out_segs:
        if seg["type"] == SEG_DOOR and seg["texture_name"] != FALLBACK_TEXTURE:
            group_textures.setdefault(seg["door_group"], seg["texture_name"])
    for seg in out_segs:
        if seg["type"] == SEG_DOOR and seg["texture_name"] == FALLBACK_TEXTURE:
            replacement = group_textures.get(seg["door_group"])
            if replacement:
                texture_usage[FALLBACK_TEXTURE] -= 1
                texture_usage[replacement] += 1
                seg["texture_name"] = replacement

    required_key_mask = KEY_NONE
    for key_mask in group_required_key:
        required_key_mask |= key_mask
    door_face_counts = Counter(seg["door_group"] for seg in out_segs
                               if seg["type"] == SEG_DOOR)
    fallback_door_faces = sum(1 for seg in out_segs
                              if seg["type"] == SEG_DOOR and
                              seg["texture_name"] == FALLBACK_TEXTURE)
    curated_material_segs = sum(1 for seg in out_segs
                                if seg["curated_material"])
    curated_material_reports = []
    grouped_transfers = {}
    for target_id, transfer in sorted(material_transfers.items()):
        key = (transfer.recipe.name, transfer.source_linedef)
        grouped_transfers.setdefault(key, []).append(target_id)
    for (name, source_id), target_ids in grouped_transfers.items():
        transfer = material_transfers[target_ids[0]]
        curated_material_reports.append(dict(
            name=name,
            source_linedef=source_id,
            source_linedef_hint=transfer.recipe.source.linedef_hint,
            target_linedefs=target_ids,
            target_linedef_hints=[target.linedef_hint
                                  for target in transfer.recipe.targets],
            texture=transfer.recipe.source.texture_name,
            retextured_segs=sum(1 for seg in out_segs
                                if seg["source_linedef"] in target_ids),
            added_segs=0,
        ))
    baseline_seg_count = len(out_segs)

    return MapData(
        mapn=mapn,
        vertices=vertices,
        sectors=sectors,
        linedefs=linedefs,
        automap_lines=automap_lines,
        linedef_automap_indices=linedef_automap_indices,
        source_seg_count=len(segs),
        out_segs=out_segs,
        out_ssectors=out_ssectors,
        out_ssector_sectors=out_ssector_sectors,
        nodes=nodes,
        out_things=out_things,
        supported_things=supported_things,
        start_x=start_x,
        start_y=start_y,
        start_angle=start_angle,
        start_angle_deg=start_angle_deg,
        texture_usage=texture_usage,
        next_door_group=next_door_group,
        door_face_counts=door_face_counts,
        fallback_door_faces=fallback_door_faces,
        wad_sha256=hashlib.sha256(wad.data).hexdigest().upper(),
        baseline_seg_count=baseline_seg_count,
        curated_material_linedefs=sorted(material_transfers),
        curated_material_segs=curated_material_segs,
        curated_material_reports=curated_material_reports,
        required_key_mask=required_key_mask,
        certificate=certificate,
    )
