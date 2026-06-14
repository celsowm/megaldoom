param(
    [string]$InstallDir = ".toolchain\sgdk",
    [switch]$BuildLibrary,
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
function Resolve-LocalPath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $Root $Path))
}
$InstallDir = Resolve-LocalPath $InstallDir
$Archive = Join-Path ([IO.Path]::GetDirectoryName($InstallDir)) "sgdk-latest.zip"

function Write-Step($Message) {
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Ensure-Dir($Path) {
    if (-not (Test-Path $Path)) {
        New-Item -ItemType Directory -Path $Path | Out-Null
    }
}

function Find-SgdkRoot($Path) {
    $direct = Join-Path $Path "makefile.gen"
    if (Test-Path $direct) { return $Path }

    $candidate = Get-ChildItem -Path $Path -Recurse -Filter "makefile.gen" -File -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($candidate) { return $candidate.Directory.FullName }
    return $null
}

if ((Test-Path (Join-Path $InstallDir "makefile.gen")) -and -not $Force) {
    Write-Step "SGDK already present at $InstallDir"
} else {
    if ((Test-Path $InstallDir) -and $Force) {
        Remove-Item -Recurse -Force $InstallDir
    }

    Ensure-Dir ([IO.Path]::GetDirectoryName($InstallDir))

    Write-Step "Finding latest SGDK release asset"
    $release = Invoke-RestMethod -Uri "https://api.github.com/repos/Stephane-D/SGDK/releases/latest" -Headers @{ "User-Agent" = "megaldoom-setup" }
    $asset = $release.assets |
        Where-Object { $_.name -match '\.zip$' } |
        Select-Object -First 1

    if (-not $asset) {
        throw "Could not find a .zip asset in the latest SGDK release. Open https://github.com/Stephane-D/SGDK/releases and download it manually."
    }

    Write-Step "Downloading $($asset.name)"
    Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $Archive -Headers @{ "User-Agent" = "megaldoom-setup" }

    $Tmp = Join-Path ([IO.Path]::GetDirectoryName($InstallDir)) "sgdk-extract"
    if (Test-Path $Tmp) { Remove-Item -Recurse -Force $Tmp }
    Ensure-Dir $Tmp

    Write-Step "Extracting SGDK"
    Expand-Archive -Path $Archive -DestinationPath $Tmp -Force

    $rootFound = Find-SgdkRoot $Tmp
    if (-not $rootFound) {
        throw "Extracted archive does not look like SGDK; makefile.gen was not found."
    }

    if (Test-Path $InstallDir) { Remove-Item -Recurse -Force $InstallDir }
    Ensure-Dir $InstallDir

    Write-Step "Installing SGDK to $InstallDir"
    Copy-Item -Path (Join-Path $rootFound "*") -Destination $InstallDir -Recurse -Force

    Remove-Item -Recurse -Force $Tmp
    Remove-Item -Force $Archive
}

$env:GDK = $InstallDir
Write-Step "GDK set for this PowerShell session: $env:GDK"

$Make = Join-Path $InstallDir "bin\make.exe"
if (-not (Test-Path $Make)) {
    $Make = "make"
}

if ($BuildLibrary) {
    Write-Step "Building SGDK library"
    & $Make -f (Join-Path $InstallDir "makelib.gen")
}

Write-Host ""
Write-Host "Done. For this terminal session:" -ForegroundColor Green
Write-Host "  `$env:GDK=\"$InstallDir\""
Write-Host "Build the ROM with:"
Write-Host "  .\tools\build-windows.ps1"
