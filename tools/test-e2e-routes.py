#!/usr/bin/env python3
"""Contract tests for certified pose-driven campaign routes."""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GENERATOR = ROOT / "tools" / "generate-e2e-routes.py"
sys.path.insert(0, str(ROOT / "tools"))

from doom_map import SEG_DOOR, load_map, point_segment_dist2  # noqa: E402
from wad_reader import WadFile  # noqa: E402

# generate-e2e-routes' own clearance; a cell nearer than this to a door face is
# inside that door while it is shut.
CLEARANCE = 20


def door_groups_crossed(map_data, x, y):
    groups = set()
    for seg in map_data.out_segs:
        if seg["type"] != SEG_DOOR:
            continue
        if point_segment_dist2(*map_data.vertices[seg["v1"]],
                               *map_data.vertices[seg["v2"]], x, y) < CLEARANCE ** 2:
            groups.add(seg["door_group"])
    return groups


def assert_every_crossed_door_is_opened(name, rows):
    """No MOVE may enter a door the route has not pressed open by then.

    The certificate records a "use" node only where a group first opens during
    its search, which is not always a node the final path descends from.  E1M2
    crossed group 3 with no press anywhere in the route and the follower spent
    6000 gameplay ticks stalled against the shut door at (-1408,-2080) --
    velocity pinned at 0, the run dying on the waypoint timeout.  E1M3 had
    three such groups.  E1M1 had none, which is why it alone ever passed.
    """
    map_data = load_map(WadFile(str(ROOT / "DOOM1.WAD")), name)
    opened = set()
    for index, row in enumerate(rows):
        x, y = int(row[0]), int(row[1])
        if row[5] == "USE":
            opened.add(int(row[8]))
            continue
        # Walking up to a door and pressing it is one certified cell: the MOVE
        # that arrives is emitted just before the USE that opens, at the same
        # pose.  Count that press as available here -- E1M3 opens its group 8
        # exactly that way at (-1360,1664).  What must not exist is a crossing
        # with no press at all, which is what stalled E1M2 on group 3.
        reachable = set(opened)
        for ahead in rows[index + 1:index + 3]:
            if ahead[5] == "USE" and (ahead[0], ahead[1]) == (row[0], row[1]):
                reachable.add(int(ahead[8]))
        shut = door_groups_crossed(map_data, x, y) - reachable
        assert not shut, ("%s: MOVE at %d,%d enters unopened door group(s) %s"
                          % (name, x, y, sorted(shut)))


def lines(path):
    return [line.split() for line in path.read_text().splitlines()
            if line and not line.startswith("#")]


def main():
    with tempfile.TemporaryDirectory() as temp:
        temp = Path(temp)
        for name in ("E1M1", "E1M2", "E1M3", "E1M4"):
            output = temp / (name.lower() + ".waypoints")
            subprocess.check_call([sys.executable, str(GENERATOR), "--map", name,
                                   "--out", str(output)])
            rows = lines(output)
            assert rows and all(len(row) == 10 for row in rows)
            assert all(row[5] in {"MOVE", "USE", "FIRE", "HURT", "EXIT"}
                       for row in rows)
            assert all(int(row[9]) > 0 for row in rows)
            moves = [row for row in rows if row[5] == "MOVE"]
            # A MOVE's arrival circle must never reach back past the waypoint
            # before it. When it does, one pose counts as arrival at several
            # cells at once, the follower consumes them without moving and
            # then steers at the survivor -- across a corner, that line runs
            # through wall. See clamp_arrival_radii in the generator.
            assert all(16 <= int(row[4]) <= 80 for row in moves)
            # Measure the step from the row immediately before, of whatever
            # kind, because that is where the follower physically stands when
            # this MOVE becomes current.  A USE relocates the player to its own
            # certified pose, so comparing MOVE against MOVE skips that hop and
            # reports a legitimate radius as a corner-cutting one -- E1M3's
            # blue-door USE sits exactly between two MOVEs 16 units apart.
            for previous, row in zip(rows, rows[1:]):
                if row[5] != "MOVE":
                    continue
                step2 = ((int(row[0]) - int(previous[0])) ** 2 +
                         (int(row[1]) - int(previous[1])) ** 2)
                if step2:
                    assert int(row[4]) ** 2 <= max(step2, 16 ** 2), (name, row)
            points = [(int(row[0]), int(row[1])) for row in rows]
            assert all((x2 - x1) ** 2 + (y2 - y1) ** 2 <= 128 ** 2
                       for (x1, y1), (x2, y2) in zip(points, points[1:]))
            assert any(row[5] == "FIRE" and row[6] == "04" for row in rows)
            assert any(row[5] == "HURT" for row in rows)
            assert rows[-1][5] == "EXIT" and rows[-1][6] == "80"
            uses = [row for row in rows if row[5] == "USE"]
            assert uses and all(int(row[7]) >= 0 and int(row[8]) >= 0 for row in uses)
            assert all(row[7] != "4" for row in uses[:-1])
            assert uses[-1][7] == "4"
            assert_every_crossed_door_is_opened(name, rows)
            subprocess.check_call([sys.executable, str(GENERATOR), "--map", name,
                                   "--out", str(output), "--check"])
        # A lock scenario is emitted only for a key the certified route really
        # collects, so E1M1 (no keys at all) has none.  Nor does E1M4: it
        # carries a blue and a yellow key and the doors to match, but its exit
        # certifies at key mask 0x00 -- the certified path reaches the switch
        # without ever needing one, so the route collects neither.  E1M2 and E1M3 must keep
        # theirs: E1M3 reaches its blue door only because the route is pinned
        # to the normal exit -- released, it beelines to the secret exit in
        # 4304 units and touches no key -- and that coverage must not vanish
        # silently if the pinning ever regresses.
        for name in ("E1M2", "E1M3"):
            rows = lines(temp / (name.lower() + ".waypoints"))
            locked = [row for row in rows if row[5] == "USE" and row[6] == "20"]
            unlocked = [row for row in rows if row[5] == "USE" and row[6] == "40"]
            assert len(locked) == len(unlocked) == 1, name
            assert locked[0][7] == "2" and unlocked[0][7] == "3", name
            assert locked[0][8] == unlocked[0][8], name
        for name in ("E1M1", "E1M4"):
            assert not [row for row in lines(temp / (name.lower() + ".waypoints"))
                        if row[5] == "USE" and row[6] in {"20", "40"}]
    print("ok    E2E routes: certified movement, combat, locks, keys and exits")


if __name__ == "__main__":
    main()
