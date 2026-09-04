param(
    [switch]$NoBuild,
    [switch]$NoClean,
    [switch]$Clean,
    [switch]$ForceAssets
)

# Project "test" entry point: build the ROM, then run tools\check-rom.ps1 so
# runtime-only failures (chiefly the SGDK work-RAM boot panic) are caught before
# launching BlastEm. Exits non-zero if the build or any guardrail fails, so this
# doubles as the CI / pre-commit gate. Analogous to `npm run test` (see
# package.json, which wraps this).

$ErrorActionPreference = "Stop"

& python (Join-Path $PSScriptRoot "test-frontend.py")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& python (Join-Path $PSScriptRoot "test-build-incremental.py")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& python (Join-Path $PSScriptRoot "test-blastem-runner.py")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& python (Join-Path $PSScriptRoot "test-billboard-layout.py")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& python (Join-Path $PSScriptRoot "test-billboard-projection.py")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& python (Join-Path $PSScriptRoot "test-billboard-posts.py")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& python (Join-Path $PSScriptRoot "test-billboard-raster.py")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& python (Join-Path $PSScriptRoot "test-billboard-population.py")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& python (Join-Path $PSScriptRoot "test-billboard-registry.py")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& python (Join-Path $PSScriptRoot "test-billboard-explosion.py")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& python (Join-Path $PSScriptRoot "test-sector-map.py")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& python (Join-Path $PSScriptRoot "test-wall-quality.py")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& python (Join-Path $PSScriptRoot "test-world-scale.py")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& python (Join-Path $PSScriptRoot "test-door-animation.py")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& python (Join-Path $PSScriptRoot "test-controller-mode1.py")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& python (Join-Path $PSScriptRoot "test-weapon-bob.py")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& python (Join-Path $PSScriptRoot "test-bsp-render-math.py")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& python (Join-Path $PSScriptRoot "test-bsp-successor.py")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& python (Join-Path $PSScriptRoot "test-flat-progression.py")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& python (Join-Path $PSScriptRoot "test-real-map-certificates.py")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& python (Join-Path $PSScriptRoot "test-campaign.py")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& python (Join-Path $PSScriptRoot "test-renderer-upload-policy.py")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& python (Join-Path $PSScriptRoot "test-hud-layout.py")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& python (Join-Path $PSScriptRoot "test-active-battle-perf.py")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& python (Join-Path $PSScriptRoot "test-weapons.py")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

if (-not $NoBuild) {
    & (Join-Path $PSScriptRoot "build-windows.ps1") `
        -NoClean:$NoClean -Clean:$Clean -ForceAssets:$ForceAssets
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Aborting tests: build failed." -ForegroundColor Red
        exit $LASTEXITCODE
    }
}

& (Join-Path $PSScriptRoot "check-rom.ps1")
exit $LASTEXITCODE
