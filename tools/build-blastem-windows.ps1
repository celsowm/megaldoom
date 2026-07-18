param(
    [switch]$Setup,
    [switch]$Debug
)

$ErrorActionPreference = "Stop"
$Root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$Source = Join-Path $Root ".externals\blastem"
$Toolchain = Join-Path $Root ".externals\toolchain"
$Output = Join-Path $Source "build\windows"
$RunnerPatch = Join-Path $PSScriptRoot "blastem-runner.patch"

if ($Setup) { & (Join-Path $PSScriptRoot "setup-blastem-toolchain.ps1"); if ($LASTEXITCODE) { exit $LASTEXITCODE } }
if (-not (Test-Path (Join-Path $Source "Makefile"))) { throw "BlastEm source is missing at $Source." }
if (-not (Test-Path (Join-Path $Source "megaldoom_runner.c"))) {
    Push-Location $Root
    try {
        & git apply --check --directory=.externals/blastem $RunnerPatch
        if ($LASTEXITCODE -ne 0) { throw "The deterministic-runner patch no longer applies cleanly." }
        & git apply --directory=.externals/blastem $RunnerPatch
        if ($LASTEXITCODE -ne 0) { throw "Could not apply the deterministic-runner patch." }
    } finally { Pop-Location }
}

$clang = Get-ChildItem -Path (Join-Path $Toolchain "llvm-mingw") -Filter "x86_64-w64-mingw32-clang.exe" -Recurse | Select-Object -First 1
$windres = Get-ChildItem -Path (Join-Path $Toolchain "llvm-mingw") -Filter "llvm-windres.exe" -Recurse | Select-Object -First 1
$sdl = Get-ChildItem -Path (Join-Path $Toolchain "SDL2") -Directory -Recurse | Where-Object { $_.Name -eq "x86_64-w64-mingw32" } | Select-Object -First 1
$glew = Join-Path $Toolchain "glew"
if (-not $clang -or -not $windres -or -not $sdl -or -not (Test-Path (Join-Path $glew "include\GL\glew.h"))) {
    throw "Toolchain is incomplete. Run: pwsh -NoProfile -File tools/setup-blastem-toolchain.ps1"
}

# BlastEm's Makefile already has a Windows target; pass every path explicitly
# so no global MinGW/MSYS installation leaks into the build.
$wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
if (-not $wsl) { throw "WSL with GNU make is required to drive this portable Windows toolchain." }
$sourceWsl = (& wsl.exe -- wslpath -a ($Source -replace '\\', '/')).Trim()
$clangWsl = (& wsl.exe -- wslpath -a ($clang.FullName -replace '\\', '/')).Trim()
$windresWsl = (& wsl.exe -- wslpath -a ($windres.FullName -replace '\\', '/')).Trim()
$sdlWsl = (& wsl.exe -- wslpath -a ($sdl.FullName -replace '\\', '/')).Trim()
$glewWsl = (& wsl.exe -- wslpath -a ($glew -replace '\\', '/')).Trim()
$makeArgs = "OS=Windows CPU=x86_64 CC='$clangWsl' WINDRES='$windresWsl' SDL2_PREFIX='$sdlWsl' GLEW_PREFIX='$glewWsl' NOLTO=1 NONUKLEAR=1"
if ($Debug) { $makeArgs += " DEBUG=1" }
$makeCommand = "set -e; command -v make >/dev/null; cd '$sourceWsl'; make $makeArgs blastem.exe"
& wsl.exe -- bash -lc $makeCommand
if ($LASTEXITCODE -ne 0) { throw "BlastEm Windows build failed." }

New-Item -ItemType Directory -Path $Output -Force | Out-Null
Copy-Item (Join-Path $Source "blastem.exe") (Join-Path $Output "blastem.exe") -Force
$sdlDll = Get-ChildItem -Path $sdl.FullName -Filter "SDL2.dll" -Recurse | Select-Object -First 1
if ($sdlDll) { Copy-Item $sdlDll.FullName (Join-Path $Output "SDL2.dll") -Force }
foreach ($runtimeFile in @("default.cfg", "systems.cfg", "rom.db", "menu.bin", "gamecontrollerdb.txt")) {
    $runtimeSource = Join-Path $Source $runtimeFile
    if (Test-Path $runtimeSource) { Copy-Item $runtimeSource (Join-Path $Output $runtimeFile) -Force }
}

Write-Host "Custom Windows BlastEm built:" -ForegroundColor Green
Write-Host "  $(Join-Path $Output 'blastem.exe')"
