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
from doom_map import (KEY_NONE, SEG_DOOR, SEG_EXIT, SEG_WALL, SEG_FLAG_DIRECT_USE,
                      certify_flat_progression, load_map, point_segment_dist2,
                      runtime_things)

EVENT_INTERACTION = 0x08
EVENT_LOCKED = 0x20
EVENT_UNLOCKED = 0x40
EVENT_COMBAT_HIT = 0x04
EVENT_EXIT = 0x80
USE_RADIUS = 256
USE_ARRIVAL_RADIUS = 48
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


def use_target(map_data, x, y, aim_x, aim_y):
    """Offline counterpart of bsp_use_in_front's probes and tie-break.

    The runtime's deliberate 1.1839 trig gain is 303/256 at this resolution.
    This model checks all +/-3 heading steps accepted by the runner, so an
    emitted waypoint cannot be merely close to a useful surface: it must
    select the declared target at the runner's exact aligned heading.
    """
    declared = []
    base = use_angle(x, y, aim_x, aim_y)
    for angle in (base,):
        best = None
        for dist in range(128, 513, 128):
            px = x + ((fixed_cos(angle) * dist) >> 8)
            py = y + ((fixed_sin(angle) * dist) >> 8)
            for index, seg in enumerate(map_data.out_segs):
                if seg["type"] == SEG_WALL or (seg["type"] == SEG_DOOR and
                   not (seg.get("flags", 0) & SEG_FLAG_DIRECT_USE)):
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
            # Score the pose against the runner's finite arrival tolerance.
            # The aim point remains fixed while the actual player can land a
            # few pixels off the certified cell, so reject poses whose probe
            # ordering changes under the cardinal edge samples.
            robust = 0
            for ox, oy in ((0, 0), (32, 0), (-32, 0), (0, 32), (0, -32)):
                try:
                    if use_target(map_data, x + ox, y + oy, *aim)[1] == expected_target:
                        robust += 1
                except AssertionError:
                    pass
            surface_distance = point_segment_dist2(ax, ay, bx, by, x, y)
            candidates.append((abs(node_index - index), aim_rank, -robust,
                               surface_distance, (node_index - index) ** 2,
                               x, y, aim, action, target))
    if not candidates:
        raise AssertionError("no stable certified use pose for target %d" % expected_target)
    _, _, _, _, _, x, y, aim, action, target = min(candidates)
    return x, y, aim, action, target


def route_lines(map_data):
    certificate = certify_flat_progression(
        map_data.vertices, map_data.out_segs, map_data.out_things,
        map_data.start_x, map_data.start_y, capture_route=True,
        collision_radius_override=E2E_CLEARANCE_RADIUS)
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

    key_index = next((index for index, node in enumerate(nodes)
                      if node["action"] == "key"), None)

    # A red-lock scenario must exercise its physical door on both sides of the
    # pickup.  Locate concrete certified positions close enough to use it;
    # this does not change the certified movement path or bypass collision.
    keyed = [seg for seg in map_data.out_segs
             if seg["type"] == SEG_DOOR and seg["required_key"] != KEY_NONE]
    red = keyed[0] if keyed else None
    injected = {}
    if red is not None:
        target = midpoint(map_data.vertices, red)
        assert key_index is not None, "keyed map certificate never collected a key"
        before = nearest_index(nodes, target, 0, key_index)
        after = nearest_index(nodes, target, key_index, len(nodes))
        assert point_segment_dist2(*map_data.vertices[red["v1"]],
                                   *map_data.vertices[red["v2"]],
                                   nodes[before]["x"], nodes[before]["y"]) <= USE_RADIUS ** 2
        assert point_segment_dist2(*map_data.vertices[red["v1"]],
                                   *map_data.vertices[red["v2"]],
                                   nodes[after]["x"], nodes[after]["y"]) <= USE_RADIUS ** 2
        injected[before] = ("USE", target, EVENT_LOCKED)
        injected[after] = ("USE", target, EVENT_UNLOCKED)

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

        if index in injected:
            action, _, event = injected[index]
            aim = closest_point(map_data.vertices, red, x, y)
            x, y, aim, expected_action, expected_target = stable_use_pose(
                map_data, nodes, index, red)
            required_action = 2 if event == EVENT_LOCKED else 3
            assert expected_target == red["door_group"] and expected_action == 2
            # The static selector sees a locked door; after the key the runtime
            # changes only the action result, never the target identity.
            emit(x, y, aim[0], aim[1], USE_ARRIVAL_RADIUS, action, event,
                 required_action, expected_target)
        if index == combat_index:
            # E1M2 needs a narrow standoff so the player cannot overshoot its
            # portal-edge shot; E1M1's certified target is farther across the
            # room and retains the legacy 160-unit combat radius.
            fire_radius = 48 if map_data.mapn == "E1M2" else 160
            emit(x, y, combat_target[0], combat_target[1], fire_radius, "FIRE",
                 EVENT_COMBAT_HIT, -1, -1, 3600)
            emit(x, y, x, y, 160, "HURT", 0, -1, -1, 3600)
        if node["action"] == "use":
            x, y, aim, expected_action, expected_target = stable_use_pose(
                map_data, nodes, index, node["detail"])
            emit(x, y, aim[0], aim[1], USE_ARRIVAL_RADIUS, "USE", EVENT_INTERACTION,
                 expected_action, expected_target)

    exits = [seg for seg in map_data.out_segs if seg["type"] == SEG_EXIT]
    assert len(exits) == 1, "%s needs exactly one certified exit" % map_data.mapn
    end = nodes[-1]
    exit_x, exit_y, exit_aim, expected_action, expected_target = stable_use_pose(
        map_data, nodes, len(nodes) - 1, exits[0])
    assert expected_action == 4
    emit(exit_x, exit_y, exit_aim[0], exit_aim[1], USE_ARRIVAL_RADIUS,
         "USE", EVENT_EXIT, expected_action, expected_target, 3600)
    emit(exit_x, exit_y, exit_x, exit_y, MOVE_ARRIVAL_RADIUS, "EXIT", EVENT_EXIT, -1, -1, 3600)
    return "\n".join(lines) + "\n"


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
