#!/usr/bin/env python3
"""A push must never carry the player through a wall.

Doom walls are zero-thickness lines and the player is a 16-unit circle, so a
single collision test at the END of a move lets any displacement of 32+ units
perpendicular to a wall land "free" on the far side. player_apply_world_push
used to do exactly that for the enemy-hit / barrel knockback, which is +-64
units per axis in one jump: a tester walked out of E1M3 while being shot by a
zombieman and could not get back in (2026-09-11).

This mirrors src/raycast.c's player_apply_world_push and src/bsp/bsp_map.c's
bsp_circle_blocked -- integer rounding included, because seg_point_dist2's
floored Q8 projection shaves the effective radius -- and runs every knockback
direction against every solid seg of the real campaign maps. The unsplit push
is kept as a negative control: it MUST tunnel, or the harness is not able to
see the bug it guards against.
"""
import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from doom_map import SEG_TRIGGER, load_map  # noqa: E402
from wad_reader import WadFile  # noqa: E402

MAPS = ("E1M1", "E1M2", "E1M3", "E1M4")
PLAYER_COLLISION_RADIUS = 16          # src/raycast.h
PLAYER_HIT_PUSH_STEP = 64             # src/main.c: FX_ONE / 4
DOOM_MAX_MOVE = 30                    # src/player_controller.c, per axis per tic
PLAYER_PUSH_MAX_AXIS_STEP = 20        # src/raycast.c
CELL = 64


def c_seg_point_dist2(ax, ay, bx, by, px, py):
    """bsp_map.c seg_point_dist2, including bsp_ratio_q8's floor and >> 8."""
    abx, aby = bx - ax, by - ay
    apx, apy = px - ax, py - ay
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
            tq = (dot * 256) // ab2
            cx = ax + ((abx * tq) >> 8)
            cy = ay + ((aby * tq) >> 8)
    dx, dy = px - cx, py - cy
    return dx * dx + dy * dy


class World:
    def __init__(self, map_data):
        self.lines = []
        for seg in map_data.out_segs:
            # Doors are tested closed (their solid state); triggers never block.
            if seg["type"] == SEG_TRIGGER:
                continue
            ax, ay = map_data.vertices[seg["v1"]]
            bx, by = map_data.vertices[seg["v2"]]
            self.lines.append((ax, ay, bx, by, seg["nx"], seg["ny"]))
        self.grid = {}
        pad = PLAYER_HIT_PUSH_STEP * 2 + PLAYER_COLLISION_RADIUS
        for index, (ax, ay, bx, by, _, _) in enumerate(self.lines):
            for cx in range((min(ax, bx) - pad) // CELL, (max(ax, bx) + pad) // CELL + 1):
                for cy in range((min(ay, by) - pad) // CELL, (max(ay, by) + pad) // CELL + 1):
                    self.grid.setdefault((cx, cy), []).append(index)

    def near(self, x, y):
        return self.grid.get((x // CELL, y // CELL), ())

    def blocked(self, x, y):
        r = PLAYER_COLLISION_RADIUS
        for index in self.near(x, y):
            ax, ay, bx, by, _, _ = self.lines[index]
            if (x + r < min(ax, bx)) or (x - r > max(ax, bx)) or \
               (y + r < min(ay, by)) or (y - r > max(ay, by)):
                continue
            if c_seg_point_dist2(ax, ay, bx, by, x, y) < r * r:
                return True
        return False

    def crossed(self, x0, y0, x1, y1):
        """Did the straight path x0,y0 -> x1,y1 properly cross a solid line?"""
        if (x0, y0) == (x1, y1):
            return False

        def cross(ox, oy, px, py, qx, qy):
            return (px - ox) * (qy - oy) - (py - oy) * (qx - ox)

        for index in self.near(x0, y0):
            ax, ay, bx, by, _, _ = self.lines[index]
            d1 = cross(x0, y0, x1, y1, ax, ay)
            d2 = cross(x0, y0, x1, y1, bx, by)
            d3 = cross(ax, ay, bx, by, x0, y0)
            d4 = cross(ax, ay, bx, by, x1, y1)
            if (d1 * d2 < 0) and (d3 * d4 < 0):
                return True
        return False


def push_unsplit(world, x, y, dx, dy, path):
    """The pre-2026-09-11 player_apply_world_push. Every position the player
    actually occupies is appended to `path`: a straight line from the start to
    the end would flag a legitimate slide around a convex corner as a tunnel."""
    if not world.blocked(x + dx, y + dy):
        path.append((x + dx, y + dy))
        return x + dx, y + dy
    if not world.blocked(x + dx, y):
        x += dx
        path.append((x, y))
    if not world.blocked(x, y + dy):
        y += dy
        path.append((x, y))
    return x, y


def push_split(world, x, y, dx, dy, path):
    """src/raycast.c player_apply_world_push: halve until each axis <= 20."""
    if abs(dx) > PLAYER_PUSH_MAX_AXIS_STEP or abs(dy) > PLAYER_PUSH_MAX_AXIS_STEP:
        half_x, half_y = dx >> 1, dy >> 1   # C >> on s32 is arithmetic
        x, y = push_split(world, x, y, half_x, half_y, path)
        return push_split(world, x, y, dx - half_x, dy - half_y, path)
    return push_unsplit(world, x, y, dx, dy, path)


def path_tunnels(world, push, x, y, dx, dy):
    path = [(x, y)]
    push(world, x, y, dx, dy, path)
    for (x0, y0), (x1, y1) in zip(path, path[1:]):
        if world.crossed(x0, y0, x1, y1):
            return (x0, y0, x1, y1)
    return None


def push_vectors():
    # Enemy hit and barrel knockback: sign per axis times PLAYER_HIT_PUSH_STEP.
    for sx in (-1, 0, 1):
        for sy in (-1, 0, 1):
            if sx or sy:
                yield sx * PLAYER_HIT_PUSH_STEP, sy * PLAYER_HIT_PUSH_STEP
                # A single movement tic at the momentum clamp.
                yield sx * DOOM_MAX_MOVE, sy * DOOM_MAX_MOVE


def run_map(name, wad):
    world = World(load_map(wad, name))
    trials = unsplit_tunnels = split_tunnels = 0
    example = None
    for ax, ay, bx, by, nx, ny in world.lines:
        length = math.hypot(bx - ax, by - ay)
        if length < 8:
            continue
        mx, my = (ax + bx) / 2, (ay + by) / 2
        norm = math.hypot(nx, ny)
        ux, uy = nx / norm, ny / norm
        for side in (1, -1):
            for dist in range(PLAYER_COLLISION_RADIUS, 50, 2):
                x = int(round(mx + side * dist * ux))
                y = int(round(my + side * dist * uy))
                if world.blocked(x, y):
                    continue
                for dx, dy in push_vectors():
                    # Only pushes that head into the wall can cross it.
                    if (dx * ux + dy * uy) * side >= 0:
                        continue
                    trials += 1
                    if path_tunnels(world, push_unsplit, x, y, dx, dy):
                        unsplit_tunnels += 1
                        if example is None:
                            example = (x, y, dx, dy)
                    hop = path_tunnels(world, push_split, x, y, dx, dy)
                    if hop:
                        split_tunnels += 1
                        print(f"FAIL  {name}: split push from ({x},{y}) by "
                              f"({dx},{dy}) crossed a wall on hop {hop}")
    return trials, unsplit_tunnels, split_tunnels, example


def main():
    wad = WadFile(str(ROOT / "DOOM1.WAD"))
    failed = False
    for name in MAPS:
        trials, unsplit, split, example = run_map(name, wad)
        print(f"      {name}: {trials} pushes, unsplit tunnels {unsplit}, "
              f"split tunnels {split}; e.g. {example}")
        # Negative control: the harness must see the original bug.
        if unsplit == 0:
            print(f"FAIL  {name}: the unsplit push never tunnelled -- the harness "
                  "cannot detect the bug it guards against")
            failed = True
        if split != 0:
            failed = True
    if failed:
        sys.exit(1)
    print("ok    player collision: no knockback or tic step crosses a wall")


if __name__ == "__main__":
    main()
