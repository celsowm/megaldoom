param(
    [string]$GdkPath = $env:GDK,
    [switch]$NoClean
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")

function Resolve-LocalPath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) { return $null }
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $Root $Path))
}

if (-not $GdkPath) {
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

Push-Location $Root
try {
    if (-not $NoClean) {
        & $Make -f Makefile clean
    }
    & $Make -f Makefile
} finally {
    Pop-Location
}
