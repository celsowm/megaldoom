param(
    [string]$InstallDir = ".tools\blastem",
    [switch]$Stable,
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
if (-not [IO.Path]::IsPathRooted($InstallDir)) {
    $InstallDir = Join-Path $Root $InstallDir
}
$InstallDir = [IO.Path]::GetFullPath($InstallDir)
$Archive = Join-Path ([IO.Path]::GetDirectoryName($InstallDir)) "blastem.zip"

function Write-Step($Message) { Write-Host "==> $Message" -ForegroundColor Cyan }
function Ensure-Dir($Path) { if (-not (Test-Path $Path)) { New-Item -ItemType Directory -Path $Path | Out-Null } }

if ((Test-Path (Join-Path $InstallDir "blastem.exe")) -and -not $Force) {
    Write-Step "BlastEm already present at $InstallDir"
} else {
    if ((Test-Path $InstallDir) -and $Force) { Remove-Item -Recurse -Force $InstallDir }
    Ensure-Dir ([IO.Path]::GetDirectoryName($InstallDir))

    if ($Stable) {
        $url = "https://www.retrodev.com/blastem/blastem-win32-0.6.2.zip"
    } else {
        Write-Step "Finding latest BlastEm win64 nightly"
        $index = Invoke-WebRequest -Uri "https://www.retrodev.com/blastem/nightlies/" -Headers @{ "User-Agent" = "megaldoom-setup" }
        $m = [regex]::Match($index.Content, 'href="([^"]*blastem-win64-[^"]*\.zip)"')
        if (-not $m.Success) { throw "Could not find a BlastEm win64 nightly in the index." }
        $url = "https://www.retrodev.com/blastem/nightlies/$($m.Groups[1].Value)"
    }

    Write-Step "Downloading BlastEm"
    Invoke-WebRequest -Uri $url -OutFile $Archive -Headers @{ "User-Agent" = "megaldoom-setup" }

    $Tmp = Join-Path ([IO.Path]::GetDirectoryName($InstallDir)) "blastem-extract"
    if (Test-Path $Tmp) { Remove-Item -Recurse -Force $Tmp }
    Ensure-Dir $Tmp

    Write-Step "Extracting BlastEm"
    Expand-Archive -Path $Archive -DestinationPath $Tmp -Force

    $exe = Get-ChildItem -Path $Tmp -Filter "blastem.exe" -Recurse -File | Select-Object -First 1
    if (-not $exe) { throw "blastem.exe was not found after extraction." }

    if (Test-Path $InstallDir) { Remove-Item -Recurse -Force $InstallDir }
    Ensure-Dir $InstallDir
    Copy-Item -Path (Join-Path $exe.Directory.FullName "*") -Destination $InstallDir -Recurse -Force

    Remove-Item -Recurse -Force $Tmp
    Remove-Item -Force $Archive
}

$emu = Join-Path $InstallDir "blastem.exe"
Write-Host ""
Write-Host "Done. Emulator path:" -ForegroundColor Green
Write-Host "  $emu"
Write-Host "Run game with:"
Write-Host "  .\tools\run-windows.ps1"
