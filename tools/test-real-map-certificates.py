#!/usr/bin/env python3
"""Acceptance certificates for Doom E1M1, E1M2 and E1M6."""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXTRACTOR = ROOT / "tools/wad-map-extract.py"
WAD = ROOT / "DOOM1.WAD"

EXPECTED = {
    "E1M1": ("available=0x00 required=0x00", "key mask=0x00", "fallback"),
    "E1M2": ("available=0x04 required=0x04", "key mask=0x04", "fallback"),
    "E1M6": ("available=0x07 required=0x07", "reached=[0, 1, 3, 4, 5, 7]", "fallback"),
}


def main():
    reports = []
    for map_name, fragments in EXPECTED.items():
        process = subprocess.run(
            [sys.executable, str(EXTRACTOR), "--wad", str(WAD),
             "--map", map_name, "--certify-only"],
            cwd=ROOT, text=True, capture_output=True)
        report = process.stdout + process.stderr
        assert process.returncode == 0, report
        assert "flat progression certified" in report
        assert "0 door faces still require fallback" in report
        for fragment in fragments:
            assert fragment in report, (map_name, fragment, report)
        reports.append(map_name)
    print("ok    real-map certificates: " + ", ".join(reports))


if __name__ == "__main__":
    main()
