$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$GdkPath = $env:GDK
if (-not $GdkPath) { $GdkPath = Join-Path $Root ".toolchain\sgdk" }
$Make = Join-Path $GdkPath "bin\make.exe"
if (-not (Test-Path $Make)) { $Make = "make" }
Push-Location $Root
try {
    if (Test-Path (Join-Path $GdkPath "makefile.gen")) {
        & $Make -f Makefile clean
    }
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue out, build, obj
} finally {
    Pop-Location
}
