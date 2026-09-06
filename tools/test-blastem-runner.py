#!/usr/bin/env python3
"""Contracts for deterministic BlastEm route orchestration."""
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main():
    runner = (ROOT / "tools" / "run-blastem-route.ps1").read_text()

    # A run must never accept a stale report, a partially-written JSON file or
    # an emulator/capture failure as success.
    assert "Remove-Item -LiteralPath $Report -Force" in runner
    # Invoke the emulator as a process, not through $LASTEXITCODE: pwsh -File
    # can leave the latter unset after BlastEm's batch-loop exit path.
    assert "Start-Process -FilePath $BlastEm" in runner
    assert "$blastEmExit = $blastEmProcess.ExitCode" in runner
    assert 'throw "BlastEm exited with code $blastEmExit."' in runner
    assert "$attempt -lt 150" in runner
    assert "ConvertFrom-Json" in runner
    assert "$reportData.schemaVersion -ne 2" in runner
    assert "$reportData.captureFailed" in runner
    assert "[string]$Waypoints" in runner
    assert '"--md-waypoints"' in runner
    assert "Specify exactly one of -Route or -Waypoints." in runner

    runner_source = (ROOT / ".externals" / "blastem" / "megaldoom_runner.c").read_text()
    runner_header = (ROOT / ".externals" / "blastem" / "megaldoom_runner.h").read_text()
    blastem_source = (ROOT / ".externals" / "blastem" / "blastem.c").read_text()
    assert "MEGALDOOM_RUNNER_VERSION 4" in runner_source
    assert "megaldoom_waypoints_load" in runner_source
    assert "WAYPOINT_USE" in runner_source and "timeout wp=" in runner_source
    assert '\\"waypoints\\"' in runner_source
    assert "megaldoom_waypoints_load" in runner_header
    assert '"--md-waypoints"' in blastem_source
    runner_patch = (ROOT / "tools" / "blastem-runner.patch").read_text()
    assert "MEGALDOOM_RUNNER_VERSION 4" in runner_patch
    assert "megaldoom_waypoints_load" in runner_patch

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

    # The pose-locked perf route must traverse the same current frontend. It
    # used to retain the pre-frontend 1050/1150 START presses, stop on the main
    # menu and leave a convincing-looking but entirely zero perf mailbox.
    fixed_route = ROOT / "tools" / "routes" / "fixed-pose.txt"
    fixed_events = [(int(frame), int(mask, 16))
                    for frame, mask in (line.split()
                        for line in fixed_route.read_text().splitlines()
                        if line.strip())]
    assert fixed_events == events[:13]
    assert fixed_events[-1] == (2054, 0)

    exit_route = ROOT / "tools" / "routes" / "e1m1-exit-switch.txt"
    exit_events = [(int(frame), int(mask, 16))
                   for frame, mask in (line.split()
                       for line in exit_route.read_text().splitlines() if line.strip())]
    assert exit_events == [(0, 0), (90, 0x40), (100, 0), (250, 0x40),
                           (260, 0), (410, 0x40), (420, 0), (570, 0x40),
                           (580, 0)]
    exit_test = (ROOT / "tools" / "test-e1m1-exit.ps1").read_text()
    assert "DEBUG_START_E1M1_EXIT=1" in exit_test
    assert '-RequireCheckpoints "84"' in exit_test

    print("ok    BlastEm runner: fresh JSON, safe captures, current release route")


if __name__ == "__main__":
    main()
