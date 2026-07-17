param(
    [string]$GdkPath = $env:GDK,
    [switch]$NoClean,
    [switch]$Clean,
    [switch]$DebugPerf,
    [switch]$ForceAssets
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

# Frontend PNGs stay out of git. The generator fingerprints its exact inputs and
# returns without touching the files when its complete cache is current.
$assetArgs = @((Join-Path $PSScriptRoot "generate-frontend-assets.py"))
if ($ForceAssets) { $assetArgs += "--force" }
& python @assetArgs
if ($LASTEXITCODE -ne 0) {
    Write-Host "Asset generation failed." -ForegroundColor Red
    exit $LASTEXITCODE
}

# SGDK's makefile.gen folds $(EXTRA_FLAGS) into every compile's CFLAGS but never
# assigns it itself. Preserve caller flags, but own the DEBUG_PERF define so a
# previous invocation in the same shell cannot leak it into a release build.
$flags = @()
if (-not [string]::IsNullOrWhiteSpace($env:EXTRA_FLAGS)) {
    $flags = @($env:EXTRA_FLAGS -split '\s+' | Where-Object {
        $_ -and $_ -notmatch '^-DDEBUG_PERF(?:=.*)?$'
    })
}
if ($DebugPerf) { $flags += "-DDEBUG_PERF=1" }
$effectiveFlags = $flags -join " "
if ($effectiveFlags) { $env:EXTRA_FLAGS = $effectiveFlags }
else { Remove-Item Env:\EXTRA_FLAGS -ErrorAction SilentlyContinue }

$statePath = Join-Path $Root "build\build-config.json"
$romStatePath = Join-Path $Root "build\rom-bin.json"
$romPath = Join-Path $Root "out\rom.bin"
$stateTool = Join-Path $PSScriptRoot "build-state.py"
$stateDecision = (& python $stateTool check --state $statePath --output (Join-Path $Root "out") "--flags=$effectiveFlags")
if ($LASTEXITCODE -ne 0) {
    Write-Host "Could not inspect the incremental build state." -ForegroundColor Red
    exit $LASTEXITCODE
}
$mustClean = $Clean -or ($stateDecision -eq "clean")
if ($stateDecision -eq "clean" -and -not $Clean) {
    Write-Host "Compiler flags changed; cleaning stale objects." -ForegroundColor Yellow
}
if ($NoClean) {
    Write-Host "-NoClean is retained for compatibility; builds are incremental by default." -ForegroundColor DarkGray
}

Push-Location $Root
try {
    if ($mustClean) {
        & $Make -f Makefile clean
        if ($LASTEXITCODE -ne 0) {
            Write-Host "Clean failed (make exit code $LASTEXITCODE)." -ForegroundColor Red
            exit $LASTEXITCODE
        }
    }
    # Keep SGDK's target-specific release CFLAGS/AFLAGS, but defer its phony
    # Java padding recipe until the ROM content actually changed.
    & $Make -f Makefile release "SIZEBND=echo"
    $buildExit = $LASTEXITCODE
    if ($buildExit -eq 0) {
        & python $stateTool seal-link --output (Join-Path $Root "out") --root $Root
        $buildExit = $LASTEXITCODE
    }
    if ($buildExit -eq 0) {
        $romDecision = (& python $stateTool check-rom --state $romStatePath --rom $romPath)
        if ($LASTEXITCODE -ne 0) {
            $buildExit = $LASTEXITCODE
        } elseif ($romDecision -eq "pad") {
            # Use release again so LIBMD and all target-specific SGDK variables
            # remain defined. seal-link makes every prerequisite up to date.
            & $Make -f Makefile release
            $buildExit = $LASTEXITCODE
            if ($buildExit -eq 0) {
                & python $stateTool record-rom --state $romStatePath --rom $romPath
                $buildExit = $LASTEXITCODE
            }
        }
    }
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

& python $stateTool seal-link --output (Join-Path $Root "out") --root $Root
if ($LASTEXITCODE -ne 0) {
    Write-Host "Build succeeded, but the incremental link state could not be prepared." -ForegroundColor Red
    exit $LASTEXITCODE
}

& python $stateTool record --state $statePath "--flags=$effectiveFlags"
if ($LASTEXITCODE -ne 0) {
    Write-Host "Build succeeded, but its incremental state could not be recorded." -ForegroundColor Red
    exit $LASTEXITCODE
}
