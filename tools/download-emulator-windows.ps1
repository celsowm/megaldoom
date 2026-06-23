param(
    [ValidateSet("All", "BlastEm", "ares")]
    [string]$Emulator = "All",
    [switch]$Stable,
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$ToolsRoot = Join-Path $Root ".tools"
$BlastEmDir = Join-Path $ToolsRoot "blastem"
$AresDir = Join-Path $ToolsRoot "ares"

function Write-Step($Message) { Write-Host "==> $Message" -ForegroundColor Cyan }
function Ensure-Dir($Path) { if (-not (Test-Path $Path)) { New-Item -ItemType Directory -Path $Path | Out-Null } }

function Set-MegalDoomBlastEmBindings([string]$CfgPath) {
    if (-not (Test-Path $CfgPath)) { return }

    $content = Get-Content -Raw -Path $CfgPath
    $content = $content -replace '(?m)^(\s*)b\s+ui\.plane_debug\s*$', '$1f9 ui.plane_debug'
    if ($content -notmatch '(?m)^\s*b\s+gamepads\.1\.b\s*$') {
        $content = $content -replace '(?m)^(\s*)s\s+gamepads\.1\.b\s*$', '$1b gamepads.1.b' + "`r`n" + '$1s gamepads.1.b'
    }
    Set-Content -Path $CfgPath -Value $content -NoNewline
}

function Get-AresDownloadUrl {
    $downloadPage = Invoke-WebRequest -Uri "https://ares-emu.net/download" -Headers @{ "User-Agent" = "megaldoom-setup" }
    $link = $downloadPage.Links |
        Where-Object { $_.href -match '/ares/releases/download/.+/ares-windows-x64\.zip$' } |
        Select-Object -First 1
    if (-not $link) { throw "Could not find the ares Windows x64 download link." }
    return $link.href
}

function Install-ZipPackage {
    param(
        [string]$Name,
        [string]$Url,
        [string]$InstallDir,
        [string]$ExeName
    )

    if ((Test-Path (Join-Path $InstallDir $ExeName)) -and -not $Force) {
        Write-Step "$Name already present at $InstallDir"
        return
    }

    if ((Test-Path $InstallDir) -and $Force) {
        Remove-Item -LiteralPath $InstallDir -Recurse -Force
    }

    Ensure-Dir $ToolsRoot

    $archiveName = "{0}.zip" -f $Name.ToLowerInvariant()
    $archivePath = Join-Path $ToolsRoot $archiveName
    $extractDir = Join-Path $ToolsRoot ("{0}-extract" -f $Name.ToLowerInvariant())

    if (Test-Path $extractDir) {
        Remove-Item -LiteralPath $extractDir -Recurse -Force
    }
    Ensure-Dir $extractDir

    Write-Step "Downloading $Name"
    Invoke-WebRequest -Uri $Url -OutFile $archivePath -Headers @{ "User-Agent" = "megaldoom-setup" }

    Write-Step "Extracting $Name"
    Expand-Archive -Path $archivePath -DestinationPath $extractDir -Force

    $exe = Get-ChildItem -Path $extractDir -Filter $ExeName -Recurse -File | Select-Object -First 1
    if (-not $exe) { throw "$ExeName was not found after extracting $Name." }

    if (Test-Path $InstallDir) {
        Remove-Item -LiteralPath $InstallDir -Recurse -Force
    }
    Ensure-Dir $InstallDir
    Copy-Item -Path (Join-Path $exe.Directory.FullName "*") -Destination $InstallDir -Recurse -Force

    Remove-Item -LiteralPath $extractDir -Recurse -Force
    Remove-Item -LiteralPath $archivePath -Force
}

function Install-BlastEm {
    if ($Stable) {
        $url = "https://www.retrodev.com/blastem/blastem-win32-0.6.2.zip"
    } else {
        Write-Step "Finding latest BlastEm win64 nightly"
        $index = Invoke-WebRequest -Uri "https://www.retrodev.com/blastem/nightlies/" -Headers @{ "User-Agent" = "megaldoom-setup" }
        $match = [regex]::Match($index.Content, 'href="([^"]*blastem-win64-[^"]*\.zip)"')
        if (-not $match.Success) { throw "Could not find a BlastEm win64 nightly in the index." }
        $url = "https://www.retrodev.com/blastem/nightlies/$($match.Groups[1].Value)"
    }

    Install-ZipPackage -Name "BlastEm" -Url $url -InstallDir $BlastEmDir -ExeName "blastem.exe"
    Set-MegalDoomBlastEmBindings (Join-Path $BlastEmDir "default.cfg")
}

function Install-Ares {
    $url = Get-AresDownloadUrl
    Install-ZipPackage -Name "ares" -Url $url -InstallDir $AresDir -ExeName "ares.exe"
}

switch ($Emulator) {
    "All" {
        Install-BlastEm
        Install-Ares
    }
    "BlastEm" {
        Install-BlastEm
    }
    "ares" {
        Install-Ares
    }
}

Write-Host ""
Write-Host "Installed emulators:" -ForegroundColor Green
if (Test-Path (Join-Path $BlastEmDir "blastem.exe")) {
    Write-Host "  BlastEm: $(Join-Path $BlastEmDir 'blastem.exe')"
}
if (Test-Path (Join-Path $AresDir "ares.exe")) {
    Write-Host "  ares:    $(Join-Path $AresDir 'ares.exe')"
}
Write-Host "Run game with:"
Write-Host "  .\tools\run-windows.ps1"
Write-Host "  .\tools\run-windows.ps1 -Emulator ares"
