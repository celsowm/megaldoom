# Runs the asm/C differential harnesses and fails if either one disagrees.
#
# The harnesses (compare_stride2_column_asm in renderer_pack.c,
# compare_overlay_posts_asm in renderer_doors.c) verify the hand-written
# renderer_hotpath.s posts against C reference implementations. They used to be
# gated on DEBUG_PERF, which meant the timing build always paid for them: the
# overlay one checks EVERY overlay column, so on a window wall (~72 of 80
# sampled columns) it alone exceeded the perf overlay's 65535-subtick display
# cap. They are opt-in now -- and this script is what keeps opt-in from meaning
# "never runs".
#
# Pose-locked at the E1M1 nukage-yard window wall, NOT a walking route. Two
# reasons. A walking route cannot be trusted to arrive anywhere in this build:
# the loop is vblank-paced and RENDERER_ASM_DIFF makes it far slower, so
# e1m1-windows.txt never even leaves the frontend here and reports a completely
# empty snapshot -- which the checked=0 guard below catches, but only after
# wasting a run. And this pose is the best coverage available: a window wall in
# close carries an overlay on ~72 of the 80 sampled columns, and PERF_FIXED_POSE
# forces a full rebuild every frame, so the pack cursor sweeps all VIEW_TILE_W
# columns and the overlay harness sees every column, every frame.
param(
    [string]$Route = "tools/routes/fixed-pose.txt",
    [int]$Frames = 3200,
    [int]$MinChecked = 500
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
Push-Location $Root
try {
    $env:EXTRA_FLAGS = "-DDEBUG_BLASTEM_CHECKPOINT=1 -DRENDERER_ASM_DIFF=1 " +
        "-DPERF_FIXED_POSE=1 -DPERF_POSE_X=1300 -DPERF_POSE_Y=3300 -DPERF_POSE_ANGLE=0"
    & (Join-Path $PSScriptRoot "build-windows.ps1") -Clean -DebugPerf | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "build failed" }

    $symbol = & python (Join-Path $PSScriptRoot "resolve-symbol.py") "out/symbol.txt" "g_debug_perf_mailbox"
    if ($LASTEXITCODE -ne 0) { throw "could not resolve g_debug_perf_mailbox" }

    $report = "out/asm-diff-report.json"
    & (Join-Path $PSScriptRoot "run-blastem-route.ps1") -Route $Route -Frames $Frames `
        -RomPath "out/rom.bin" -PerfMailbox "$($symbol):256" -Report $report | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "route run failed" }

    $line = (& python (Join-Path $PSScriptRoot "decode-perf-full.py") $report |
             Select-String -Pattern "^\s*asm:").ToString()
    if (-not $line) { throw "decoder printed no asm line" }
    Write-Host ""
    Write-Host "== asm/C differential ==" -ForegroundColor Cyan
    Write-Host "  $($line.Trim())"

    $checked  = [int]([regex]::Match($line, "checked=(\d+)").Groups[1].Value)
    $mismatch = [int]([regex]::Match($line, "mismatches=(\d+)").Groups[1].Value)
    $canary   = [int]([regex]::Match($line, "canary_failures=(\d+)").Groups[1].Value)

    # checked=0 is a FAILURE, not a pass. A differential that never ran reports
    # zero mismatches too, and that exact false negative is how the pack harness
    # silently compared asm against asm for months (LOG 2026-08-04).
    if ($checked -lt $MinChecked) {
        Write-Host "  FAIL: only $checked tiles checked (want >= $MinChecked)." -ForegroundColor Red
        Write-Host "  A differential that did not run reports zero mismatches too." -ForegroundColor Red
        exit 1
    }
    if ($mismatch -ne 0 -or $canary -ne 0) {
        Write-Host "  FAIL: $mismatch mismatches, $canary canary failures." -ForegroundColor Red
        Write-Host "  Isolate the side with -DRENDERER_OVERLAY_C_REFERENCE=1 (silences" -ForegroundColor Red
        Write-Host "  renderer_doors.c's asm) or -DRENDERER_HOTPATH_C_REFERENCE=1 (the packer's)." -ForegroundColor Red
        exit 1
    }
    Write-Host "  ok    $checked tiles checked (>= $MinChecked), 0 mismatches, 0 canary failures." -ForegroundColor Green
}
finally {
    Remove-Item Env:\EXTRA_FLAGS -ErrorAction SilentlyContinue
    Pop-Location
}
