param(
    [string]$Route = "tools\routes\checkpoints.txt",
    [int]$Frames = 450,
    [string]$RequireCheckpoints = "1F",
    [switch]$NoBuild
)

# Deterministic BlastEm route that walks title -> main menu -> new game ->
# movement -> combat, using SGDK's own g_debug_checkpoint_state mailbox
# (src/debug_checkpoint.h) to prove each screen/state was actually reached,
# not just that N frames elapsed. Requires a build with
# -DDEBUG_BLASTEM_CHECKPOINT=1 so the checkpoint globals and marks compile in;
# that define is NOT part of the normal release build (it costs a few bytes of
# work RAM and is only meaningful under the BlastEm route runner), so this
# script always forces its own rebuild rather than trusting out/rom.bin.

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")

if (-not $NoBuild) {
    $previousExtraFlags = $env:EXTRA_FLAGS
    $env:EXTRA_FLAGS = "-DDEBUG_BLASTEM_CHECKPOINT=1"
    try {
        & (Join-Path $PSScriptRoot "build-windows.ps1") -Clean
        if ($LASTEXITCODE -ne 0) {
            Write-Host "Checkpoint build failed." -ForegroundColor Red
            exit $LASTEXITCODE
        }
    } finally {
        if ($null -eq $previousExtraFlags) { Remove-Item Env:\EXTRA_FLAGS -ErrorAction SilentlyContinue }
        else { $env:EXTRA_FLAGS = $previousExtraFlags }
    }
}

$symbolFile = Join-Path $Root "out\symbol.txt"
$mailbox = (& python (Join-Path $PSScriptRoot "resolve-symbol.py") $symbolFile g_debug_checkpoint_state --bytes 1).Trim()
if ($LASTEXITCODE -ne 0 -or -not $mailbox) {
    Write-Host "Could not resolve g_debug_checkpoint_state - was the build compiled with -DDEBUG_BLASTEM_CHECKPOINT=1?" -ForegroundColor Red
    exit 1
}

$report = Join-Path $Root "out\checkpoint-report.json"
& (Join-Path $PSScriptRoot "run-blastem-route.ps1") `
    -Route (Join-Path $Root $Route) -Frames $Frames -Report $report `
    -Mailbox $mailbox -RequireCheckpoints $RequireCheckpoints
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$reportData = Get-Content $report -Raw | ConvertFrom-Json
$checkpoints = $reportData.checkpoints
Write-Host ("Checkpoints seen: 0x{0:X2}  required: 0x{1:X2}" -f $checkpoints.seen, $checkpoints.required)
if (-not $checkpoints.ok) {
    Write-Host "Checkpoint route FAILED - not all required checkpoints were reached." -ForegroundColor Red
    exit 1
}
Write-Host "Checkpoint route OK." -ForegroundColor Green
exit 0
