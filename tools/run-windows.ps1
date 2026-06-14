param(
    [string]$RomPath = "",
    [string]$EmulatorPath = $env:EMULATOR,
    [switch]$NoBuild,
    [switch]$NoClean
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")

function Resolve-LocalPath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) { return $null }
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $Root $Path))
}

function Find-Rom {
    $known = @(
        (Join-Path $Root "out\rom.bin"),
        (Join-Path $Root "out\megaldoom.bin")
    )
    foreach ($p in $known) {
        if (Test-Path $p) { return $p }
    }

    $rom = Get-ChildItem -Path $Root -Filter "*.bin" -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -notmatch '\\.toolchain\\|\\.tools\\' } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($rom) { return $rom.FullName }
    return $null
}

if (-not $NoBuild) {
    if ($NoClean) {
        & (Join-Path $PSScriptRoot "build-windows.ps1") -NoClean
    } else {
        & (Join-Path $PSScriptRoot "build-windows.ps1")
    }
}

$RomPath = Resolve-LocalPath $RomPath
if (-not $RomPath) { $RomPath = Find-Rom }
if (-not $RomPath -or -not (Test-Path $RomPath)) {
    Write-Host "ROM was not found. Build first with .\tools\build-windows.ps1" -ForegroundColor Red
    exit 1
}

if (-not $EmulatorPath) {
    $localEmu = Join-Path $Root ".tools\blastem\blastem.exe"
    if (Test-Path $localEmu) { $EmulatorPath = $localEmu }
}

$EmulatorPath = Resolve-LocalPath $EmulatorPath
if (-not $EmulatorPath -or -not (Test-Path $EmulatorPath)) {
    Write-Host "BlastEm was not found." -ForegroundColor Red
    Write-Host "Run: .\tools\download-emulator-windows.ps1"
    Write-Host "Or set: `$env:EMULATOR=\"C:\tools\blastem\blastem.exe\""
    exit 1
}

Write-Host "Running: $RomPath" -ForegroundColor Green
& $EmulatorPath $RomPath
