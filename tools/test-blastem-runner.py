#!/usr/bin/env python3
"""Contracts for deterministic BlastEm route orchestration."""
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main():
    runner = (ROOT / "tools" / "run-blastem-route.ps1").read_text()

    # A run must never accept a stale report, a partially-written JSON file or
    # an emulator/capture failure as success.
    assert "Remove-Item -LiteralPath $Report -Force" in runner
    assert "$blastEmExit = $LASTEXITCODE" in runner
    assert 'throw "BlastEm exited with code $blastEmExit."' in runner
    assert "$attempt -lt 150" in runner
    assert "ConvertFrom-Json" in runner
    assert "$reportData.schemaVersion -ne 2" in runner
    assert "$reportData.captureFailed" in runner

    # Reusing a capture directory is opt-in and removes only the exact PPM
    # pattern the runner owns; unrelated images remain available for review.
    assert "[switch]$CleanCaptureDir" in runner
    assert '-Filter "frame-*.ppm" -File' in runner
    assert "if (-not $CleanCaptureDir)" in runner
    assert "Remove-Item -LiteralPath $capture.FullName -Force" in runner

    # This release-visual route has to press START after the current frontend
    # settles. Its former 1050/1120 inputs landed in the fade and left all
    # captures on the title screen.
    route = ROOT / "tools" / "routes" / "e1m1-secret-courtyard.txt"
    events = [(int(frame), int(mask, 16))
              for frame, mask in (line.split()
                  for line in route.read_text().splitlines() if line.strip())]
    assert [frame for frame, _ in events] == sorted(frame for frame, _ in events)
    assert events[:10] == [
        (0, 0), (900, 0x80), (910, 0), (950, 0x80), (960, 0),
        (1650, 0x80), (1660, 0), (1720, 0x10), (1730, 0), (1800, 0x11),
    ]
    assert events[-1] == (8365, 0)

    print("ok    BlastEm runner: fresh JSON, safe captures, current release route")


if __name__ == "__main__":
    main()
