#!/usr/bin/env python3
"""Emit pose-driven full-playthrough routes from the flat-map certificate.

The generated files are deliberately build artifacts.  Their source of truth
is the same geometry proof that gates map conversion, so a new campaign map
cannot gain a hand-timed route that quietly diverges from its certified path.
"""
import argparse
import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from wad_reader import WadFile
from doom_map import (KEY_NONE, SEG_DOOR, SEG_EXIT, SEG_SWITCH, SEG_TRIGGER,
                      SEG_WALL, SEG_FLAG_DIRECT_USE, certify_flat_progression,
                      load_map, point_segment_dist2, runtime_things)

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
EVENT_EXIT = 0x80
USE_RADIUS = 256
USE_ARRIVAL_RADIUS = 48
# waypoint_turn's dead-band for a USE press, in 256ths of a circle.
USE_AIM_SPREAD = 2
# Momentum can leave the player a few pixels outside a corner cell even after
# the controller has released thrust.  MOVE is a navigation tolerance, not an
# interaction tolerance; keep USE narrow while allowing the next certified
# cell to take over without stalling on the diagonal edge of its radius.
MOVE_ARRIVAL_RADIUS = 80
MOVE_CORNER_RADIUS = 32
ROUTE_SAMPLE_STEP = 64
# Keep the certified route outside the player's collision footprint at turns;
# this is deliberately wider than the map proof's default point sample.
E2E_CLEARANCE_RADIUS = 20
COMBAT_THINGS = {3001, 3002, 3003, 3004, 3005, 3006, 3007, 3008, 58}


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


def use_target(map_data, x, y, aim_x, aim_y, spread=0):
    """Offline counterpart of bsp_use_in_front's probes and tie-break.

    The runtime's deliberate 1.1839 trig gain is 303/256 at this resolution.
    This model checks all +/-3 heading steps accepted by the runner, so an
    emitted waypoint cannot be merely close to a useful surface: it must
    select the declared target at the runner's exact aligned heading.
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
                a = map_data.vertices[seg["v1"]]
                b = map_data.vertices[seg["v2"]]
                distance2 = point_segment_dist2(*a, *b, px, py)
                if distance2 >= USE_RADIUS ** 2:
                    continue
                candidate = (distance2, dist, index, seg)
                if best is None or candidate[:2] < best[:2]:
                    best = candidate
        if best is None:
            raise AssertionError("no runtime use target at %d,%d angle %d" % (x, y, angle))
        _, _, index, seg = best
        action = 4 if seg["type"] == SEG_EXIT else (2 if seg["required_key"] else 1)
        target = 0 if seg["type"] == SEG_EXIT else seg["door_group"]
        declared.append((action, target))
    if len(set(declared)) != 1:
        raise AssertionError("ambiguous use pose at %d,%d: %r" % (x, y, declared))
    return declared[0]


def stable_use_pose(map_data, nodes, index, seg):
    """Pick a certified path cell whose whole runner aim tolerance hits seg."""
    expected_target = 0 if seg["type"] == SEG_EXIT else seg["door_group"]
    candidates = []
    # The runtime selection is a ray probe, not a nearest-segment query.  The
    # closest point on a door can therefore point at an adjacent door (the
    # exact E1M2 regression).  Try several points on the declared SEG so the
    # generated pose can express the intended aim while remaining at the
    # certified path node.
    ax, ay = map_data.vertices[seg["v1"]]
    bx, by = map_data.vertices[seg["v2"]]
    for node_index, node in enumerate(nodes):
        x, y = node["x"], node["y"]
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
                action, target = use_target(map_data, x, y, *aim)
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
                        if use_target(map_data, x + ox, y + oy, *aim,
                                      spread=(USE_AIM_SPREAD if sample_index == 0
                                              else 0))[1] != expected_target:
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
    if not candidates:
        raise AssertionError("no stable certified use pose for target %d" % expected_target)
    (_, _, _, _, _, _, x, y, aim, action, target, stable_radius,
     pose_index) = min(candidates)
    return x, y, aim, action, target, stable_radius, pose_index


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
    nodes = compact

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

    def groups_at(x, y):
        groups = 0
        for seg in map_data.out_segs:
            if seg["type"] != SEG_DOOR:
                continue
            if point_segment_dist2(*map_data.vertices[seg["v1"]],
                                   *map_data.vertices[seg["v2"]], x, y) <                     E2E_CLEARANCE_RADIUS ** 2:
                groups |= 1 << seg["door_group"]
        return groups

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
        crossing = groups_at(node["x"], node["y"]) & ~opened
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
                        pose = stable_use_pose(map_data, nodes, back, control)
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

    enemies = [(x, y) for x, y, thing_type, _, _ in runtime_things(map_data.out_things)
               if thing_type in COMBAT_THINGS]
    assert enemies, "%s has no combat target for E2E" % map_data.mapn
    # Do not stand on top of the monster: billboard targeting intentionally
    # rejects objects inside its minimum projection depth.  Prefer a nearby
    # certified cell with a real point-blank gap, then fall back to the old
    # nearest pair only for maps whose combat thing is unusually embedded in
    # the navigation path.
    combat_candidates = [
        (node, enemy) for node in nodes for enemy in enemies
        if 96 ** 2 <= (node["x"] - enemy[0]) ** 2 +
                      (node["y"] - enemy[1]) ** 2 <= 256 ** 2]
    # Avoid firing at the first nearby billboard immediately after spawn.  In
    # E1M2 that pose is beside a portal edge and the centre ray can be
    # occluded even though the cell itself is reachable.  Prefer a later
    # certified standoff (the reference shot is 1008,-720 -> 912,-720), while
    # retaining a fallback for very small maps with no later combat cell.
    if map_data.mapn == "E1M2":
        late_combat = [pair for pair in combat_candidates
                       if nodes.index(pair[0]) >= len(nodes) // 5]
        combat_candidates = late_combat or combat_candidates
    combat_node, combat_target = min(
        combat_candidates or ((node, enemy) for node in nodes for enemy in enemies),
        key=lambda pair: (pair[0]["x"] - pair[1][0]) ** 2 +
                         (pair[0]["y"] - pair[1][1]) ** 2)
    combat_index = nodes.index(combat_node)

    cert_uses = {}
    for index, node in enumerate(nodes):
        if node["action"] != "use":
            continue
        pose = stable_use_pose(map_data, nodes, index, node["detail"])
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
            move_radius = (USE_ARRIVAL_RADIUS if needs_position else
                           (MOVE_CORNER_RADIUS if corner else MOVE_ARRIVAL_RADIUS))
            emit(x, y, x, y, move_radius, "MOVE")

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
            assert expected_target == door["door_group"] and expected_action == 2
            # The static selector sees a locked door; after the key the runtime
            # changes only the action result, never the target identity.
            emit(x, y, aim[0], aim[1], radius, action, event,
                 required_action, expected_target)
        if index == combat_index:
            # E1M2 needs a narrow standoff so the player cannot overshoot its
            # portal-edge shot; E1M1's certified target is farther across the
            # room and retains the legacy 160-unit combat radius.
            fire_radius = 48 if map_data.mapn == "E1M2" else 160
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
    return "\n".join(clearance_radii(map_data, clamp_arrival_radii(lines))) + "\n"


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
