param(
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$Root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$ToolchainRoot = Join-Path $Root ".externals\toolchain"
$Downloads = Join-Path $ToolchainRoot "downloads"

# These are release artifacts, deliberately pinned so a clean checkout gets the
# same compiler ABI and headers.  They remain local because .externals is ignored.
$Packages = @(
    @{ Name = "llvm-mingw"; File = "llvm-mingw-20260407-ucrt-x86_64.zip"; Url = "https://github.com/mstorsjo/llvm-mingw/releases/download/20260407/llvm-mingw-20260407-ucrt-x86_64.zip"; Destination = "llvm-mingw"; Kind = "zip"; Bytes = 187115849 },
    @{ Name = "SDL2"; File = "SDL2-devel-2.32.10-mingw.tar.gz"; Url = "https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-devel-2.32.10-mingw.tar.gz"; Destination = "SDL2"; Kind = "tar" },
    @{ Name = "GLEW"; File = "glew-2.2.0-win32.zip"; Url = "https://github.com/nigels-com/glew/releases/download/glew-2.2.0/glew-2.2.0-win32.zip"; Destination = "glew"; Kind = "zip" }
)

function Get-SingleDirectory([string]$Path) {
    $items = @(Get-ChildItem -LiteralPath $Path -Force)
    if ($items.Count -eq 1 -and $items[0].PSIsContainer) { return $items[0].FullName }
    return $Path
}

function Install-Package($Package) {
    $destination = Join-Path $ToolchainRoot $Package.Destination
    if ((Test-Path $destination) -and -not $Force) {
        Write-Host "==> $($Package.Name) already present" -ForegroundColor DarkGray
        return
    }
    if (Test-Path $destination) { Remove-Item -LiteralPath $destination -Recurse -Force }
    $archive = Join-Path $Downloads $Package.File
    if ((Test-Path $archive) -and (Get-Item -LiteralPath $archive).Length -eq 0) {
        Remove-Item -LiteralPath $archive -Force
    }
    $needsDownload = -not (Test-Path $archive)
    if (-not $needsDownload -and $Package.Bytes -and (Get-Item -LiteralPath $archive).Length -ne $Package.Bytes) {
        $needsDownload = $true
    }
    if ($needsDownload) {
        Write-Host "==> Downloading $($Package.Name)" -ForegroundColor Cyan
        # curl keeps a partially downloaded archive from being mistaken for a
        # completed package and is markedly more reliable with GitHub's large
        # release redirects than Invoke-WebRequest on Windows PowerShell 5.
        $curlArgs = @("-L", "--fail", "--retry", "3")
        if (Test-Path $archive) { $curlArgs += @("-C", "-") }
        $curlArgs += @("--output", $archive, $Package.Url)
        & curl.exe @curlArgs
        if ($LASTEXITCODE -ne 0) { throw "Download failed for $($Package.Name)." }
    }
    $extract = Join-Path $ToolchainRoot ("extract-" + $Package.Destination)
    if (Test-Path $extract) { Remove-Item -LiteralPath $extract -Recurse -Force }
    New-Item -ItemType Directory -Path $extract -Force | Out-Null
    if ($Package.Kind -eq "zip") {
        Expand-Archive -LiteralPath $archive -DestinationPath $extract -Force
    } else {
        & tar.exe -xzf $archive -C $extract
        if ($LASTEXITCODE -ne 0) { throw "Could not extract $archive" }
    }
    Move-Item -LiteralPath (Get-SingleDirectory $extract) -Destination $destination -Force
}

New-Item -ItemType Directory -Path $Downloads -Force | Out-Null
foreach ($package in $Packages) { Install-Package $package }

$clang = Get-ChildItem -Path (Join-Path $ToolchainRoot "llvm-mingw") -Filter "x86_64-w64-mingw32-clang.exe" -Recurse | Select-Object -First 1
$sdlHeader = Get-ChildItem -Path (Join-Path $ToolchainRoot "SDL2") -Filter "SDL.h" -Recurse | Where-Object { $_.FullName -match "include" } | Select-Object -First 1
$glewHeader = Join-Path $ToolchainRoot "glew\include\GL\glew.h"
if (-not $clang -or -not $sdlHeader -or -not (Test-Path $glewHeader)) {
    throw "The portable BlastEm toolchain is incomplete. Re-run with -Force."
}

Write-Host "Portable BlastEm toolchain is ready:" -ForegroundColor Green
Write-Host "  Compiler: $($clang.FullName)"
Write-Host "  SDL2:     $($sdlHeader.Directory.Parent.Parent.FullName)"
Write-Host "  GLEW:     $(Join-Path $ToolchainRoot 'glew')"
