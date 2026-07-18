param(
    [string]$RomPath = "",
    [string]$EmulatorPath = $env:EMULATOR,
    [ValidateSet("Auto", "BlastEm", "ares")]
    [string]$Emulator = "Auto",
    [switch]$NoBuild,
    [switch]$NoClean,
    [switch]$Clean,
    [switch]$DebugPerf,
    [switch]$ForceAssets,
    [switch]$Restart,
    [switch]$Detach
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")

function Resolve-LocalPath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) { return $null }
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $Root $Path))
}

function Set-MegalDoomBlastEmBindings([string]$EmulatorPath) {
    $localTools = [IO.Path]::GetFullPath((Join-Path $Root ".tools\blastem"))
    $emuDir = [IO.Path]::GetFullPath([IO.Path]::GetDirectoryName($EmulatorPath))
    if (-not $emuDir.StartsWith($localTools, [StringComparison]::OrdinalIgnoreCase)) { return }

    $cfg = Join-Path $emuDir "default.cfg"
    if (-not (Test-Path $cfg)) { return }

    $content = Get-Content -Raw -Path $cfg
    $content = $content -replace '(?m)^(\s*)b\s+ui\.plane_debug\s*$', '$1f9 ui.plane_debug'
    if ($content -notmatch '(?m)^\s*b\s+gamepads\.1\.b\s*$') {
        $content = $content -replace '(?m)^(\s*)s\s+gamepads\.1\.b\s*$', '$1b gamepads.1.b' + "`r`n" + '$1s gamepads.1.b'
    }
    Set-Content -Path $cfg -Value $content -NoNewline
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

function Find-LocalEmulator([string]$Name) {
    switch ($Name) {
        "BlastEm" {
            $path = Join-Path $Root ".tools\blastem\blastem.exe"
            if (Test-Path $path) { return $path }
        }
        "ares" {
            $path = Join-Path $Root ".tools\ares\ares.exe"
            if (Test-Path $path) { return $path }
        }
        "Auto" {
            $blastem = Find-LocalEmulator "BlastEm"
            if ($blastem) { return $blastem }

            $ares = Find-LocalEmulator "ares"
            if ($ares) { return $ares }
        }
    }

    return $null
}

if (-not $EmulatorPath) {
    $EmulatorPath = Find-LocalEmulator $Emulator
}

$EmulatorPath = Resolve-LocalPath $EmulatorPath
if (-not $EmulatorPath -or -not (Test-Path $EmulatorPath)) {
    Write-Host "No emulator was found for selection '$Emulator'." -ForegroundColor Red
    Write-Host "Run: .\tools\download-emulator-windows.ps1"
    Write-Host "Or set: `$env:EMULATOR=\"C:\path\to\emulator.exe\""
    exit 1
}

if ($Restart) {
    $processName = [IO.Path]::GetFileNameWithoutExtension($EmulatorPath)
    $running = @(Get-Process -Name $processName -ErrorAction SilentlyContinue)
    if ($running.Count -gt 0) {
        Write-Host "Stopping $($running.Count) running $processName instance(s)..." -ForegroundColor DarkGray
        $running | Stop-Process -Force
        $running | Wait-Process -ErrorAction SilentlyContinue
    }
}

if (-not $NoBuild) {
    # Preserve named switch values when forwarding to the build script. String
    # array splatting can leave these switches unbound in nested script calls.
    $buildArgs = @{
        NoClean = [bool]$NoClean
        Clean = [bool]$Clean
        DebugPerf = [bool]$DebugPerf
        ForceAssets = [bool]$ForceAssets
    }
    & (Join-Path $PSScriptRoot "build-windows.ps1") @buildArgs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$RomPath = Resolve-LocalPath $RomPath
if (-not $RomPath) { $RomPath = Find-Rom }
if (-not $RomPath -or -not (Test-Path $RomPath)) {
    Write-Host "ROM was not found. Build first with .\tools\build-windows.ps1" -ForegroundColor Red
    exit 1
}

Write-Host "Running: $RomPath" -ForegroundColor Green
Set-MegalDoomBlastEmBindings $EmulatorPath
if ($Detach) {
    Start-Process -FilePath $EmulatorPath -ArgumentList @($RomPath)
    Write-Host "Emulator started in the background." -ForegroundColor Green
} else {
    & $EmulatorPath $RomPath
}
