param(
    [Parameter(Mandatory = $true)][string]$Route,
    [int]$Frames = 600,
    [string]$RomPath = "out\rom.bin",
    [string]$Report = "out\blastem-route-report.json",
    [string]$Mailbox = "",
    [string]$PerfMailbox = "",
    [string]$RequireCheckpoints = "",
    [string]$CaptureDir = "",
    [int]$CaptureEvery = 0
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
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Report) | Out-Null

$args = @("-b", $Frames, "--md-route", $Route, "--md-report", $Report)
if ($Mailbox) { $args += @("--md-mailbox", $Mailbox) }
if ($PerfMailbox) { $args += @("--md-perf-mailbox", $PerfMailbox) }
if ($RequireCheckpoints) { $args += @("--md-require-checkpoints", $RequireCheckpoints) }
if ($CaptureDir) {
    $CaptureDir = AbsolutePath $CaptureDir
    New-Item -ItemType Directory -Force -Path $CaptureDir | Out-Null
    $args += @("--md-capture-dir", $CaptureDir)
    if ($CaptureEvery -gt 0) { $args += @("--md-capture-every", $CaptureEvery) }
}
$args += $RomPath
Push-Location (Split-Path -Parent $BlastEm)
try { & $BlastEm @args } finally { Pop-Location }
if (-not (Test-Path $Report)) { throw "BlastEm finished without emitting $Report" }
Write-Host "Deterministic route report: $Report" -ForegroundColor Green
