#!/usr/bin/env python3
"""Construct a conservative, executable route through a flattened Doom map.

Unlike the sector-level semantic check, this verifier searches actual world
coordinates with the MegalDoom player radius. A successful result is a concrete
sequence of collision-clear 16-unit moves that collects reusable coloured keys,
opens matching doors, reaches and kills every spawned target with sufficient
ammo, and finally approaches a runtime-emitted exit switch.
"""

from __future__ import annotations

from collections import defaultdict
from dataclasses import dataclass
import heapq
import importlib.util
import math
from pathlib import Path
import sys
from typing import Any, Callable, Iterable

MODULE_PATH = Path(__file__).with_name("wad-flat-playable.py")
SPEC = importlib.util.spec_from_file_location("wad_flat_playable_route_base", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot load {MODULE_PATH}")
base = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = base
SPEC.loader.exec_module(base)

PLAYER_RADIUS = 32
GRID_STEP = 16
COLLECT_RADIUS = 128
TARGET_APPROACH_RADIUS = 128
EXIT_APPROACH_RADIUS = 128
START_AMMO = 50
MAX_AMMO = 99
ENEMY_SHOTS = 3
MAX_RUNTIME_OBJECTS = 112
MAX_GRID_POINTS = 1_000_000
MAX_ASTAR_EXPANSIONS = 500_000
SPATIAL_CELL = 128

DOOM_THING_SKILL_MEDIUM = 0x0002
DOOM_THING_NOT_SINGLE_PLAYER = 0x0010
SUPPORTED_RUNTIME_THINGS = {
    5, 6, 13, 2014, 2015, 2011, 2012, 2018, 2019,
    2007, 2048, 2035, 9, 3001, 3004,
}
ENEMY_THINGS = {9, 3001, 3004}
AMMO_THINGS = {2007: 10, 2048: 20}
BLOCKING_PROPS = {2035: 20}


@dataclass(frozen=True)
class Segment:
    ax: int
    ay: int
    bx: int
    by: int
    linedef: int
    required_key_mask: int = 0


@dataclass(frozen=True)
class RuntimeThing:
    source_index: int
    x: int
    y: int
    thing_type: int
    flags: int


class SpatialSegments:
    def __init__(self, segments: Iterable[Segment], cell_size: int = SPATIAL_CELL):
        self.segments = list(segments)
        self.cell_size = cell_size
        self.cells: dict[tuple[int, int], list[int]] = defaultdict(list)
        for index, segment in enumerate(self.segments):
            x0 = min(segment.ax, segment.bx) // cell_size
            x1 = max(segment.ax, segment.bx) // cell_size
            y0 = min(segment.ay, segment.by) // cell_size
            y1 = max(segment.ay, segment.by) // cell_size
            for cy in range(y0, y1 + 1):
                for cx in range(x0, x1 + 1):
                    self.cells[(cx, cy)].append(index)

    def query(self, min_x: float, min_y: float, max_x: float, max_y: float):
        x0 = math.floor(min_x / self.cell_size)
        x1 = math.floor(max_x / self.cell_size)
        y0 = math.floor(min_y / self.cell_size)
        y1 = math.floor(max_y / self.cell_size)
        seen: set[int] = set()
        for cy in range(y0, y1 + 1):
            for cx in range(x0, x1 + 1):
                for index in self.cells.get((cx, cy), ()):
                    if index not in seen:
                        seen.add(index)
                        yield self.segments[index]


def runtime_thing_enabled(thing: dict[str, Any]) -> bool:
    return (
        thing["type"] in SUPPORTED_RUNTIME_THINGS
        and (thing["flags"] & DOOM_THING_SKILL_MEDIUM) != 0
        and (thing["flags"] & DOOM_THING_NOT_SINGLE_PLAYER) == 0
    )


def spawned_runtime_things(model: Any) -> list[RuntimeThing]:
    result: list[RuntimeThing] = []
    for index, thing in enumerate(model.things):
        if not runtime_thing_enabled(thing):
            continue
        result.append(RuntimeThing(index, thing["x"], thing["y"],
                                   thing["type"], thing["flags"]))
        if len(result) >= MAX_RUNTIME_OBJECTS:
            break
    return result


def suppress_unspawned_keys(model: Any) -> list[int]:
    spawned_key_indices = {
        thing.source_index for thing in spawned_runtime_things(model)
        if thing.thing_type in base.KEY_THINGS
    }
    suppressed: list[int] = []
    for index, thing in enumerate(model.things):
        if thing["type"] in base.KEY_THINGS and index not in spawned_key_indices:
            thing["type"] = 0
            suppressed.append(index)
    return suppressed


def point_segment_dist2(px: float, py: float, segment: Segment) -> float:
    abx = segment.bx - segment.ax
    aby = segment.by - segment.ay
    apx = px - segment.ax
    apy = py - segment.ay
    length2 = abx * abx + aby * aby
    if length2 <= 0:
        return apx * apx + apy * apy
    fraction = (apx * abx + apy * aby) / length2
    fraction = max(0.0, min(1.0, fraction))
    dx = px - (segment.ax + abx * fraction)
    dy = py - (segment.ay + aby * fraction)
    return dx * dx + dy * dy


def orientation(ax: float, ay: float, bx: float, by: float,
                cx: float, cy: float) -> float:
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax)


def on_segment(ax: float, ay: float, bx: float, by: float,
               px: float, py: float) -> bool:
    return (min(ax, bx) <= px <= max(ax, bx) and
            min(ay, by) <= py <= max(ay, by))


def segments_intersect(ax: float, ay: float, bx: float, by: float,
                       segment: Segment) -> bool:
    c1 = orientation(ax, ay, bx, by, segment.ax, segment.ay)
    c2 = orientation(ax, ay, bx, by, segment.bx, segment.by)
    c3 = orientation(segment.ax, segment.ay, segment.bx, segment.by, ax, ay)
    c4 = orientation(segment.ax, segment.ay, segment.bx, segment.by, bx, by)
    if ((c1 > 0 > c2 or c2 > 0 > c1) and
            (c3 > 0 > c4 or c4 > 0 > c3)):
        return True
    return (
        (c1 == 0 and on_segment(ax, ay, bx, by, segment.ax, segment.ay)) or
        (c2 == 0 and on_segment(ax, ay, bx, by, segment.bx, segment.by)) or
        (c3 == 0 and on_segment(segment.ax, segment.ay, segment.bx, segment.by, ax, ay)) or
        (c4 == 0 and on_segment(segment.ax, segment.ay, segment.bx, segment.by, bx, by))
    )


def segment_segment_dist2(ax: float, ay: float, bx: float, by: float,
                          segment: Segment) -> float:
    if segments_intersect(ax, ay, bx, by, segment):
        return 0.0
    edge = Segment(int(ax), int(ay), int(bx), int(by), -1)
    return min(
        point_segment_dist2(ax, ay, segment),
        point_segment_dist2(bx, by, segment),
        point_segment_dist2(segment.ax, segment.ay, edge),
        point_segment_dist2(segment.bx, segment.by, edge),
    )


def point_dist2(x: float, y: float, target_x: float, target_y: float) -> float:
    dx = x - target_x
    dy = y - target_y
    return dx * dx + dy * dy


def source_line_emitted(model: Any, linedef: dict[str, Any]) -> bool:
    if linedef["left"] == base.NO_SIDE or linedef["right"] == base.NO_SIDE:
        return True
    if linedef["flags"] & base.LINE_FLAG_IMPASSABLE:
        return True
    right = model.sectors[model.sidedefs[linedef["right"]]["sector"]]
    left = model.sectors[model.sidedefs[linedef["left"]]["sector"]]
    opening = min(right["ceiling"], left["ceiling"]) - max(
        right["floor"], left["floor"]
    )
    return opening <= 0


def build_collision_segments(model: Any):
    static: list[Segment] = []
    doors: list[Segment] = []
    exits: list[Segment] = []
    seen: set[tuple[int, int, int]] = set()
    for seg in model.segs:
        line_id = seg["linedef"]
        linedef = model.linedefs[line_id]
        if not source_line_emitted(model, linedef):
            continue
        first, second = sorted((seg["v1"], seg["v2"]))
        key = (line_id, first, second)
        if key in seen:
            continue
        seen.add(key)
        ax, ay = model.vertices[seg["v1"]]
        bx, by = model.vertices[seg["v2"]]
        if ax == bx and ay == by:
            continue
        special = linedef["special"]
        shape = Segment(ax, ay, bx, by, line_id,
                        base.key_mask_for_special(special))
        if special in base.EXIT_SPECIALS:
            static.append(shape)
            exits.append(shape)
        elif special in base.DOOR_SPECIALS:
            doors.append(shape)
        else:
            static.append(shape)
    return static, doors, exits


class NavigationGrid:
    def __init__(self, model: Any, start_x: int, start_y: int,
                 static_segments: list[Segment], door_segments: list[Segment],
                 props: list[tuple[int, int, int]]):
        self.model = model
        self.start_x = start_x
        self.start_y = start_y
        self.static_index = SpatialSegments(static_segments)
        self.door_index = SpatialSegments(door_segments)
        self.props = props
        self.point_cache: dict[tuple[int, int], bool] = {}
        self.edge_cache: dict[tuple[tuple[int, int], tuple[int, int]], int | None] = {}
        min_x = min(x for x, _ in model.vertices)
        max_x = max(x for x, _ in model.vertices)
        min_y = min(y for _, y in model.vertices)
        max_y = max(y for _, y in model.vertices)
        self.min_ix = math.ceil((min_x - start_x) / GRID_STEP)
        self.max_ix = math.floor((max_x - start_x) / GRID_STEP)
        self.min_iy = math.ceil((min_y - start_y) / GRID_STEP)
        self.max_iy = math.floor((max_y - start_y) / GRID_STEP)
        self.grid_points = ((self.max_ix - self.min_ix + 1) *
                            (self.max_iy - self.min_iy + 1))
        if self.grid_points <= 0 or self.grid_points > MAX_GRID_POINTS:
            raise ValueError(
                f"navigation grid has {self.grid_points} points; supported maximum is {MAX_GRID_POINTS}"
            )

    def world(self, node: tuple[int, int]) -> tuple[int, int]:
        return (self.start_x + node[0] * GRID_STEP,
                self.start_y + node[1] * GRID_STEP)

    def in_bounds(self, node: tuple[int, int]) -> bool:
        return (self.min_ix <= node[0] <= self.max_ix and
                self.min_iy <= node[1] <= self.max_iy)

    def point_valid(self, node: tuple[int, int]) -> bool:
        cached = self.point_cache.get(node)
        if cached is not None:
            return cached
        if not self.in_bounds(node):
            self.point_cache[node] = False
            return False
        x, y = self.world(node)
        radius2 = PLAYER_RADIUS * PLAYER_RADIUS
        valid = True
        for wall in self.static_index.query(
                x - PLAYER_RADIUS, y - PLAYER_RADIUS,
                x + PLAYER_RADIUS, y + PLAYER_RADIUS):
            if point_segment_dist2(x, y, wall) < radius2:
                valid = False
                break
        if valid:
            for prop_x, prop_y, prop_radius in self.props:
                limit = PLAYER_RADIUS + prop_radius
                if point_dist2(x, y, prop_x, prop_y) < limit * limit:
                    valid = False
                    break
        self.point_cache[node] = valid
        return valid

    def edge_requirement(self, first: tuple[int, int],
                         second: tuple[int, int]) -> int | None:
        key = (first, second) if first <= second else (second, first)
        if key in self.edge_cache:
            return self.edge_cache[key]
        if not self.point_valid(second):
            self.edge_cache[key] = None
            return None
        ax, ay = self.world(first)
        bx, by = self.world(second)
        radius2 = PLAYER_RADIUS * PLAYER_RADIUS
        for wall in self.static_index.query(
                min(ax, bx) - PLAYER_RADIUS, min(ay, by) - PLAYER_RADIUS,
                max(ax, bx) + PLAYER_RADIUS, max(ay, by) + PLAYER_RADIUS):
            if segment_segment_dist2(ax, ay, bx, by, wall) < radius2:
                self.edge_cache[key] = None
                return None
        for prop_x, prop_y, prop_radius in self.props:
            limit = PLAYER_RADIUS + prop_radius
            edge = Segment(ax, ay, bx, by, -1)
            if point_segment_dist2(prop_x, prop_y, edge) < limit * limit:
                self.edge_cache[key] = None
                return None
        required = 0
        for door in self.door_index.query(
                min(ax, bx) - PLAYER_RADIUS, min(ay, by) - PLAYER_RADIUS,
                max(ax, bx) + PLAYER_RADIUS, max(ay, by) + PLAYER_RADIUS):
            if segment_segment_dist2(ax, ay, bx, by, door) < radius2:
                required |= door.required_key_mask
        self.edge_cache[key] = required
        return required

    def neighbours(self, node: tuple[int, int], key_mask: int):
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1),
                       (1, 1), (1, -1), (-1, 1), (-1, -1)):
            destination = (node[0] + dx, node[1] + dy)
            required = self.edge_requirement(node, destination)
            if required is None or (required & key_mask) != required:
                continue
            yield destination

    def line_of_sight(self, node: tuple[int, int], x: int, y: int) -> bool:
        ax, ay = self.world(node)
        for wall in self.static_index.query(min(ax, x), min(ay, y),
                                            max(ax, x), max(ay, y)):
            if segments_intersect(ax, ay, x, y, wall):
                return False
        return True

    def find_path(self, start: tuple[int, int], key_mask: int,
                  goal: Callable[[tuple[int, int]], bool],
                  heuristic: Callable[[tuple[int, int]], float]):
        if not self.point_valid(start):
            return None, 0
        if goal(start):
            return [start], 0
        queue: list[tuple[float, int, tuple[int, int]]] = []
        serial = 0
        heapq.heappush(queue, (heuristic(start), serial, start))
        distance = {start: 0}
        parent: dict[tuple[int, int], tuple[int, int]] = {}
        expanded = 0
        while queue:
            _priority, _serial, node = heapq.heappop(queue)
            current_distance = distance[node]
            expanded += 1
            if expanded > MAX_ASTAR_EXPANSIONS:
                return None, expanded
            for destination in self.neighbours(node, key_mask):
                step_cost = 14 if (destination[0] != node[0] and
                                   destination[1] != node[1]) else 10
                candidate = current_distance + step_cost
                if candidate >= distance.get(destination, 1 << 60):
                    continue
                distance[destination] = candidate
                parent[destination] = node
                if goal(destination):
                    path = [destination]
                    cursor = destination
                    while cursor != start:
                        cursor = parent[cursor]
                        path.append(cursor)
                    path.reverse()
                    return path, expanded
                serial += 1
                heapq.heappush(queue, (candidate + heuristic(destination),
                                       serial, destination))
        return None, expanded


def nearest_point_heuristic(grid: NavigationGrid,
                            points: list[tuple[int, int]], radius: int):
    def heuristic(node: tuple[int, int]) -> float:
        x, y = grid.world(node)
        distance = min(math.hypot(x - px, y - py) for px, py in points)
        return max(0.0, distance - radius) * 10.0 / GRID_STEP
    return heuristic


def segment_goal(grid: NavigationGrid, segments: list[Segment], radius: int):
    radius2 = radius * radius
    def goal(node: tuple[int, int]) -> bool:
        x, y = grid.world(node)
        return any(point_segment_dist2(x, y, segment) <= radius2
                   for segment in segments)
    def heuristic(node: tuple[int, int]) -> float:
        x, y = grid.world(node)
        distance = math.sqrt(min(point_segment_dist2(x, y, segment)
                                 for segment in segments))
        return max(0.0, distance - radius) * 10.0 / GRID_STEP
    return goal, heuristic


def point_goal(grid: NavigationGrid, point: tuple[int, int], radius: int,
               require_los: bool = False):
    radius2 = radius * radius
    def goal(node: tuple[int, int]) -> bool:
        x, y = grid.world(node)
        if point_dist2(x, y, point[0], point[1]) > radius2:
            return False
        return not require_los or grid.line_of_sight(node, point[0], point[1])
    return goal, nearest_point_heuristic(grid, [point], radius)


def verify_route(model: Any, player_start: dict[str, int]) -> dict[str, Any]:
    static_segments, door_segments, exit_segments = build_collision_segments(model)
    spawned = spawned_runtime_things(model)
    keys = [thing for thing in spawned if thing.thing_type in base.KEY_THINGS]
    ammo_pickups = [thing for thing in spawned if thing.thing_type in AMMO_THINGS]
    enemies = [thing for thing in spawned if thing.thing_type in ENEMY_THINGS]
    props = [(thing.x, thing.y, BLOCKING_PROPS[thing.thing_type])
             for thing in spawned if thing.thing_type in BLOCKING_PROPS]

    if not exit_segments:
        return {"playable": False, "reason": "no runtime-emitted exit segment"}

    try:
        grid = NavigationGrid(model, player_start["x"], player_start["y"],
                              static_segments, door_segments, props)
    except ValueError as error:
        return {"playable": False, "reason": str(error)}

    current = (0, 0)
    route = [current]
    events: list[dict[str, Any]] = []
    collected_keys: set[int] = set()
    collected_ammo: set[int] = set()
    key_mask = 0
    ammo = START_AMMO
    total_expanded = 0

    def process_pickups(path: list[tuple[int, int]]) -> None:
        nonlocal key_mask, ammo
        for node in path:
            x, y = grid.world(node)
            for thing in keys:
                if thing.source_index in collected_keys:
                    continue
                if point_dist2(x, y, thing.x, thing.y) <= COLLECT_RADIUS ** 2:
                    collected_keys.add(thing.source_index)
                    bit = base.KEY_THINGS[thing.thing_type]
                    key_mask |= bit
                    events.append({"type": "key", "thing_index": thing.source_index,
                                   "color": base.key_name(bit), "x": x, "y": y})
            for thing in ammo_pickups:
                if thing.source_index in collected_ammo:
                    continue
                if point_dist2(x, y, thing.x, thing.y) <= COLLECT_RADIUS ** 2:
                    collected_ammo.add(thing.source_index)
                    amount = AMMO_THINGS[thing.thing_type]
                    ammo = min(MAX_AMMO, ammo + amount)
                    events.append({"type": "ammo", "thing_index": thing.source_index,
                                   "amount": amount, "x": x, "y": y})

    if not grid.point_valid(current):
        return {"playable": False, "reason": "player start collides with emitted geometry"}
    process_pickups(route)

    # Collect every key that is actually reachable under the keys held so far.
    while True:
        candidates = [thing for thing in keys if thing.source_index not in collected_keys]
        best = None
        for thing in candidates:
            goal, heuristic = point_goal(grid, (thing.x, thing.y), COLLECT_RADIUS)
            path, expanded = grid.find_path(current, key_mask, goal, heuristic)
            total_expanded += expanded
            if path is not None and (best is None or len(path) < len(best[0])):
                best = (path, thing)
        if best is None:
            break
        path, _thing = best
        route.extend(path[1:])
        current = path[-1]
        process_pickups(path)

    required_shots = len(enemies) * ENEMY_SHOTS
    total_ammo = START_AMMO + sum(AMMO_THINGS[thing.thing_type]
                                  for thing in ammo_pickups)
    if total_ammo < required_shots:
        return {
            "playable": False,
            "reason": f"{required_shots} shots required but only {total_ammo} ammo exists",
            "final_keys": base.masks_to_names(key_mask),
        }

    # Every target must be killed because main.c locks the exit while targets live.
    for enemy in enemies:
        while ammo < ENEMY_SHOTS:
            best_ammo = None
            for pickup in ammo_pickups:
                if pickup.source_index in collected_ammo:
                    continue
                goal, heuristic = point_goal(grid, (pickup.x, pickup.y), COLLECT_RADIUS)
                path, expanded = grid.find_path(current, key_mask, goal, heuristic)
                total_expanded += expanded
                if path is not None and (best_ammo is None or len(path) < len(best_ammo[0])):
                    best_ammo = (path, pickup)
            if best_ammo is None:
                return {"playable": False,
                        "reason": "reachable ammo is insufficient for all spawned targets",
                        "final_keys": base.masks_to_names(key_mask)}
            path, _pickup = best_ammo
            route.extend(path[1:])
            current = path[-1]
            process_pickups(path)

        goal, heuristic = point_goal(
            grid, (enemy.x, enemy.y), TARGET_APPROACH_RADIUS, require_los=True)
        path, expanded = grid.find_path(current, key_mask, goal, heuristic)
        total_expanded += expanded
        if path is None:
            return {
                "playable": False,
                "reason": f"spawned target THING {enemy.source_index} is unreachable",
                "final_keys": base.masks_to_names(key_mask),
            }
        route.extend(path[1:])
        current = path[-1]
        process_pickups(path)
        ammo -= ENEMY_SHOTS
        x, y = grid.world(current)
        events.append({"type": "target", "thing_index": enemy.source_index,
                       "shots": ENEMY_SHOTS, "x": x, "y": y})

    exit_goal, exit_heuristic = segment_goal(
        grid, exit_segments, EXIT_APPROACH_RADIUS)
    path, expanded = grid.find_path(current, key_mask, exit_goal, exit_heuristic)
    total_expanded += expanded
    if path is None:
        return {
            "playable": False,
            "reason": "no collision-clear route reaches a runtime exit after clearing targets",
            "final_keys": base.masks_to_names(key_mask),
        }
    route.extend(path[1:])
    current = path[-1]
    process_pickups(path)
    x, y = grid.world(current)
    events.append({"type": "exit", "x": x, "y": y})

    waypoints = []
    for index, node in enumerate(route):
        if index == 0 or index == len(route) - 1 or index % 32 == 0:
            wx, wy = grid.world(node)
            waypoints.append({"step": index, "x": wx, "y": wy})

    return {
        "playable": True,
        "method": "constructive_collision_grid",
        "grid_step": GRID_STEP,
        "player_radius": PLAYER_RADIUS,
        "grid_points": grid.grid_points,
        "astar_expansions": total_expanded,
        "route_steps": len(route) - 1,
        "route_distance_upper_bound": (len(route) - 1) * math.ceil(
            math.sqrt(2) * GRID_STEP),
        "final_keys": base.masks_to_names(key_mask),
        "targets_cleared": len(enemies),
        "ammo_remaining": ammo,
        "events": events,
        "waypoints": waypoints,
    }
