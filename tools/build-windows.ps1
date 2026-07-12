param(
    [string]$GdkPath = $env:GDK,
    [switch]$NoClean,
    [switch]$DebugPerf,
    [switch]$SectorRenderer
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")

function Resolve-LocalPath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) { return $null }
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $Root $Path))
}

# Prefer an explicitly provided GDK; fall back to the vendored local toolchain
# when it is missing OR points at a path without makefile.gen (a stale $env:GDK
# pointing at e.g. C:\sgdk\ must not block the local fallback).
if (-not $GdkPath -or -not (Test-Path (Join-Path $GdkPath "makefile.gen"))) {
    $localGdk = Join-Path $Root ".toolchain\sgdk"
    if (Test-Path (Join-Path $localGdk "makefile.gen")) {
        $GdkPath = $localGdk
    }
}

$GdkPath = Resolve-LocalPath $GdkPath
if (-not $GdkPath -or -not (Test-Path (Join-Path $GdkPath "makefile.gen"))) {
    Write-Host "SGDK was not found." -ForegroundColor Red
    Write-Host "Run: .\tools\setup-sgdk-windows.ps1"
    Write-Host "Or set: `$env:GDK=\"C:\sgdk\""
    exit 1
}

$env:GDK = $GdkPath
$Make = Join-Path $GdkPath "bin\make.exe"
if (-not (Test-Path $Make)) { $Make = "make" }

# SGDK's makefile.gen folds $(EXTRA_FLAGS) into every compile's CFLAGS but never
# assigns it itself, so it falls through to this environment variable. Object
# files carry no record of which flags built them, so a define toggle must force
# a full rebuild or stale .o files silently keep the old behavior.
if ($DebugPerf -or $SectorRenderer) {
    $flags = @()
    if ($DebugPerf) { $flags += "-DDEBUG_PERF=1" }
    if ($SectorRenderer) { $flags += "-DBSP_SECTOR_RENDERER=1" }
    $env:EXTRA_FLAGS = $flags -join " "
    if ($NoClean) {
        Write-Host "Build flags force a clean rebuild; ignoring -NoClean." -ForegroundColor Yellow
        $NoClean = $false
    }
} elseif ($env:EXTRA_FLAGS -match "DEBUG_PERF|BSP_SECTOR_RENDERER") {
    # Previous invocation in this shell left DEBUG_PERF on; drop it so a plain
    # rebuild doesn't silently keep the perf overlay.
    Remove-Item Env:\EXTRA_FLAGS
    if ($NoClean) {
        Write-Host "Clearing a stale DEBUG_PERF build forces a clean rebuild; ignoring -NoClean." -ForegroundColor Yellow
        $NoClean = $false
    }
}

Push-Location $Root
try {
    if (-not $NoClean) {
        & $Make -f Makefile clean
    }
    & $Make -f Makefile
    $buildExit = $LASTEXITCODE
} finally {
    Pop-Location
}

# make is a native exe, so a non-zero exit does NOT trip $ErrorActionPreference.
# Surface it explicitly so callers (test-windows.ps1 / CI) can't mistake a failed
# compile for success.
if ($buildExit -ne 0) {
    Write-Host "Build failed (make exit code $buildExit)." -ForegroundColor Red
    exit $buildExit
}
