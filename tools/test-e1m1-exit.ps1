param(
    [switch]$NoBuild
)

# Deterministically enters E1M1 at the certified exit switch and proves that
# repeated C pulses reach gameplay plus the exit transition. The test bypasses
# frontend timing and its pose hook is compiled only for this test ROM.
$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")

if (-not $NoBuild) {
    $previousExtraFlags = $env:EXTRA_FLAGS
    $env:EXTRA_FLAGS = "-DDEBUG_BLASTEM_CHECKPOINT=1 -DDEBUG_START_E1M1_EXIT=1"
    try {
        & (Join-Path $PSScriptRoot "build-windows.ps1") -Clean
        if ($LASTEXITCODE -ne 0) {
            Write-Host "E1M1 exit checkpoint build failed." -ForegroundColor Red
            exit $LASTEXITCODE
        }
    } finally {
        if ($null -eq $previousExtraFlags) { Remove-Item Env:\EXTRA_FLAGS -ErrorAction SilentlyContinue }
        else { $env:EXTRA_FLAGS = $previousExtraFlags }
    }
}

$mailbox = (& python (Join-Path $PSScriptRoot "resolve-symbol.py") `
    (Join-Path $Root "out\symbol.txt") g_debug_checkpoint_state --bytes 1).Trim()
if ($LASTEXITCODE -ne 0 -or -not $mailbox) {
    Write-Host "Could not resolve the E1M1 exit checkpoint mailbox." -ForegroundColor Red
    exit 1
}

$report = Join-Path $Root "out\e1m1-exit-report.json"
& (Join-Path $PSScriptRoot "run-blastem-route.ps1") `
    -Route (Join-Path $Root "tools\routes\e1m1-exit-switch.txt") `
    -Frames 800 -Report $report -Mailbox $mailbox -RequireCheckpoints "84"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$checkpoints = (Get-Content $report -Raw | ConvertFrom-Json).checkpoints
Write-Host ("E1M1 exit checkpoints seen: 0x{0:X2}" -f $checkpoints.seen)
if ($checkpoints.ok -ne $true) { exit 1 }
