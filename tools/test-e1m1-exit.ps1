param(
    [switch]$NoBuild
)

# Deterministically enters E1M1 at the certified exit switch and proves that
# repeated C pulses reach gameplay plus the exit transition. The test bypasses
# frontend timing and its pose hook is compiled only for this test ROM.
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "e2e-common.ps1")
$Root = Get-MegalDoomRoot

if (-not $NoBuild) {
    Invoke-MegalDoomDebugBuild "-DDEBUG_BLASTEM_CHECKPOINT=1 -DDEBUG_START_E1M1_EXIT=1"
}

$mailbox = Resolve-MegalDoomMailbox -Symbol "g_debug_checkpoint_state" -Bytes 1

$report = Join-Path $Root "out\e1m1-exit-report.json"
Invoke-MegalDoomRoute -Route (Join-Path $Root "tools\routes\e1m1-exit-switch.txt") `
    -Frames 800 -Report $report -Mailbox $mailbox -RequireCheckpoints "84"

$checkpoints = (Get-Content $report -Raw | ConvertFrom-Json).checkpoints
Write-Host ("E1M1 exit checkpoints seen: 0x{0:X2}" -f $checkpoints.seen)
if ($checkpoints.ok -ne $true) { exit 1 }
