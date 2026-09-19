param(
    # Levels to walk; default is the whole campaign.
    [string[]]$Levels = @("E1M1", "E1M2", "E1M3", "E1M4", "E1M5"),
    # Pass for the negative control: the run is then REQUIRED to report
    # mismatches (bake with tools/bsp_vis.py --negative-control-drop-every N).
    [switch]$ExpectMismatch,
    # The oracle casts every frame twice, so a route needs more host frames
    # than the plain E2E budget to finish.
    [double]$FrameScale = 1.5,
    # Frames with a program between checks (BSP_VIS_ORACLE_EVERY).
    [int]$Every = 4
)

# Differential check for the baked visibility programs (BSP_VIS_LIST). Each
# level's certified E2E route is walked in a BSP_VIS_ORACLE build, which casts
# every frame both through its subsector's program and through the node
# traversal and counts frames whose RayColumns differ. Zero is the only pass.

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "e2e-common.ps1")

$root = Get-MegalDoomRoot
$manifest = Get-Content -LiteralPath (Join-Path $PSScriptRoot "e2e-levels.json") -Raw |
    ConvertFrom-Json
$failed = $false
$anyMismatch = $false
foreach ($level in $Levels) {
    $case = @($manifest.levels | Where-Object { $_.name -eq $level })[0]
    if (-not $case) { throw "Unknown level $level" }
    $waypoints = Join-Path $root $case.waypoints
    & python (Join-Path $PSScriptRoot "generate-e2e-routes.py") --map $case.map --out $waypoints
    if ($LASTEXITCODE -ne 0) { throw "Could not generate waypoints for $level." }

    Invoke-MegalDoomDebugBuild ("-DDEBUG_BLASTEM_CHECKPOINT=1 -DDEBUG_E2E_START_LEVEL=$($case.index) " +
        "-DDEBUG_E2E_GOD=1 -DBSP_VIS_LIST=1 -DBSP_VIS_ORACLE=1 -DBSP_VIS_ORACLE_EVERY=$Every")
    $mailbox = Resolve-MegalDoomMailbox -Symbol "g_debug_e2e_state" -Bytes 20
    $oracle = Resolve-MegalDoomMailbox -Symbol "g_bsp_vis_oracle" -Bytes 28
    $report = Join-Path $root ("out\{0}-vis-oracle.json" -f $level.ToLowerInvariant())
    Invoke-MegalDoomRoute -Waypoints $waypoints -Frames ([int]([int]$case.frames * $FrameScale)) -Report $report `
        -Mailbox $mailbox -PerfMailbox $oracle

    $data = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
    $hex = [string]$data.perfMailbox
    [byte[]]$b = for ($i = 0; $i -lt 28; $i++) { [Convert]::ToByte($hex.Substring($i * 2, 2), 16) }
    $u32 = { param($o) ([uint32]$b[$o] -shl 24) -bor ([uint32]$b[$o + 1] -shl 16) -bor
                        ([uint32]$b[$o + 2] -shl 8) -bor [uint32]$b[$o + 3] }
    $u16 = { param($o) ([int]$b[$o] -shl 8) -bor [int]$b[$o + 1] }
    $s16 = { param($o) $v = & $u16 $o; if ($v -ge 32768) { $v - 65536 } else { $v } }
    $frames = & $u32 0
    $mismatch = & $u32 4
    $unprogrammed = & $u16 8
    $progress = "{0}/{1}" -f $data.waypoints.current, $data.waypoints.total
    # A route that stalled proves nothing about the poses it never reached.
    $complete = [bool]$data.waypoints.complete -and -not [bool]$data.waypoints.failed -and
                [int]$data.waypoints.current -eq [int]$data.waypoints.total
    if (-not $complete -and -not $ExpectMismatch) {
        Write-Host ("{0} oracle: route did not complete ({1}, reason '{2}') -- rerun with a larger -FrameScale" -f
            $level, $progress, $data.waypoints.reason) -ForegroundColor Red
        $failed = $true
    }
    if ($frames -eq 0) {
        Write-Host "$level oracle: no frames checked (route $progress)" -ForegroundColor Red
        $failed = $true
        continue
    }
    if ($mismatch -ne 0) {
        $anyMismatch = $true
        Write-Host ("{0} oracle: {1} of {2} frames differ; first at subsector {3} pose ({4},{5}) angle {6} sample {7}: program depth {8} tex {9} door {10} / traversal depth {11} tex {12} door {13} (route {14})" -f
            $level, $mismatch, $frames, (& $u16 10), (& $s16 12), (& $s16 14), (& $u16 16), (& $u16 18),
            (& $u16 20), $b[24], $b[26], (& $u16 22), $b[25], $b[27], $progress) `
            -ForegroundColor $(if ($ExpectMismatch) { "Yellow" } else { "Red" })
        if (-not $ExpectMismatch) { $failed = $true }
    } else {
        Write-Host ("{0} oracle: {1} frames identical (1 in {2}), {3} without a program (route {4})" -f
            $level, $frames, $Every, $unprogrammed, $progress) -ForegroundColor Green
    }
}
if ($ExpectMismatch -and -not $anyMismatch) {
    Write-Host "negative control: the broken bake was NOT detected" -ForegroundColor Red
    $failed = $true
}
if ($failed) { exit 1 }
exit 0
