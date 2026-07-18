param(
    [string]$Distro = "Ubuntu"
)

$ErrorActionPreference = "Stop"

$Root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$SourceDir = Join-Path $Root ".externals\blastem"
$OutputDir = Join-Path $SourceDir "build"
$OutputFile = Join-Path $OutputDir "libblastem.so"
$PatchFile = Join-Path $PSScriptRoot "blastem-megaldoom.patch"
$PinnedRevision = "732f5689d4381862da283c420b68f37ac320c1d8"

function Write-Step([string]$Message) {
    Write-Host "==> $Message" -ForegroundColor Cyan
}

if (-not (Test-Path (Join-Path $SourceDir ".hg_archival.txt"))) {
    throw "BlastEm source is missing at $SourceDir. Sync the official source snapshot first."
}

$archival = Get-Content -LiteralPath (Join-Path $SourceDir ".hg_archival.txt") -Raw
if ($archival -notmatch [regex]::Escape("node: $PinnedRevision")) {
    throw "Unexpected BlastEm revision. Expected $PinnedRevision."
}

Write-Step "Checking the MegalDoom BlastEm portability patch"
Push-Location $Root
try {
    & git apply --reverse --check --directory=.externals/blastem $PatchFile 2>$null
    if ($LASTEXITCODE -ne 0) {
        & git apply --check --directory=.externals/blastem $PatchFile
        if ($LASTEXITCODE -ne 0) {
            throw "The BlastEm patch no longer applies cleanly."
        }
        & git apply --directory=.externals/blastem $PatchFile
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to apply the BlastEm patch."
        }
    }
} finally {
    Pop-Location
}

if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
    throw "WSL is required for the current reproducible core build."
}

& wsl.exe -d $Distro -- bash -lc "command -v gcc >/dev/null && command -v g++ >/dev/null && command -v make >/dev/null"
if ($LASTEXITCODE -ne 0) {
    throw "Install gcc, g++ and make inside WSL distro '$Distro'."
}

$sourceForWsl = $SourceDir -replace '\\', '/'
$wslSource = (& wsl.exe -d $Distro -- wslpath -a $sourceForWsl).Trim()
$linuxHome = (& wsl.exe -d $Distro -- bash -lc 'printf %s "$HOME"').Trim()
if (-not $linuxHome.StartsWith("/")) {
    throw "Could not resolve the WSL home directory."
}

$buildDir = "$linuxHome/megaldoom-blastem-build"
$tempDir = "$linuxHome/megaldoom-blastem-tmp"
$buildCommand = "set -e; mkdir -p '$buildDir' '$tempDir'; cp -au '$wslSource/.' '$buildDir/'; export TMPDIR='$tempDir'; cd '$buildDir'; make NOLTO=1 NONUKLEAR=1 libblastem.so"

Write-Step "Building BlastEm core at revision $($PinnedRevision.Substring(0, 12))"
& wsl.exe -d $Distro -- bash -lc $buildCommand
if ($LASTEXITCODE -ne 0) {
    throw "BlastEm core build failed."
}

$uncArtifact = "\\wsl.localhost\$Distro" + ($buildDir -replace '/', '\') + "\libblastem.so"
if (-not (Test-Path -LiteralPath $uncArtifact)) {
    throw "The build completed but libblastem.so was not found."
}

New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
Copy-Item -LiteralPath $uncArtifact -Destination $OutputFile -Force

Write-Step "Verifying exported emulator entry points"
$outputForWsl = $OutputFile -replace '\\', '/'
$wslOutput = (& wsl.exe -d $Distro -- wslpath -a $outputForWsl).Trim()
& wsl.exe -d $Distro -- bash -lc "file '$wslOutput'; nm -D '$wslOutput' | grep -q ' retro_run$'"
if ($LASTEXITCODE -ne 0) {
    throw "libblastem.so is missing the expected Libretro API."
}

Write-Host ""
Write-Host "BlastEm development core built successfully:" -ForegroundColor Green
Write-Host "  Source:   $SourceDir"
Write-Host "  Revision: $PinnedRevision"
Write-Host "  Output:   $OutputFile"
