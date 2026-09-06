#!/usr/bin/env python3
"""Contract tests for certified pose-driven campaign routes."""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GENERATOR = ROOT / "tools" / "generate-e2e-routes.py"


def lines(path):
    return [line.split() for line in path.read_text().splitlines()
            if line and not line.startswith("#")]


def main():
    with tempfile.TemporaryDirectory() as temp:
        temp = Path(temp)
        for name in ("E1M1", "E1M2"):
            output = temp / (name.lower() + ".waypoints")
            subprocess.check_call([sys.executable, str(GENERATOR), "--map", name,
                                   "--out", str(output)])
            rows = lines(output)
            assert rows and all(len(row) == 10 for row in rows)
            assert all(row[5] in {"MOVE", "USE", "FIRE", "HURT", "EXIT"}
                       for row in rows)
            assert all(int(row[9]) > 0 for row in rows)
            moves = [row for row in rows if row[5] == "MOVE"]
            assert all(int(row[4]) in {12, 24, 32, 48, 80} for row in moves)
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
            subprocess.check_call([sys.executable, str(GENERATOR), "--map", name,
                                   "--out", str(output), "--check"])
        e1m2 = lines(temp / "e1m2.waypoints")
        locked = [row for row in e1m2 if row[5] == "USE" and row[6] == "20"]
        unlocked = [row for row in e1m2 if row[5] == "USE" and row[6] == "40"]
        assert len(locked) == len(unlocked) == 1
        assert locked[0][7] == "2" and unlocked[0][7] == "3"
        assert locked[0][8] == unlocked[0][8]
    print("ok    E2E routes: certified movement, combat, locks, keys and exits")


if __name__ == "__main__":
    main()
