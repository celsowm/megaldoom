#!/usr/bin/env python3
"""Emit pose-driven full-playthrough routes from the flat-map certificate.

The generated files are deliberately build artifacts.  Their source of truth
is the same geometry proof that gates map conversion, so a new campaign map
cannot gain a hand-timed route that quietly diverges from its certified path.
"""
import argparse
import math
import re
import sys
from collections import deque
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from wad_reader import WadFile
from doom_map import (BLOCKING_THING_RADIUS, DOOM_THING_NOT_SINGLE_PLAYER,
                      DOOM_THING_SKILL_MEDIUM, KEY_NONE, KEY_THING_MASK,
                      PICKUP_RADIUS, SEG_DOOR, SEG_EXIT, SEG_SWITCH, SEG_TRIGGER,
                      SEG_WALL, SEG_FLAG_DIRECT_USE, certify_flat_progression,
                      load_map, point_segment_dist2)

# Doom special 11 is the normal exit switch; 51 is the secret exit.  E1M3 ships
# both (linedef 982 at 736,1760 and linedef 785 at -1024,1536).  The campaign
# route must certify normal progression, and the runtime cannot tell them apart
# on its own -- bsp_use_in_front reports target 0 for any SEG_EXIT -- so the
# choice has to be made here, from the originating linedef.
NORMAL_EXIT_SPECIAL = 11

EVENT_INTERACTION = 0x08
EVENT_LOCKED = 0x20
EVENT_UNLOCKED = 0x40
EVENT_COMBAT_HIT = 0x04
EVENT_KEY = 0x10
EVENT_EXIT = 0x80
USE_RADIUS = 256
USE_ARRIVAL_RADIUS = 48
# waypoint_turn's dead-band for a USE press, in 256ths of a circle. Must
# match the runtime's USE tolerance (waypoint_turn in megaldoom_runner.c):
# a tighter runtime band looked like the fix for a wrong-surface mispress,
# but the turn controller can overshoot a 2-unit window every frame and
# hunt forever without ever pressing C, so the runtime keeps travel's wide
# 8-unit band for USE too. The accuracy has to come from here instead: pick
# a pose whose ray resolves to the intended surface across that whole band.
USE_AIM_SPREAD = 8
# Momentum can leave the player a few pixels outside a corner cell even after
# the controller has released thrust.  MOVE is a navigation tolerance, not an
# interaction tolerance; keep USE narrow while allowing the next certified
# cell to take over without stalling on the diagonal edge of its radius.
MOVE_ARRIVAL_RADIUS = 80
MOVE_CORNER_RADIUS = 32
# billboard_collect_near's touch radius (BILLBOARD_COLLECT_RADIUS) is 128, and
# the certifier's own PICKUP_RADIUS in doom_map.py matches it -- but the
# certified cell it records can already be sitting right at that 128-unit
# edge, so the arrival radius has to be the tightest the MOVE contract allows
# to avoid stopping short of the real pickup range.
KEY_ARRIVAL_RADIUS = 16
ROUTE_SAMPLE_STEP = 64
# Keep the certified route outside the player's collision footprint at turns;
# this is deliberately wider than the map proof's default point sample.
E2E_CLEARANCE_RADIUS = 20
COMBAT_THINGS = {3001, 3002, 3003, 3004, 3005, 3006, 3007, 3008, 58}
# An explosive barrel. Preferred over a monster as the FIRE target: it never
# walks off its spawn column and monster infighting cannot remove it before the
# follower arrives -- both of which happened to E1M2's certified enemy target,
# stalling waypoint 390 for 85% of the run's frame budget. src/main.c marks
# combat_hit on a direct barrel detonation, so a barrel satisfies the waypoint.
BARREL_THING = 2035
# The runtime's own billboard spawn cap (src/bsp/generated_map_limits.h). It
# must NOT be read from doom_map.runtime_things: that helper carries a stale
# hardcoded 112, which silently drops every one of E1M2's 24 barrels (they sit
# past the 112th qualifying thing in table order) and is why the barrel-target
# search below found nothing on the first attempt. The campaign maps all have
# fewer things than this, so in practice no target is ever capped away.
MAX_ACTIVE_THINGS = int(re.search(
    r"MEGALDOOM_MAP_MAX_ACTIVE_THINGS\s+(\d+)",
    (ROOT / "src" / "bsp" / "generated_map_limits.h").read_text()).group(1))


def spawnable_things(out_things):
    """Things the runtime actually spawns: same skill filter as billboard_init,
    the real cap, and no type restriction (barrels included)."""
    kept = []
    for x, y, thing_type, angle, flags in out_things:
        if not (flags & DOOM_THING_SKILL_MEDIUM):
            continue
        if flags & DOOM_THING_NOT_SINGLE_PLAYER:
            continue
        kept.append((x, y, thing_type, angle, flags))
        if len(kept) >= MAX_ACTIVE_THINGS:
            break
    return kept


def midpoint(vertices, seg):
    ax, ay = vertices[seg["v1"]]
    bx, by = vertices[seg["v2"]]
    return ((ax + bx) // 2, (ay + by) // 2)


def closest_point(vertices, seg, x, y):
    ax, ay = vertices[seg["v1"]]
    bx, by = vertices[seg["v2"]]
    dx, dy = bx - ax, by - ay
    length2 = dx * dx + dy * dy
    if not length2:
        return ax, ay
    dot = (x - ax) * dx + (y - ay) * dy
    dot = max(0, min(length2, dot))
    return (ax + (dx * dot) // length2, ay + (dy * dot) // length2)


def nearest_index(nodes, target, begin, end):
    return min(range(begin, end), key=lambda index:
               (nodes[index]["x"] - target[0]) ** 2 +
               (nodes[index]["y"] - target[1]) ** 2)


def _side(ax, ay, bx, by, px, py):
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax)


def segments_intersect(p1, p2, p3, p4):
    """True if the closed segments p1-p2 and p3-p4 cross or touch.

    Used to catch a MOVE step that crosses a door's face between two
    certified points even when the arrival point itself lands past the
    20-unit clearance radius the point-distance check uses. E1M3 put an
    unlock press's MOVE at (-1184,2352), 24 units south of door group 14's
    face at y=2376-2392 -- outside that radius, so nothing flagged it, and
    the door stayed shut in front of a follower walking straight down from
    y=2408 into a wall it could never open from the far side it was aimed at.
    """
    d1 = _side(*p3, *p4, *p1)
    d2 = _side(*p3, *p4, *p2)
    d3 = _side(*p1, *p2, *p3)
    d4 = _side(*p1, *p2, *p4)
    if ((d1 > 0 and d2 < 0) or (d1 < 0 and d2 > 0)) and \
       ((d3 > 0 and d4 < 0) or (d3 < 0 and d4 > 0)):
        return True
    # Touching/collinear cases: only relevant here for axis-aligned faces,
    # so a simple bounding-box overlap after confirming collinearity suffices.
    def on_segment(a, b, c):
        return (min(a[0], b[0]) <= c[0] <= max(a[0], b[0]) and
                min(a[1], b[1]) <= c[1] <= max(a[1], b[1]))
    if d1 == 0 and on_segment(p3, p4, p1):
        return True
    if d2 == 0 and on_segment(p3, p4, p2):
        return True
    if d3 == 0 and on_segment(p1, p2, p3):
        return True
    if d4 == 0 and on_segment(p1, p2, p4):
        return True
    return False


def use_angle(x, y, aim_x, aim_y):
    return round(math.atan2(aim_y - y, aim_x - x) * 256 / (2 * math.pi)) & 255


def fixed_sin(angle):
    """Bit-for-bit model of src/fixed_math.c's DEBUG trig table."""
    angle &= 255
    quadrant, local = divmod(angle, 64)
    def quarter(a):
        x = (a * 256) // 64
        x2 = (x * x) >> 8
        x3 = (x2 * x) >> 8
        x5 = (x3 * x2) >> 8
        return (479 * x - 196 * x3 + 24 * x5) >> 8
    if quadrant == 0:
        return quarter(local)
    if quadrant == 1:
        return quarter(63 - local)
    if quadrant == 2:
        return -quarter(local)
    return -quarter(63 - local)


def fixed_cos(angle):
    return fixed_sin(angle + 64)


USE_SIGHT_STANDOFF = 4  # bsp_map.c BSP_USE_SIGHT_STANDOFF


def _cross(ox, oy, px, py, qx, qy):
    return (px - ox) * (qy - oy) - (py - oy) * (qx - ox)


def runtime_closest_point(vertices, seg, px, py):
    """bsp_map.c seg_closest_point, including bsp_ratio_q8's floored Q8
    projection -- not the exact rational one closest_point computes. The
    runtime's use selection and its sight-ray endpoint both go through this."""
    ax, ay = vertices[seg["v1"]]
    bx, by = vertices[seg["v2"]]
    abx, aby = bx - ax, by - ay
    ab2 = abx * abx + aby * aby
    if ab2 <= 0:
        return ax, ay
    dot = (px - ax) * abx + (py - ay) * aby
    if dot <= 0:
        return ax, ay
    if dot >= ab2:
        return bx, by
    tq = (dot * 256) // ab2
    return ax + ((abx * tq) >> 8), ay + ((aby * tq) >> 8)


def runtime_dist2(vertices, seg, px, py):
    cx, cy = runtime_closest_point(vertices, seg, px, py)
    return (px - cx) ** 2 + (py - cy) ** 2


SIGHT_CELL = 128
# One map per generator process; (map_data, cell -> blocking lines, sight memo).
_sight_index = None


def _sight_state(map_data):
    global _sight_index
    if _sight_index is None or _sight_index[0] is not map_data:
        grid = {}
        for other in map_data.out_segs:
            # bsp_seg_is_open: a trigger never blocks, a door only while its
            # group is fully open.  Doors stay in the index, tagged with their
            # group, so each query can apply the door state at that press.
            if other["type"] == SEG_TRIGGER:
                continue
            x0, y0 = map_data.vertices[other["v1"]]
            x1, y1 = map_data.vertices[other["v2"]]
            group = other["door_group"] if other["type"] == SEG_DOOR else None
            line = (x0, y0, x1, y1, group)
            for cx in range(min(x0, x1) // SIGHT_CELL, max(x0, x1) // SIGHT_CELL + 1):
                for cy in range(min(y0, y1) // SIGHT_CELL, max(y0, y1) // SIGHT_CELL + 1):
                    grid.setdefault((cx, cy), []).append(line)
        _sight_index = (map_data, grid, {})
    return _sight_index[1], _sight_index[2]


def _sight_blockers(map_data, x, y, ex, ey):
    """(crosses a solid wall, door groups crossed) for the ray x,y -> ex,ey."""
    grid, memo = _sight_state(map_data)
    key = (x, y, ex, ey)
    cached = memo.get(key)
    if cached is not None:
        return cached
    solid = False
    doors = set()
    seen = set()
    for cx in range(min(x, ex) // SIGHT_CELL, max(x, ex) // SIGHT_CELL + 1):
        for cy in range(min(y, ey) // SIGHT_CELL, max(y, ey) // SIGHT_CELL + 1):
            for line in grid.get((cx, cy), ()):
                if line in seen:
                    continue
                seen.add(line)
                bx0, by0, bx1, by1, group = line
                if (_cross(x, y, ex, ey, bx0, by0) * _cross(x, y, ex, ey, bx1, by1) < 0 and
                        _cross(bx0, by0, bx1, by1, x, y) * _cross(bx0, by0, bx1, by1, ex, ey) < 0):
                    if group is None:
                        solid = True
                        break
                    doors.add(group)
            if solid:
                break
        if solid:
            break
    memo[key] = (solid, frozenset(doors))
    return memo[key]


def use_surface_visible(map_data, seg, x, y, probe_x, probe_y, open_groups=frozenset()):
    """bsp_map.c use_surface_visible: front side, and nothing between the
    player and the probed point that bsp_seg_is_open would call shut.

    A shut door blocks this ray exactly like a wall.  This used to model every
    door as open ("the route presses them open before it looks through them"),
    and that is not what the runtime does: E1M2's door group 2 is pressed from
    (-832,-400), where its one DIRECT_USE face (-960,-272)-(-832,-272) is only
    reachable through the same door's other face 16 units nearer.  Offline the
    press resolved to group 2 at 41 units; in the ROM that nearer face is shut,
    group 2 is not visible, and the runtime picked the red door (group 7) 221
    units away -- the live "expected=1:2 got=3:7" nobody could reproduce while
    the model and the ROM agreed on every other input.  open_groups is the set
    of door groups the route has opened by the time of this press."""
    ax, ay = map_data.vertices[seg["v1"]]
    if (x - ax) * seg["nx"] + (y - ay) * seg["ny"] <= 0:
        return False
    cx, cy = runtime_closest_point(map_data.vertices, seg, probe_x, probe_y)
    normal_max = max(abs(seg["nx"]), abs(seg["ny"]))
    if normal_max:
        # C integer division truncates toward zero.
        cx += int(seg["nx"] * USE_SIGHT_STANDOFF / normal_max)
        cy += int(seg["ny"] * USE_SIGHT_STANDOFF / normal_max)
    solid, doors = _sight_blockers(map_data, x, y, cx, cy)
    return not solid and doors <= open_groups


def use_target(map_data, x, y, aim_x, aim_y, owned_keys=0, spread=0,
               open_groups=frozenset()):
    """Offline counterpart of bsp_use_in_front's probes and tie-break.

    The runtime's deliberate 1.1839 trig gain is 303/256 at this resolution.
    This model checks all +/-3 heading steps accepted by the runner, so an
    emitted waypoint cannot be merely close to a useful surface: it must
    select the declared target at the runner's exact aligned heading.

    owned_keys must be the keys the player actually holds by this point in
    the route, not just whether the target ever needs one: bsp_use_in_front
    reports LOCKED only while the key is still missing and UNLOCKED once it
    is held, and a door crossed twice -- once before its key, once after --
    is a genuine live route (E1M2's red door, 2026-09-12: the certified path
    presses door group 7 again after already holding its key, and the old
    static "requires a key -> LOCKED" guess reported LOCKED both times,
    mismatching the runtime's own UNLOCKED on the second press).
    """
    declared = []
    base = use_angle(x, y, aim_x, aim_y)
    # The runner presses once its heading is within the USE dead-band of the
    # aim, not at the exact angle, so every heading it may press at has to
    # resolve to the same surface.  Perturbing heading and position separately
    # rather than as a product keeps this affordable: each probe walks every
    # SEG four times.
    angles = [base]
    for step in range(1, spread + 1):
        angles.append((base - step) & 255)
        angles.append((base + step) & 255)
    for angle in angles:
        best = None
        for dist in range(128, 513, 128):
            px = x + ((fixed_cos(angle) * dist) >> 8)
            py = y + ((fixed_sin(angle) * dist) >> 8)
            for index, seg in enumerate(map_data.out_segs):
                # Mirror bsp_use_in_front exactly: only these four answer to
                # use.  Modelling windows as candidates made this generator
                # predict poses the runtime resolves differently.
                if not (seg["type"] in (SEG_EXIT, SEG_SWITCH, SEG_TRIGGER) or
                        (seg["type"] == SEG_DOOR and
                         seg.get("flags", 0) & SEG_FLAG_DIRECT_USE)):
                    continue
                distance2 = runtime_dist2(map_data.vertices, seg, px, py)
                if distance2 >= USE_RADIUS ** 2:
                    continue
                candidate = (distance2, dist, index, seg)
                if ((best is None or candidate[:2] < best[:2]) and
                        use_surface_visible(map_data, seg, x, y, px, py,
                                            open_groups)):
                    best = candidate
        if best is None:
            raise AssertionError("no runtime use target at %d,%d angle %d" % (x, y, angle))
        _, _, index, seg = best
        if seg["type"] == SEG_EXIT:
            action = 4
        elif not seg["required_key"]:
            action = 1
        elif (owned_keys & seg["required_key"]) != seg["required_key"]:
            action = 2
        else:
            action = 3
        target = 0 if seg["type"] == SEG_EXIT else seg["door_group"]
        declared.append((action, target))
    if len(set(declared)) != 1:
        raise AssertionError("ambiguous use pose at %d,%d: %r" % (x, y, declared))
    return declared[0]


def door_groups_at(map_data, x, y, from_x=None, from_y=None):
    """Bitmask of the door groups a certified node touches."""
    groups = 0
    for seg in map_data.out_segs:
        if seg["type"] != SEG_DOOR:
            continue
        v1 = map_data.vertices[seg["v1"]]
        v2 = map_data.vertices[seg["v2"]]
        hit = point_segment_dist2(*v1, *v2, x, y) < E2E_CLEARANCE_RADIUS ** 2
        # The point check alone misses a node that lands past the door's
        # face outside that radius but only reaches there by crossing it:
        # E1M3 put an unlock press's MOVE at (-1184,2352), 24 units south
        # of group 14's face at y=2376-2392, and nothing flagged it, so
        # the door stayed shut in front of a follower that could never
        # walk through it to press from the far side the route named.
        if not hit and from_x is not None and \
                segments_intersect((from_x, from_y), (x, y), v1, v2):
            hit = True
        if hit:
            groups |= 1 << seg["door_group"]
    return groups


_open_groups_memo = None


def open_groups_by_index(map_data, nodes):
    """Door groups already open when the route stands at each node.

    route_lines presses every door group before the path first touches it and
    presses each group once (toggle_door would shut it again), so a group the
    path touched at any earlier node is open from there on.  A group pressed
    ahead of its first touch is still counted shut in that gap; the replay in
    verify_use_replay checks every press against the exact door state."""
    global _open_groups_memo
    if (_open_groups_memo is None or _open_groups_memo[0] is not map_data or
            _open_groups_memo[1] is not nodes):
        result = []
        touched = 0
        previous = None
        for node in nodes:
            result.append(frozenset(group for group in range(touched.bit_length())
                                    if touched >> group & 1))
            touched |= door_groups_at(map_data, node["x"], node["y"],
                                      previous["x"] if previous else None,
                                      previous["y"] if previous else None)
            previous = node
        _open_groups_memo = (map_data, nodes, result)
    return _open_groups_memo[2]


def use_target_across_doors(map_data, x, y, aim_x, aim_y, owned_keys=0, spread=0,
                            door_states=(frozenset(),)):
    """use_target, required to agree under every door state in door_states."""
    results = {use_target(map_data, x, y, aim_x, aim_y, owned_keys, spread=spread,
                          open_groups=state)
               for state in door_states}
    if len(results) != 1:
        raise AssertionError("door-state dependent use pose at %d,%d: %r" %
                             (x, y, sorted(results)))
    return results.pop()


def stable_use_pose(map_data, nodes, index, seg, before=None, first_only=False):
    """Pick a certified path cell whose whole runner aim tolerance hits seg."""
    expected_target = 0 if seg["type"] == SEG_EXIT else seg["door_group"]
    # The group being pressed is shut until this very press opens it.
    pressed_group = frozenset() if seg["type"] == SEG_EXIT else frozenset({seg["door_group"]})
    # Which other doors are open at the press is only settled once every pose
    # is chosen and the presses are ordered, so a pose has to hold both ways:
    # with only the doors the path has already walked through open, and with
    # every other door open too.  E1M4 is why the estimate alone is not
    # enough: its group 7 press stands right after the group 2 press, before
    # the path touches door 2, and with that door really open one heading in
    # the aim spread resolved to group 2's far face.  verify_use_replay then
    # checks the chosen poses against the exact state.
    every_group = frozenset(other["door_group"] for other in map_data.out_segs
                            if other["type"] == SEG_DOOR) - pressed_group
    door_states_by_index = [(state - pressed_group, every_group)
                            for state in open_groups_by_index(map_data, nodes)]
    candidates = []
    # The runtime selection is a ray probe, not a nearest-segment query.  The
    # closest point on a door can therefore point at an adjacent door (the
    # exact E1M2 regression).  Try several points on the declared SEG so the
    # generated pose can express the intended aim while remaining at the
    # certified path node.
    ax, ay = map_data.vertices[seg["v1"]]
    bx, by = map_data.vertices[seg["v2"]]
    # Keys accumulate monotonically along the certified path; a node that IS a
    # key pickup already holds it (the player collects on arrival, before any
    # press from that same spot).
    owned_keys_by_index = []
    owned = 0
    for node in nodes:
        if node["action"] == "key":
            owned |= node["detail"]
        owned_keys_by_index.append(owned)
    for node_index, node in enumerate(nodes):
        x, y = node["x"], node["y"]
        # A press that opens a door has to stand on the near side of it.  The
        # door-aware model made E1M2's group 2 resolvable only from its far
        # face, and the pose landed at (-928,-256) -- past the shut door the
        # follower then walked into for 6000 ticks.  `before` is the first
        # node that touches the door, so no candidate reaches it through it.
        if before is not None and node_index >= before:
            continue
        owned_keys = owned_keys_by_index[node_index]
        if point_segment_dist2(*map_data.vertices[seg["v1"]],
                               *map_data.vertices[seg["v2"]], x, y) > USE_RADIUS ** 2:
            continue
        closest = closest_point(map_data.vertices, seg, x, y)
        # Prefer the endpoint farthest from the exit panel.  At the E1M1
        # EXITDOOR the midpoint ray is valid in the offline replica but the
        # runtime's wider probe can still see the adjacent exit SEG.
        exit_seg = next((candidate for candidate in map_data.out_segs
                         if candidate["type"] == SEG_EXIT), None)
        if exit_seg is not None:
            ex, ey = midpoint(map_data.vertices, exit_seg)
            endpoint_order = sorted(((ax, ay), (bx, by)),
                                    key=lambda point:
                                    -((point[0] - ex) ** 2 +
                                      (point[1] - ey) ** 2))
        else:
            endpoint_order = [(ax, ay), (bx, by)]
        aim_points = endpoint_order + [
                      ((ax + bx) // 2, (ay + by) // 2), closest,
                      ((3 * ax + bx) // 4, (3 * ay + by) // 4),
                      ((ax + 3 * bx) // 4, (ay + 3 * by) // 4)]
        for aim_rank, aim in enumerate(aim_points):
            # A pose on the segment itself yields a zero-length aim vector;
            # the runtime then probes whichever neighboring door is first.
            if (aim[0] - x) ** 2 + (aim[1] - y) ** 2 < 32 ** 2:
                continue
            try:
                action, target = use_target_across_doors(
                    map_data, x, y, *aim, owned_keys,
                    door_states=door_states_by_index[node_index])
            except AssertionError:
                continue
            if target != expected_target:
                continue
            # The aim point is fixed but the player only has to land inside
            # USE_ARRIVAL_RADIUS of the certified cell, so the probe has to
            # resolve to the declared target from anywhere in that disc -- not
            # merely at its centre.  Scoring this as a preference was not
            # enough: E1M3 picked a pose that selected door group 9 at the
            # exact cell and group 8 from where the follower actually stopped,
            # 24 units away, and the run died on the first press.  Require it
            # across the disc instead of ranking by it.
            # Find the largest arrival tolerance over which this pose still
            # resolves to the declared target, and emit that as the waypoint's
            # radius.  Scoring stability as a mere preference let E1M3 pick a
            # pose that selected door group 9 at the exact cell and group 8
            # from where the follower actually stopped 24 units away, killing
            # the run on its first press.  Demanding full stability at the
            # fixed 48 instead dragged poses hundreds of units off the path.
            # Choosing the radius per press keeps the pose on the path and
            # makes the tolerance it promises true.
            stable_radius = 0
            for edge in (USE_ARRIVAL_RADIUS, 40, 32, 24, 16):
                corner_offset = int(edge * 0.7071)
                samples = ((0, 0), (edge, 0), (-edge, 0), (0, edge), (0, -edge),
                           (corner_offset, corner_offset),
                           (corner_offset, -corner_offset),
                           (-corner_offset, corner_offset),
                           (-corner_offset, -corner_offset))
                good = True
                for sample_index, (ox, oy) in enumerate(samples):
                    try:
                        # Only the centre carries the heading spread; the edge
                        # samples already vary the angle by moving the origin.
                        if use_target_across_doors(
                                map_data, x + ox, y + oy, *aim, owned_keys,
                                spread=(USE_AIM_SPREAD if sample_index == 0 else 0),
                                door_states=door_states_by_index[node_index]
                        )[1] != expected_target:
                            good = False
                            break
                    except AssertionError:
                        good = False
                        break
                if good:
                    stable_radius = edge
                    break
            if not stable_radius:
                continue
            robust = stable_radius
            surface_distance = point_segment_dist2(ax, ay, bx, by, x, y)
            # Rank by how far the pose is in WORLD space from the cell it
            # stands in for, not by how far apart their indices are: a node
            # twenty steps along the path can be a long walk away, and the
            # route contract caps the gap between consecutive waypoints.
            detour = ((x - nodes[index]["x"]) ** 2 +
                      (y - nodes[index]["y"]) ** 2)
            candidates.append((detour, abs(node_index - index), aim_rank, -robust,
                               surface_distance, (node_index - index) ** 2,
                               x, y, aim, action, target, stable_radius,
                               node_index))
            if first_only:
                # Existence is all add_door_control_detours asks; ranking every
                # candidate would double the generator's slowest pass.
                return x, y, aim, action, target, stable_radius, node_index
    if not candidates:
        raise AssertionError("no stable certified use pose for target %d" % expected_target)
    (_, _, _, _, _, _, x, y, aim, action, target, stable_radius,
     pose_index) = min(candidates)
    return x, y, aim, action, target, stable_radius, pose_index


# A locked-door press before the key used to fall out of the certified path for
# free, because that path walked past the door on its way to the key. Once use
# stopped working through walls (2026-09-11) the certificate reached E1M2's and
# E1M3's keys without ever passing within USE_RADIUS of their locked doors (913
# and 597 units), and the scenario silently vanished. add_lock_detours walks the
# route out to the door and back instead.
NAV_STEP = 16
# Stand this close to press the locked door: the same witness distance the
# certificate uses, so the press is the square-on one a player would make.
LOCK_DETOUR_REACH = 128
# Stay this far beyond PICKUP_RADIUS from every key while detouring, or the
# follower could pick the key up on the way and the "locked" press would unlock.
LOCK_DETOUR_KEY_MARGIN = 32
LOCK_DETOUR_MAX_CELLS = 200000
LOCK_DETOUR_POSE_TRIES = 24


def _compact_grid_path(cells):
    """Keep every turn and at least one cell per ROUTE_SAMPLE_STEP of a 4-neighbour
    grid path -- the shape route_lines compacts the certified path to."""
    if len(cells) <= 2:
        return list(cells)
    kept = [cells[0]]
    for index in range(1, len(cells) - 1):
        px, py = cells[index - 1]
        x, y = cells[index]
        nx, ny = cells[index + 1]
        turn = (x - px, y - py) != (nx - x, ny - y)
        far = abs(x - kept[-1][0]) + abs(y - kept[-1][1]) >= ROUTE_SAMPLE_STEP
        if turn or far:
            kept.append(cells[index])
    kept.append(cells[-1])
    return kept


def _press_detour(map_data, nodes, key_index, faces, expected_action=2):
    """Insert a walk from the pre-key path to a cell that presses `faces` and back.

    Movement is a breadth-first search over the certificate's 16-unit grid with
    four-neighbour steps (the controller cannot cut corners), keeping
    E2E_CLEARANCE_RADIUS from every solid SEG -- every door counts as shut, so the
    detour never needs a press of its own -- and from blocking things, and
    staying out of pickup range of every key. Candidate press cells are tried in
    walking order and kept only when stable_use_pose resolves the locked door
    from them, exactly as the emitted press will.
    """
    vertices = map_data.vertices
    group = faces[0]["door_group"]
    cell_size = SIGHT_CELL
    grid = {}
    for seg in map_data.out_segs:
        if seg["type"] == SEG_TRIGGER:
            continue
        ax, ay = vertices[seg["v1"]]
        bx, by = vertices[seg["v2"]]
        line = (ax, ay, bx, by)
        pad = E2E_CLEARANCE_RADIUS
        for cx in range((min(ax, bx) - pad) // cell_size, (max(ax, bx) + pad) // cell_size + 1):
            for cy in range((min(ay, by) - pad) // cell_size, (max(ay, by) + pad) // cell_size + 1):
                grid.setdefault((cx, cy), []).append(line)
    things = list(spawnable_things(map_data.out_things))
    blockers = [(x, y, BLOCKING_THING_RADIUS[thing_type])
                for x, y, thing_type, _, _ in things if thing_type in BLOCKING_THING_RADIUS]
    keys = [(x, y) for x, y, thing_type, _, _ in things if thing_type in KEY_THING_MASK]
    key_clearance2 = (PICKUP_RADIUS + LOCK_DETOUR_KEY_MARGIN) ** 2
    memo = {}

    def free(x, y):
        cached = memo.get((x, y))
        if cached is not None:
            return cached
        ok = True
        for ax, ay, bx, by in grid.get((x // cell_size, y // cell_size), ()):
            if point_segment_dist2(ax, ay, bx, by, x, y) < E2E_CLEARANCE_RADIUS ** 2:
                ok = False
                break
        if ok:
            ok = all((x - ox) ** 2 + (y - oy) ** 2 >= (E2E_CLEARANCE_RADIUS + radius) ** 2
                     for ox, oy, radius in blockers)
        if ok:
            ok = all((x - kx) ** 2 + (y - ky) ** 2 >= key_clearance2 for kx, ky in keys)
        memo[(x, y)] = ok
        return ok

    def presses(x, y):
        for face in faces:
            ax, ay = vertices[face["v1"]]
            bx, by = vertices[face["v2"]]
            if point_segment_dist2(ax, ay, bx, by, x, y) > LOCK_DETOUR_REACH ** 2:
                continue
            if (x - ax) * face["nx"] + (y - ay) * face["ny"] <= 0:
                continue
            cx, cy = closest_point(vertices, face, x, y)
            normal_max = max(abs(face["nx"]), abs(face["ny"]))
            if normal_max:
                cx += int(face["nx"] * USE_SIGHT_STANDOFF / normal_max)
                cy += int(face["ny"] * USE_SIGHT_STANDOFF / normal_max)
            # Every door counts as shut here too: the detour opens nothing.
            solid, doors = _sight_blockers(map_data, x, y, cx, cy)
            if not solid and not doors:
                return True
        return False

    # Multi-source: whichever pre-key cell is the shortest walk from the door.
    origin = {}
    parent = {}
    queue = deque()
    for index in range(key_index):
        cell = (nodes[index]["x"], nodes[index]["y"])
        if cell not in parent:
            parent[cell] = None
            origin[cell] = index
            queue.append(cell)
    tries = 0
    while queue and len(parent) < LOCK_DETOUR_MAX_CELLS and tries < LOCK_DETOUR_POSE_TRIES:
        cell = queue.popleft()
        if parent[cell] is not None and presses(*cell):
            tries += 1
            path = [cell]
            while parent[path[-1]] is not None:
                path.append(parent[path[-1]])
            path.reverse()
            source = origin[path[0]]
            outbound = _compact_grid_path(path)
            inbound = _compact_grid_path(list(reversed(path)))
            detour = ([dict(x=x, y=y, action="move", detail=None) for x, y in outbound[1:]] +
                      [dict(x=x, y=y, action="move", detail=None) for x, y in inbound[1:]])
            candidate = nodes[:source + 1] + detour + nodes[source + 1:]
            goal_index = source + len(outbound) - 1
            try:
                pose = stable_use_pose(map_data, candidate, goal_index, faces[0])
            except AssertionError:
                pose = None
            if (pose is not None and pose[4] == group and pose[3] == expected_action and
                    source < pose[6] <= source + len(detour)):
                return candidate
        for dx, dy in ((NAV_STEP, 0), (-NAV_STEP, 0), (0, NAV_STEP), (0, -NAV_STEP)):
            step = (cell[0] + dx, cell[1] + dy)
            if step in parent or not free(*step):
                continue
            parent[step] = cell
            origin[step] = origin[cell]
            queue.append(step)
    raise AssertionError("%s: no reachable, stable press for door group %d "
                         "before node %d" % (map_data.mapn, group, key_index))


def add_lock_detours(map_data, nodes):
    """Make every collected key's locked door reachable for a press before the key.

    Mirrors the selection in route_lines' lock-scenario block: the door is the
    first SEG carrying that key, and a detour is added only where the certified
    path never comes within USE_RADIUS of it before the pickup."""
    faces_by_key = {}
    for seg in map_data.out_segs:
        if seg["type"] == SEG_DOOR and seg["required_key"] != KEY_NONE:
            faces_by_key.setdefault(seg["required_key"], []).append(seg)
    for required_key in sorted(faces_by_key):
        key_index = next((index for index, node in enumerate(nodes)
                          if node["action"] == "key" and node["detail"] & required_key),
                         None)
        if key_index is None:
            continue
        door = faces_by_key[required_key][0]
        before = nearest_index(nodes, midpoint(map_data.vertices, door), 0, key_index)
        if point_segment_dist2(*map_data.vertices[door["v1"]], *map_data.vertices[door["v2"]],
                               nodes[before]["x"], nodes[before]["y"]) <= USE_RADIUS ** 2:
            continue
        faces = [seg for seg in faces_by_key[required_key]
                 if seg["door_group"] == door["door_group"]]
        nodes = _press_detour(map_data, nodes, key_index, faces)
    return nodes

def key_pickup_legs(map_data, node):
    """Walk in from a key's certified cell until the pickup cannot be missed.

    The certificate records the first cell its flood fill finds within
    PICKUP_RADIUS of a key and turns back there, so that cell can sit exactly
    on the 128-unit edge -- and the follower is only promised to stop within
    KEY_ARRIVAL_RADIUS of it.  E1M3's blue key at (-160,864) certifies from
    (-160,736), 128 away with 93 units of open floor around it; live, the
    follower called itself arrived a few units short, billboard_collect_near
    never fired, and the blue door answered LOCKED to the unlock press 400
    waypoints later.  Step straight toward the key through free space (every
    door counted shut, blocking things kept clear) until even a
    KEY_ARRIVAL_RADIUS miss stays inside the pickup radius, then come back to
    the certified cell the rest of the route starts from."""
    x, y = node["x"], node["y"]
    keys = [(kx, ky) for kx, ky, thing_type, _, _ in spawnable_things(map_data.out_things)
            if KEY_THING_MASK.get(thing_type, 0) & node["detail"]]
    assert keys, "%s: key node at %d,%d has no key thing" % (map_data.mapn, x, y)
    kx, ky = min(keys, key=lambda key: (key[0] - x) ** 2 + (key[1] - y) ** 2)
    distance = math.hypot(kx - x, ky - y)
    target = PICKUP_RADIUS - 2 * KEY_ARRIVAL_RADIUS
    if distance <= target:
        return []
    solid = [seg for seg in map_data.out_segs if seg["type"] != SEG_TRIGGER]
    blockers = [(bx, by, BLOCKING_THING_RADIUS[thing_type])
                for bx, by, thing_type, _, _ in spawnable_things(map_data.out_things)
                if thing_type in BLOCKING_THING_RADIUS]

    def clear(px, py):
        if any((px - bx) ** 2 + (py - by) ** 2 < (E2E_CLEARANCE_RADIUS + radius) ** 2
               for bx, by, radius in blockers):
            return False
        return all(point_segment_dist2(*map_data.vertices[seg["v1"]],
                                       *map_data.vertices[seg["v2"]], px, py)
                   >= E2E_CLEARANCE_RADIUS ** 2 for seg in solid)

    best = None
    for step in range(4, int(distance) + 1, 4):
        px = x + round((kx - x) * step / distance)
        py = y + round((ky - y) * step / distance)
        if not clear(px, py):
            break
        best = (px, py)
        if math.hypot(kx - px, ky - py) <= target:
            break
    assert best is not None and         math.hypot(kx - best[0], ky - best[1]) <= PICKUP_RADIUS - KEY_ARRIVAL_RADIUS, (
            "%s: no clear approach puts the key at %d,%d safely in pickup range "
            "from %d,%d" % (map_data.mapn, kx, ky, x, y))
    return [best, (x, y)]


def add_door_control_detours(map_data, nodes):
    """Walk out to a control for any door the path cannot open from its near side.

    E1M2's door group 2 certifies from (-832,-400), where the witness ray to the
    door's far face grazes the corner of its shut near face.  That press only
    resolves at one exact heading -- across the runner's USE dead-band some
    headings reach the red door instead -- so no stable pose opens the door
    before the path walks into it.  The group also has a switch at
    (-592,-1088), 384 units off the path.  Detour to a control that can be
    pressed stably before the door, the way a locked door's press is reached
    before its key.  Keyed doors keep their own locked/unlocked pair."""
    keyed = {seg["door_group"] for seg in map_data.out_segs
             if seg["type"] == SEG_DOOR and seg["required_key"] != KEY_NONE}
    controls = {}
    for seg in map_data.out_segs:
        if seg["type"] in (SEG_SWITCH, SEG_TRIGGER) or (
                seg["type"] == SEG_DOOR and seg["flags"] & SEG_FLAG_DIRECT_USE):
            controls.setdefault(seg["door_group"], []).append(seg)
    settled = set()
    while True:
        first_touch = {}
        previous = None
        for index, node in enumerate(nodes):
            touched = door_groups_at(map_data, node["x"], node["y"],
                                     previous["x"] if previous else None,
                                     previous["y"] if previous else None)
            while touched:
                group = (touched & -touched).bit_length() - 1
                touched &= touched - 1
                first_touch.setdefault(group, index)
            previous = node
        missing = None
        for group, before in sorted(first_touch.items(), key=lambda item: item[1]):
            if group in keyed or group in settled:
                continue
            for control in controls.get(group, ()):
                try:
                    stable_use_pose(map_data, nodes, max(0, before - 1), control,
                                    before=before, first_only=True)
                except AssertionError:
                    continue
                settled.add(group)
                break
            if group not in settled:
                missing = (group, before)
                break
        if missing is None:
            return nodes
        group, before = missing
        for control in sorted(controls.get(group, ()),
                              key=lambda seg: seg["type"] != SEG_SWITCH):
            try:
                nodes = _press_detour(map_data, nodes, before, [control],
                                      expected_action=1)
            except AssertionError:
                continue
            break
        else:
            raise AssertionError("%s: door group %d is crossed at node %d and no "
                                 "control can be pressed stably before it" %
                                 (map_data.mapn, group, before))
        settled.add(group)


def route_lines(map_data):
    normal_exits = {index for index, seg in enumerate(map_data.out_segs)
                    if seg["type"] == SEG_EXIT and
                    map_data.linedefs[seg["source_linedef"]]["special"]
                    == NORMAL_EXIT_SPECIAL}
    assert normal_exits, "%s has no normal exit switch" % map_data.mapn
    certificate = certify_flat_progression(
        map_data.vertices, map_data.out_segs, map_data.out_things,
        map_data.start_x, map_data.start_y, capture_route=True,
        collision_radius_override=E2E_CLEARANCE_RADIUS,
        exit_seg_indices=normal_exits)
    nodes = certificate["route"]
    assert nodes and nodes[0]["action"] == "start"

    # Compress only straight runs. Every retained segment is a contiguous
    # axis-aligned prefix of the certified path; turns remain explicit so the
    # runner cannot cut a corner or invent a shortcut.
    compact = [nodes[0]]
    previous_move = nodes[0]
    previous_direction = None
    for node in nodes[1:]:
        if node["action"] != "move":
            compact.append(node)
            previous_move = node
            previous_direction = None
            continue
        dx = node["x"] - previous_move["x"]
        dy = node["y"] - previous_move["y"]
        direction = (0 if dx == 0 else (1 if dx > 0 else -1),
                     0 if dy == 0 else (1 if dy > 0 else -1))
        if previous_direction is not None and direction != previous_direction:
            compact.append(previous_move)
        if (previous_direction is None or direction != previous_direction or
                abs(node["x"] - compact[-1]["x"]) +
                abs(node["y"] - compact[-1]["y"]) >= ROUTE_SAMPLE_STEP):
            compact.append(node)
        previous_move = node
        previous_direction = direction
    if compact[-1] is not nodes[-1]:
        compact.append(nodes[-1])
    nodes = add_door_control_detours(map_data, add_lock_detours(map_data, compact))

    # A locked-door scenario must exercise its physical door on both sides of
    # the key pickup.  Locate concrete certified positions close enough to use
    # it; this does not change the certified movement path or bypass collision.
    #
    # A map can hold locked doors its certified route never needs.  This engine
    # is flat: doom_map.line_solid_without_recipe deliberately turns every
    # two-sided height transition into open floor, so a level whose progression
    # was gated by stairs, lifts or ledges flattens into one whose exit is
    # reachable without any key at all.  E1M3 is exactly that -- 4304 units of
    # certified route against 2691 of straight line, both its keys sitting off
    # the path.  Asserting a key pickup here would demand a detour the
    # certificate does not make, so emit the pair only for a key the route
    # genuinely collects, and let a keyless map generate a keyless route.
    collected = [(index, node) for index, node in enumerate(nodes)
                 if node["action"] == "key"]
    doors = {}
    for seg in map_data.out_segs:
        if seg["type"] == SEG_DOOR and seg["required_key"] != KEY_NONE:
            doors.setdefault(seg["required_key"], seg)
    injected = {}
    for required_key, door in sorted(doors.items()):
        key_index = next((index for index, node in collected
                          if node["detail"] & required_key), None)
        if key_index is None:
            continue
        target = midpoint(map_data.vertices, door)
        before = nearest_index(nodes, target, 0, key_index)
        # The nearest certified node to the door, by raw distance, can lie
        # PAST it: the search that proved this route may have crossed this
        # exact door somewhere else in its exploration before ever walking
        # the reconstructed path through here, so "nearest" silently picks a
        # far-side cell.  E1M3 put the unlock press for group 14 at
        # (-1184,2352), 24 units south of its face at y=2376-2392, and the
        # follower could never walk there in the first place -- the door was
        # still shut in front of it.  Walk the path forward from the key
        # pickup instead and press at the certified node standing right
        # before the segment that actually steps across the door's face.
        after = None
        v1 = map_data.vertices[door["v1"]]
        v2 = map_data.vertices[door["v2"]]
        for index in range(key_index + 1, len(nodes)):
            if segments_intersect((nodes[index - 1]["x"], nodes[index - 1]["y"]),
                                  (nodes[index]["x"], nodes[index]["y"]), v1, v2):
                after = index - 1
                break
        if after is None:
            after = nearest_index(nodes, target, key_index, len(nodes))
        if point_segment_dist2(*map_data.vertices[door["v1"]],
                               *map_data.vertices[door["v2"]],
                               nodes[before]["x"],
                               nodes[before]["y"]) > USE_RADIUS ** 2:
            continue
        assert point_segment_dist2(*map_data.vertices[door["v1"]],
                                   *map_data.vertices[door["v2"]],
                                   nodes[after]["x"], nodes[after]["y"]) <= USE_RADIUS ** 2
        # Two door groups resolving to one certified cell would silently drop a
        # scenario: the second write would replace the first.
        assert before not in injected and after not in injected, (
            "%s: locked doors share a certified use cell" % map_data.mapn)
        injected[before] = ("USE", door, EVENT_LOCKED)
        injected[after] = ("USE", door, EVENT_UNLOCKED)

    # Every door the route walks through has to be opened by the player, and
    # the certificate only records a "use" node when a group first opens during
    # its search -- which is not always a node the reconstructed path descends
    # from.  E1M2 is the case that exposed it: the route crosses door group 3
    # at node 1405 with no use node anywhere for that group, so the follower
    # walked into a shut door at (-1408,-2080) and burned 6000 ticks against it
    # while the trace showed velocity pinned at 0.  The proof itself is sound
    # (replaying the route, no node ever enters a group it could not open from
    # where it stands); what is missing is the press.  Emit one for any group
    # the path crosses that nothing else in the route already opens.
    controls = {}
    for seg in map_data.out_segs:
        if seg["type"] in (SEG_SWITCH, SEG_TRIGGER) or (
                seg["type"] == SEG_DOOR and seg["flags"] & SEG_FLAG_DIRECT_USE):
            controls.setdefault(seg["door_group"], []).append(seg)

    def groups_at(x, y, from_x=None, from_y=None):
        return door_groups_at(map_data, x, y, from_x, from_y)

    # The keyed pair injected above already presses its own door twice.
    opened = 0
    for _, door, _ in injected.values():
        opened |= 1 << door["door_group"]
    openings = {}
    for index, node in enumerate(nodes):
        # Order matters: a group the certificate opens later in the path is
        # still shut here.  Counting every certificate use node as open from
        # the start hid E1M3's group 8, crossed at (-1360,1664) well before
        # its own press.
        if node["action"] == "use" and node["detail"] is not None:
            opened |= 1 << node["detail"]["door_group"]
        previous = nodes[index - 1] if index > 0 else None
        crossing = groups_at(node["x"], node["y"],
                             previous["x"] if previous else None,
                             previous["y"] if previous else None) & ~opened
        while crossing:
            group = (crossing & -crossing).bit_length() - 1
            crossing &= crossing - 1
            opened |= 1 << group
            placed = False
            for back in range(index, max(-1, index - 32), -1):
                # Never press from inside the door being opened: while it is
                # shut that cell is solid, so the follower could never stand
                # there to press.  This is what put E1M2's first attempt at
                # the group-3 switch at (-1408,-2080), inside the very door.
                if groups_at(nodes[back]["x"], nodes[back]["y"]) & (1 << group):
                    continue
                for control in controls.get(group, ()):
                    try:
                        pose = stable_use_pose(map_data, nodes, back, control,
                                               before=index)
                    except AssertionError:
                        continue
                    if pose[4] != group:
                        continue
                    # Emit the press where its pose stands, not where the
                    # crossing is: keying it to the crossing made the route
                    # jump 145 units off the path and back for E1M3's group 11.
                    openings.setdefault(min(pose[6], back), []).append(pose)
                    placed = True
                    break
                if placed:
                    break
            assert placed, ("%s: door group %d is crossed at node %d with no "
                            "reachable control to open it" %
                            (map_data.mapn, group, index))

    # (priority, x, y): a barrel is priority 0, a monster priority 1, so the
    # selection below takes a barrel whenever a certified standoff cell has one
    # in the point-blank band and only falls back to a monster where no map
    # geometry puts a barrel in reach.
    targets = []
    for x, y, thing_type, _, _ in spawnable_things(map_data.out_things):
        if thing_type == BARREL_THING:
            targets.append((0, x, y))
        elif thing_type in COMBAT_THINGS:
            targets.append((1, x, y))
    assert any(priority == 1 for priority, _, _ in targets), \
        "%s has no combat target for E2E" % map_data.mapn
    # Do not stand on top of the target: billboard targeting intentionally
    # rejects objects inside its minimum projection depth.  Prefer a nearby
    # certified cell with a real point-blank gap, then fall back to the old
    # nearest pair only for maps whose combat thing is unusually embedded in
    # the navigation path.
    combat_candidates = [
        (node, target) for node in nodes for target in targets
        if 96 ** 2 <= (node["x"] - target[1]) ** 2 +
                      (node["y"] - target[2]) ** 2 <= 256 ** 2]
    # Avoid firing at the first nearby billboard immediately after spawn.  In
    # E1M2 that pose is beside a portal edge and the centre ray can be
    # occluded even though the cell itself is reachable.  Prefer a later
    # certified standoff, while retaining a fallback for very small maps with
    # no later combat cell.
    if map_data.mapn == "E1M2":
        late_combat = [pair for pair in combat_candidates
                       if nodes.index(pair[0]) >= len(nodes) // 5]
        combat_candidates = late_combat or combat_candidates
    combat_node, combat_pick = min(
        combat_candidates or ((node, target) for node in nodes for target in targets),
        key=lambda pair: (pair[1][0],
                          (pair[0]["x"] - pair[1][1]) ** 2 +
                          (pair[0]["y"] - pair[1][2]) ** 2))
    combat_target = (combat_pick[1], combat_pick[2])
    combat_target_is_barrel = combat_pick[0] == 0
    combat_index = nodes.index(combat_node)

    # A certificate use node opens its group somewhere before the path walks
    # through that door, but the witness that proved it only has to see the
    # surface's closest point -- and that ray may graze a door vertex.  E1M2's
    # group 2 certifies from (-832,-400) through the corner of the shut face at
    # y=-288 onto its far face at y=-272; at any real heading the runtime's
    # aim point moves off that corner and the shut face blocks it, so the only
    # stable pose for that face was past the door, which the follower cannot
    # reach.  Press each group from before the path first touches it, trying
    # the certified face first and then any other control of the same group
    # (E1M2's group 2 also has a switch).
    first_touch = {}
    previous = None
    for index, node in enumerate(nodes):
        touched = groups_at(node["x"], node["y"],
                            previous["x"] if previous else None,
                            previous["y"] if previous else None)
        while touched:
            group = (touched & -touched).bit_length() - 1
            touched &= touched - 1
            first_touch.setdefault(group, index)
        previous = node
    injected_groups = {door["door_group"] for _, door, _ in injected.values()}
    cert_uses = {}
    for index, node in enumerate(nodes):
        if node["action"] != "use":
            continue
        certified = node["detail"]
        group = certified["door_group"]
        if certified["type"] != SEG_EXIT and group in injected_groups:
            continue
        before = None if certified["type"] == SEG_EXIT else first_touch.get(group)
        pose = None
        for control in [certified] + [other for other in controls.get(group, ())
                                      if other is not certified]:
            try:
                pose = stable_use_pose(map_data, nodes, index, control, before=before)
            except AssertionError:
                continue
            if pose[4] == group:
                break
            pose = None
        assert pose is not None, (
            "%s: no control opens door group %d from before node %s" %
            (map_data.mapn, group, before))
        cert_uses.setdefault(pose[6], []).append(pose)

    # bsp_map.c's toggle_door TOGGLES: pressing a group that is already open
    # shuts it again.  The certificate can name the same group more than once
    # (its search opens one under several key masks), and E1M3 emitted group 13
    # twice -- the second press closed the door the first had opened and the
    # follower walked into it at (-2384,1053) and timed out.  Press each group
    # once.  The keyed pair is the exception and is seeded here: its first
    # press is refused for want of the key, so it does not open anything.
    pressed = set()
    for _, door, _ in injected.values():
        pressed.add(door["door_group"])

    lines = ["# generated by tools/generate-e2e-routes.py; do not hand-time inputs",
             "# X Y AIM_X AIM_Y ARRIVAL_RADIUS ACTION EVENT_MASK EXPECTED_ACTION EXPECTED_TARGET TIMEOUT"]
    last = None
    def emit(x, y, aim_x, aim_y, radius, action, event=0, expected_action=-1,
             expected_target=-1, timeout=6000):
        nonlocal last
        line = (f"{x} {y} {aim_x} {aim_y} {radius} {action} {event:02x} "
                f"{expected_action} {expected_target} {timeout}")
        lines.append(line)
        last = (x, y)

    for index, node in enumerate(nodes):
        x, y = node["x"], node["y"]
        needs_position = node["action"] != "move" or index in injected or index == combat_index
        corner = False
        if 0 < index < len(nodes) - 1 and node["action"] == "move":
            before = (node["x"] - nodes[index - 1]["x"],
                      node["y"] - nodes[index - 1]["y"])
            after = (nodes[index + 1]["x"] - node["x"],
                     nodes[index + 1]["y"] - node["y"])
            corner = (before[0] == 0) != (after[0] == 0)
        # Preserve every certified grid cell.  Chord-compressing a path at a
        # corner can cut through a wall even though both endpoints are valid.
        # Reached cells advance in one host frame, so fidelity costs little.
        if last is None or needs_position or last != (x, y):
            if node["action"] == "key":
                # A key is picked up by proximity, not a use-ray, at whatever
                # cell the certifier's flood fill first came within
                # PICKUP_RADIUS (128) of it -- and that cell can be the worst
                # case, exactly 128 away, when the key sits somewhere the
                # flattened geometry never lets the path approach any closer
                # (E1M3's blue key: the corridor dead-ends at -160,736, 128
                # units from the key at -160,864, then backtracks). USE_
                # ARRIVAL_RADIUS's 48-unit slack let the follower call itself
                # arrived up to 48 units short of that cell -- as far as 176
                # from the key, well outside the real pickup radius -- so the
                # key silently never entered inventory and the door it opens
                # later reported LOCKED. Use the tightest radius the MOVE
                # contract allows instead.
                move_radius = KEY_ARRIVAL_RADIUS
            elif needs_position:
                move_radius = USE_ARRIVAL_RADIUS
            else:
                move_radius = MOVE_CORNER_RADIUS if corner else MOVE_ARRIVAL_RADIUS
            emit(x, y, x, y, move_radius, "MOVE")

        if node["action"] == "key":
            for leg_index, (leg_x, leg_y) in enumerate(key_pickup_legs(map_data, node)):
                # The inward leg carries the key bit so the runner's catch-up
                # never skips the pickup it exists for; the runner does not
                # otherwise read a MOVE's event mask.
                emit(leg_x, leg_y, leg_x, leg_y, KEY_ARRIVAL_RADIUS, "MOVE",
                     EVENT_KEY if leg_index == 0 else 0)
        for pose in openings.get(index, ()):
            ux, uy, aim, expected_action, expected_target, radius, _ = pose
            if expected_target in pressed:
                continue
            pressed.add(expected_target)
            emit(ux, uy, aim[0], aim[1], radius, "USE",
                 EVENT_INTERACTION, expected_action, expected_target)
        if index in injected:
            action, door, event = injected[index]
            x, y, aim, expected_action, expected_target, radius, _ = stable_use_pose(
                map_data, nodes, index, door)
            required_action = 2 if event == EVENT_LOCKED else 3
            # Same physical door on both sides of the key; only the result
            # changes, and stable_use_pose now models which one from the keys
            # held at the pose it picked.
            assert expected_target == door["door_group"] and expected_action == required_action, (
                "%s: %s press for door group %d resolves to action %d" %
                (map_data.mapn, "locked" if event == EVENT_LOCKED else "unlocked",
                 door["door_group"], expected_action))
            emit(x, y, aim[0], aim[1], radius, action, event,
                 required_action, expected_target)
        if index == combat_index:
            # The narrow E1M2 standoff existed to stop the follower overshooting
            # a portal-edge enemy shot. A barrel target is immovable, so that
            # risk is gone and the uniform radius applies; keep the tight one
            # only where we fell back to an enemy on E1M2.
            fire_radius = 160 if combat_target_is_barrel else \
                (48 if map_data.mapn == "E1M2" else 160)
            emit(x, y, combat_target[0], combat_target[1], fire_radius, "FIRE",
                 EVENT_COMBAT_HIT, -1, -1, 3600)
            emit(x, y, x, y, 160, "HURT", 0, -1, -1, 3600)
        for pose in cert_uses.get(index, ()):
            x, y, aim, expected_action, expected_target, radius, _ = pose
            if expected_target in pressed:
                continue
            pressed.add(expected_target)
            emit(x, y, aim[0], aim[1], radius, "USE", EVENT_INTERACTION,
                 expected_action, expected_target)

    exits = [map_data.out_segs[index] for index in sorted(normal_exits)]
    assert len(exits) == 1, ("%s needs exactly one certified normal exit, found %d"
                             % (map_data.mapn, len(exits)))
    end = nodes[-1]
    (exit_x, exit_y, exit_aim, expected_action, expected_target,
     exit_radius, _) = stable_use_pose(map_data, nodes, len(nodes) - 1, exits[0])
    assert expected_action == 4
    emit(exit_x, exit_y, exit_aim[0], exit_aim[1], exit_radius,
         "USE", EVENT_EXIT, expected_action, expected_target, 3600)
    emit(exit_x, exit_y, exit_x, exit_y, MOVE_ARRIVAL_RADIUS, "EXIT", EVENT_EXIT, -1, -1, 3600)
    lines = clearance_radii(map_data, clamp_arrival_radii(lines))
    verify_use_replay(map_data, lines)
    return "\n".join(lines) + "\n"


def verify_use_replay(map_data, lines):
    """Replay the emitted presses in order against the door state the runtime
    will actually hold at each one.

    stable_use_pose can only estimate that state (open_groups_by_index), since
    which press lands first is decided after every pose is chosen.  Here the
    order is final: bsp_map.c toggle_door flips a group on every TOGGLED or
    UNLOCKED press and nothing else moves a door, so the state is exact.  Each
    press must still resolve to its declared target across the arrival disc and
    aim spread it promises, or the generator fails here rather than in a live
    replay hundreds of thousands of frames in."""
    open_groups = frozenset()
    doors = [seg for seg in map_data.out_segs if seg["type"] == SEG_DOOR]
    previous = None
    for line in lines:
        if line.startswith("#"):
            continue
        fields = line.split(" ")
        # The follower walks to every row's position in order, so no leg may
        # cross a door face that is still shut at that point in the presses.
        position = (int(fields[0]), int(fields[1]))
        if previous is not None and previous != position:
            for door in doors:
                if door["door_group"] in open_groups:
                    continue
                if segments_intersect(previous, position,
                                      map_data.vertices[door["v1"]],
                                      map_data.vertices[door["v2"]]):
                    raise AssertionError(
                        "%s: leg %r -> '%s' crosses shut door group %d" %
                        (map_data.mapn, previous, line, door["door_group"]))
        previous = position
        if fields[5] != "USE":
            continue
        x, y, aim_x, aim_y, radius = (int(value) for value in fields[:5])
        expected_action, expected_target = int(fields[7]), int(fields[8])
        corner = int(radius * 0.7071)
        samples = ((0, 0), (radius, 0), (-radius, 0), (0, radius), (0, -radius),
                   (corner, corner), (corner, -corner),
                   (-corner, corner), (-corner, -corner))
        for sample_index, (ox, oy) in enumerate(samples):
            try:
                _, target = use_target(
                    map_data, x + ox, y + oy, aim_x, aim_y,
                    spread=(USE_AIM_SPREAD if sample_index == 0 else 0),
                    open_groups=open_groups)
            except AssertionError as error:
                raise AssertionError("%s: press '%s' with doors %s open: %s" %
                                     (map_data.mapn, line, sorted(open_groups),
                                      error)) from error
            assert target == expected_target, (
                "%s: press '%s' resolves to target %d at offset %d,%d with "
                "doors %s open" % (map_data.mapn, line, target, ox, oy,
                                   sorted(open_groups)))
        if expected_action in (1, 3):
            open_groups = open_groups ^ {expected_target}


def clearance_radii(map_data, lines):
    """Cap each MOVE's arrival circle at the free room around that cell.

    A certified cell can sit a few units from a wall or a shut door: the proof
    only needs E2E_CLEARANCE_RADIUS of margin, and the player's own collision
    radius is 16 of that.  Declaring a 48-unit arrival there lets the follower
    call itself arrived from anywhere in a disc that is mostly solid, stop
    steering, and drift into the geometry -- E1M3 wedged against its blue door
    at (-1152,2409), 19 units into a door face whose certified approach cell
    had four units of margin.  Keep the circle inside the free space.
    """
    solid = [seg for seg in map_data.out_segs if seg["type"] != SEG_TRIGGER]
    out = []
    for line in lines:
        if line.startswith("#"):
            out.append(line)
            continue
        fields = line.split(" ")
        if fields[5] != "MOVE":
            out.append(line)
            continue
        x, y = int(fields[0]), int(fields[1])
        room = min(point_segment_dist2(*map_data.vertices[seg["v1"]],
                                       *map_data.vertices[seg["v2"]], x, y)
                   for seg in solid) ** 0.5
        fields[4] = str(max(16, min(int(fields[4]), int(room))))
        out.append(" ".join(fields))
    return out


def clamp_arrival_radii(lines):
    """Make every MOVE waypoint be genuinely visited, not merely approached.

    Certified cells are 16 units apart but straight runs declare an 80-unit
    arrival radius, so a follower standing on one cell counts as arrived at
    the next three or four. It consumes them in consecutive frames and then
    steers at whatever survived. Through DOOM's diagonal passages -- which
    flatten to one-cell-wide staircases, e.g. the single free channel around
    E1M2's (-1392,-2080) -- that survivor is diagonally across a corner and
    the straight line to it runs through wall.

    Capping the radius at the distance the waypoint is actually away keeps
    the generous value on genuinely long runs, where nothing is skipped
    anyway, and restores the path's shape wherever the cells are close.
    """
    MIN_ARRIVAL_RADIUS = 16
    out = []
    previous = None
    for line in lines:
        if line.startswith("#"):
            out.append(line)
            continue
        fields = line.split(" ")
        x, y = int(fields[0]), int(fields[1])
        radius, action = int(fields[4]), fields[5]
        if action == "MOVE" and previous is not None:
            # Floor, never round: rounding 22.6 up to 23 makes the circle
            # reach a hair past the waypoint it was clamped against, which is
            # exactly the corner-cut this clamp exists to prevent.
            step = int(math.hypot(x - previous[0], y - previous[1]))
            if step > 0:
                fields[4] = str(max(MIN_ARRIVAL_RADIUS, min(radius, step)))
        previous = (x, y)
        out.append(" ".join(fields))
    return out

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--map", required=True, dest="map_name")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    generated = route_lines(load_map(WadFile(str(ROOT / "DOOM1.WAD")), args.map_name))
    if args.check:
        if not args.out.exists() or args.out.read_text() != generated:
            raise SystemExit("stale E2E route: %s" % args.out)
    else:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(generated, newline="\n")


if __name__ == "__main__":
    main()
