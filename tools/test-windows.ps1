param(
    [switch]$NoBuild,
    [switch]$NoClean,
    [switch]$SectorRenderer
)

# Project "test" entry point: build the ROM, then run tools\check-rom.ps1 so
# runtime-only failures (chiefly the SGDK work-RAM boot panic) are caught before
# launching BlastEm. Exits non-zero if the build or any guardrail fails, so this
# doubles as the CI / pre-commit gate. Analogous to `npm run test` (see
# package.json, which wraps this).

$ErrorActionPreference = "Stop"

& python (Join-Path $PSScriptRoot "test-billboard-layout.py")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& python (Join-Path $PSScriptRoot "test-billboard-projection.py")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& python (Join-Path $PSScriptRoot "test-billboard-population.py")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& python (Join-Path $PSScriptRoot "test-sector-map.py")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& python (Join-Path $PSScriptRoot "test-renderer-upload-policy.py")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& python (Join-Path $PSScriptRoot "test-sector-tile-pipeline.py")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

if (-not $NoBuild) {
    & (Join-Path $PSScriptRoot "build-windows.ps1") -NoClean:$NoClean -SectorRenderer:$SectorRenderer
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Aborting tests: build failed." -ForegroundColor Red
        exit $LASTEXITCODE
    }
}

& (Join-Path $PSScriptRoot "check-rom.ps1")
exit $LASTEXITCODE
