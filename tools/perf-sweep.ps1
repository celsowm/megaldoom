<#
.SYNOPSIS
  Pose-locked performance sweep: builds one ROM per heading and reports where a
  frame goes at each.

.DESCRIPTION
  The instrument behind the 2026-09-07 courtyard-hall finding. Two rules from
  AGENTS.md are baked in so they cannot be forgotten:

    * A route cannot hold pose (the loop is vblank-paced, so a faster build walks
      somewhere else), therefore every measurement is PERF_FIXED_POSE.
    * Heading matters more than position -- at one spot in E1M1 the frame spans
      2.6 to 5.3 fps on heading alone, and cast and pack peak at DIFFERENT
      headings -- therefore the unit of measurement is a sweep, not a pose.

  Find the pose to sweep by walking a route and reading the pose mailbox back;
  see tools/decode-e2e-pose.py and the AGENTS.md note above it.

  Variants (-Variant), all pose-identical so they can be diffed directly:
    base    : as shipped.
    stub    : PERF_STUB_DOOR_OVERLAYS -- draw_door_overlays is a no-op, so
              base.pack - stub.pack is the door/window compositor's true cost.
              cast must NOT move between the two; if it does, discard the run.
    drawseg : CADENCE_DRAWSEG_SPLIT -- breaks cast into draw_seg vs traversal.
              Costs ~1500 subticks of cast itself, so read it for the SPLIT, not
              for the total.

.EXAMPLE
  # Where does this spot get slow, and which way?
  tools/perf-sweep.ps1 -X -285 -Y 3295

.EXAMPLE
  # Price the window compositor at the worst heading found above.
  tools/perf-sweep.ps1 -X -285 -Y 3295 -Angles 233 -Variant base,stub
#>
param(
    [Parameter(Mandatory = $true)][int]$X,
    [Parameter(Mandatory = $true)][int]$Y,
    [int[]]$Angles = @(9, 41, 73, 105, 137, 169, 201, 233),
    [ValidateSet("base", "stub", "drawseg")][string[]]$Variant = @("base"),
    [int]$Frames = 3200,
    [string]$Route = "tools/routes/fixed-pose.txt",
    [string]$OutDir = "out/sweep",
    # Extra -D flags appended to every build in the sweep, for one-off
    # experiments (e.g. "-DBSP_EXP_NO_CHEAP_REJECT=1"). -Label keeps their
    # reports and CSV from overwriting the baseline's.
    [string]$ExtraFlags = "",
    [string]$Label = "",
    [switch]$Capture
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
Push-Location $Root

$VariantFlags = @{
    base    = ""
    stub    = " -DPERF_STUB_DOOR_OVERLAYS=1"
    drawseg = " -DCADENCE_DRAWSEG_SPLIT=1"
}

function Get-Field([string[]]$lines, [string]$label) {
    $m = $lines | Select-String -Pattern ([regex]::Escape($label)) | Select-Object -First 1
    if (-not $m) { return $null }
    # "  cast       avg   =   18529 subticks/rebuild (14.48 vblanks)" -> 18529
    $after = $m.ToString().Split('=')[1].Trim()
    if ($after -match '^([-\d.]+)') { return [double]$Matches[1] }
    return $null
}

try {
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
    $rows = @()

    foreach ($v in $Variant) {
        foreach ($a in $Angles) {
            $tag = "$($v)$(if ($Label) { "-$Label" })-x$($X)y$($Y)a$($a)"
            Write-Host "building $tag ..." -ForegroundColor DarkGray
            $env:EXTRA_FLAGS = ("-DDEBUG_BLASTEM_CHECKPOINT=1 -DPERF_FIXED_POSE=1 " +
                "-DPERF_POSE_X=$X -DPERF_POSE_Y=$Y -DPERF_POSE_ANGLE=$a") + $VariantFlags[$v] +
                $(if ($ExtraFlags) { " $ExtraFlags" })
            & (Join-Path $PSScriptRoot "build-windows.ps1") -Clean *> $null
            if ($LASTEXITCODE -ne 0) { throw "build failed for $tag" }

            $sym = & python (Join-Path $PSScriptRoot "resolve-symbol.py") "out/symbol.txt" "g_debug_perf_mailbox"
            if ($LASTEXITCODE -ne 0) { throw "could not resolve g_debug_perf_mailbox" }

            $report = Join-Path $OutDir "$tag.json"
            $runArgs = @{ Route = $Route; Frames = $Frames; RomPath = "out/rom.bin";
                          PerfMailbox = "$($sym):256"; Report = $report }
            if ($Capture) {
                # Late only: fixed-pose.txt is still in the frontend early on, and a
                # capture from there is the TITLE SCREEN, not the scene (AGENTS.md).
                $runArgs.CaptureDir = Join-Path $OutDir $tag
                $runArgs.CaptureEvery = $Frames - 100
                $runArgs.CleanCaptureDir = $true
            }
            & (Join-Path $PSScriptRoot "run-blastem-route.ps1") @runArgs *> $null
            if ($LASTEXITCODE -ne 0) { throw "route run failed for $tag" }

            $d = @(& python (Join-Path $PSScriptRoot "decode-cadence.py") $report)
            if ($d -match "bad magic") { throw "$tag decoded as a non-cadence build" }

            $rows += [pscustomobject]@{
                variant = $v
                angle   = $a
                vblanks = Get-Field $d "avg vblanks"
                cast    = Get-Field $d "cast       avg"
                pack    = Get-Field $d "pack       avg"
                proj    = Get-Field $d "projection avg"
                bb      = Get-Field $d "billboard  avg"
                nodes   = Get-Field $d "nodes visited   avg"
                segs    = Get-Field $d "segs tested     avg"
                boxes   = Get-Field $d "box calls"
                rebuilds= Get-Field $d "rebuild frames"
                drawseg = Get-Field $d "drawseg total"
                report  = $report
            }
        }
    }

    Write-Host ""
    Write-Host "pose ($X, $Y)   route $Route   $Frames frames" -ForegroundColor Cyan
    # Out-String, not a bare Format-Table: formatted output is otherwise flushed
    # after the Write-Host lines below and the table lands under its own summary.
    ($rows | Format-Table variant, angle, vblanks, cast, pack, proj, bb, nodes, segs,
        boxes, rebuilds -AutoSize | Out-String).TrimEnd() | Write-Host

    # rebuilds is the sample count behind every average in the row. A handful of
    # frames is a thin sample, and a zero means the run never reached gameplay --
    # which a route CAN do (see AGENTS.md on empty snapshots), so say so loudly
    # rather than printing an authoritative-looking table of zeros.
    $empty = $rows | Where-Object { -not $_.rebuilds -or $_.rebuilds -lt 5 }
    if ($empty) {
        Write-Host ("WARNING: " + ($empty | ForEach-Object { "$($_.variant)/a$($_.angle)" } ) -join ", ") -ForegroundColor Red
        Write-Host "  fewer than 5 rebuild frames -- the run may never have reached gameplay." -ForegroundColor Red
    }

    $worst = $rows | Sort-Object vblanks -Descending | Select-Object -First 1
    $best  = $rows | Sort-Object vblanks | Select-Object -First 1
    if ($worst -and $best -and $worst.vblanks) {
        Write-Host ("worst: {0}/a{1}  {2} vb ({3:N1} fps)   best: {4}/a{5}  {6} vb ({7:N1} fps)" -f `
            $worst.variant, $worst.angle, $worst.vblanks, (60 / $worst.vblanks),
            $best.variant, $best.angle, $best.vblanks, (60 / $best.vblanks)) -ForegroundColor Yellow
    }

    if ($Variant -contains "base" -and $Variant -contains "stub") {
        Write-Host ""
        Write-Host "overlay compositor (base.pack - stub.pack), cast as control:" -ForegroundColor Cyan
        foreach ($a in $Angles) {
            $b = $rows | Where-Object { $_.variant -eq "base" -and $_.angle -eq $a }
            $s = $rows | Where-Object { $_.variant -eq "stub" -and $_.angle -eq $a }
            if ($b -and $s) {
                $castDrift = [math]::Abs($b.cast - $s.cast)
                $verdict = if ($castDrift -gt ($b.cast * 0.02)) { "  INVALID: cast moved $castDrift" } else { "" }
                Write-Host ("  a{0,-4} overlay = {1,6} subticks ({2,5:N2} vb, {3,4:N1}% of frame){4}" -f `
                    $a, ($b.pack - $s.pack), (($b.pack - $s.pack) / 1280),
                    (100 * ($b.pack - $s.pack) / ($b.vblanks * 1280)), $verdict)
            }
        }
    }

    $csv = Join-Path $OutDir "sweep$(if ($Label) { "-$Label" })-x$($X)y$($Y).csv"
    $rows | Export-Csv -NoTypeInformation -Path $csv
    Write-Host ""
    Write-Host "rows -> $csv" -ForegroundColor DarkGray
}
finally {
    Remove-Item Env:\EXTRA_FLAGS -ErrorAction SilentlyContinue
    Pop-Location
}
