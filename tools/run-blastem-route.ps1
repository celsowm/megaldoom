param(
    [Parameter(Mandatory = $true)][string]$Route,
    [int]$Frames = 600,
    [string]$RomPath = "out\rom.bin",
    [string]$Report = "out\blastem-route-report.json",
    [string]$Mailbox = "",
    [string]$PerfMailbox = "",
    [string]$RequireCheckpoints = "",
    [string]$CaptureDir = "",
    [int]$CaptureEvery = 0,
    [switch]$CleanCaptureDir
)

$ErrorActionPreference = "Stop"
$Root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$BlastEm = Join-Path $Root ".externals\blastem\build\windows\blastem.exe"
if (-not (Test-Path $BlastEm)) { throw "Custom BlastEm is missing. Run npm run blastem:windows first." }

function AbsolutePath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $Root $Path))
}

$Route = AbsolutePath $Route
$RomPath = AbsolutePath $RomPath
$Report = AbsolutePath $Report
if (-not (Test-Path $Route)) { throw "Route file does not exist: $Route" }
if (-not (Test-Path $RomPath)) { throw "ROM does not exist: $RomPath" }
if ($Frames -le 0) { throw "Frames must be greater than zero." }
if ($CaptureEvery -lt 0) { throw "CaptureEvery cannot be negative." }
if ($CaptureEvery -gt 0 -and -not $CaptureDir) {
    throw "CaptureEvery requires CaptureDir."
}
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Report) | Out-Null
if (Test-Path -LiteralPath $Report) {
    # Never let a previous run satisfy the fresh-report checks below.
    Remove-Item -LiteralPath $Report -Force
}

$args = @("-b", $Frames, "--md-route", $Route, "--md-report", $Report)
if ($Mailbox) { $args += @("--md-mailbox", $Mailbox) }
if ($PerfMailbox) { $args += @("--md-perf-mailbox", $PerfMailbox) }
if ($RequireCheckpoints) { $args += @("--md-require-checkpoints", $RequireCheckpoints) }
if ($CaptureDir) {
    $CaptureDir = AbsolutePath $CaptureDir
    New-Item -ItemType Directory -Force -Path $CaptureDir | Out-Null
    $oldCaptures = @(Get-ChildItem -LiteralPath $CaptureDir -Filter "frame-*.ppm" -File)
    if ($oldCaptures.Count -gt 0) {
        if (-not $CleanCaptureDir) {
            throw (("Capture directory already contains {0} frame(s): {1}. " +
                    "Choose a new directory or pass -CleanCaptureDir.") -f
                   $oldCaptures.Count, $CaptureDir)
        }
        # Delete only the runner's exact output pattern inside the resolved
        # capture directory. Contact sheets and unrelated files are preserved.
        foreach ($capture in $oldCaptures) {
            Remove-Item -LiteralPath $capture.FullName -Force
        }
    }
    $args += @("--md-capture-dir", $CaptureDir)
    if ($CaptureEvery -gt 0) { $args += @("--md-capture-every", $CaptureEvery) }
}
$args += $RomPath
Push-Location (Split-Path -Parent $BlastEm)
try {
    & $BlastEm @args
    $blastEmExit = $LASTEXITCODE
} finally {
    Pop-Location
}
if ($blastEmExit -ne 0) {
    throw "BlastEm exited with code $blastEmExit."
}

# Large PPM batches can remain invisible to this process for several seconds
# after BlastEm exits on Windows. Wait for a fresh, complete JSON document, not
# merely for the path to exist; the former two-second existence poll produced
# false failures on dense capture runs.
$reportData = $null
$reportError = $null
for ($attempt = 0; $attempt -lt 150 -and $null -eq $reportData; $attempt++) {
    if (Test-Path -LiteralPath $Report) {
        try {
            $reportData = Get-Content -LiteralPath $Report -Raw | ConvertFrom-Json
        } catch {
            $reportError = $_
        }
    }
    if ($null -eq $reportData) { Start-Sleep -Milliseconds 100 }
}
if ($null -eq $reportData) {
    $detail = if ($null -ne $reportError) { " Last JSON error: $reportError" } else { "" }
    throw "BlastEm finished without a complete report at $Report.$detail"
}
if ($reportData.schemaVersion -ne 2) {
    throw "Unsupported BlastEm report schema: $($reportData.schemaVersion)"
}
if ($reportData.captureFailed) {
    throw "BlastEm reported a capture write failure."
}

$captureSummary = ""
if ($CaptureDir) {
    $captureCount = @(Get-ChildItem -LiteralPath $CaptureDir -Filter "frame-*.ppm" -File).Count
    $captureSummary = ", captures=$captureCount"
}
Write-Host (("Deterministic route OK: frames={0}, cycles={1}{2}") -f
           $reportData.frames, $reportData.cycles, $captureSummary) -ForegroundColor Green
Write-Host "Report: $Report" -ForegroundColor Green
