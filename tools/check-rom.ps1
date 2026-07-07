param(
    [string]$RomOut = "out/rom.out",
    [string]$RomBin = "out/rom.bin",
    # Work-RAM free-byte thresholds. Empirically calibrated: a build that left
    # ~13.7 KB free crashed at boot with SGDK's "not enough memory to reset VDP";
    # the ~20 KB-free baseline ran fine. Error below MinFree, warn below WarnFree.
    [int]$MinFreeBytes = 16384,
    [int]$WarnFreeBytes = 20480
)

# Post-build guardrails for the Mega Drive ROM. Deterministic, fast, no emulator
# needed - catches the failure classes that only show up at runtime in BlastEm:
#   1. Work-RAM budget. The MD has 64 KB of work RAM shared by static data
#      (.data + .bss), the stack and SGDK's heap (MEM_alloc). Oversized static
#      data starves the heap and SGDK panics "not enough memory to reset VDP" at
#      boot. This is exactly the crash a bare `size` never flags on its own.
#   2. ROM binary present and 128 KB-aligned (sizebnd pads to a valid cart size);
#      a missing/odd ROM means the build didn't actually finish.
# Exits non-zero on any hard failure so `npm run test` / CI go red.

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")

# MD main RAM: 0xFF0000-0xFFFFFF. Holds .data + .bss + stack + SGDK heap. text
# (.text/.rodata) lives in the cartridge ROM, not here, so it does NOT count.
$RamTotal = 65536

$RomOutPath = Join-Path $Root $RomOut
$RomBinPath = Join-Path $Root $RomBin
$SizeExe = Join-Path $Root ".toolchain/sgdk/bin/size.exe"

$script:failed = $false
function Fail([string]$msg) { Write-Host "  FAIL  $msg" -ForegroundColor Red; $script:failed = $true }
function Pass([string]$msg) { Write-Host "  ok    $msg" -ForegroundColor Green }
function Note([string]$msg) { Write-Host "  warn  $msg" -ForegroundColor Yellow }

Write-Host "== MegalDoom ROM guardrails ==" -ForegroundColor Cyan

# --- 0. Prerequisites -------------------------------------------------------
if (-not (Test-Path $SizeExe)) {
    Write-Host "  FAIL  size.exe not found under .toolchain/sgdk/bin - is SGDK installed?" -ForegroundColor Red
    exit 1
}
if (-not (Test-Path $RomOutPath)) {
    Write-Host "  FAIL  $RomOut not found - build first (npm run build / tools\build-windows.ps1)." -ForegroundColor Red
    exit 1
}
if (-not (Test-Path $RomBinPath)) {
    Write-Host "  FAIL  $RomBin not found - build first (npm run build / tools\build-windows.ps1)." -ForegroundColor Red
    exit 1
}

# --- 1. Work-RAM budget -----------------------------------------------------
# `size` prints: header line, then "  text  data  bss  dec  hex  filename".
$sizeLine = (& $SizeExe $RomOutPath | Select-Object -Last 1).Trim() -split '\s+'
[int]$text = $sizeLine[0]
[int]$data = $sizeLine[1]
[int]$bss = $sizeLine[2]
$static = $data + $bss
$free = $RamTotal - $static

Write-Host ""
Write-Host ("  work RAM used : {0,6} / {1} bytes  ({2:P0})" -f $static, $RamTotal, ($static / $RamTotal))
Write-Host ("  work RAM free : {0,6} bytes  (stack + SGDK heap)" -f $free)
Write-Host ("  ROM code/data : {0,6} bytes  (.text, in cartridge - not work RAM)" -f $text)
Write-Host ""

if ($free -lt $MinFreeBytes) {
    Fail ("only $free bytes work-RAM free (need >= $MinFreeBytes). SGDK will likely panic " +
          "'not enough memory to reset VDP' at boot. Shrink static data: prefer `const` " +
          "(lands in ROM) over mutable globals, or compute at runtime instead of pre-baking.")
}
elseif ($free -lt $WarnFreeBytes) {
    Note ("$free bytes work-RAM free (< $WarnFreeBytes recommended). Tight - a new static " +
          "array could tip it into the SGDK boot panic.")
}
else {
    Pass ("$free bytes work-RAM free (>= $WarnFreeBytes).")
}

# --- 2. ROM binary sanity ---------------------------------------------------
$romLen = (Get-Item $RomBinPath).Length
if ($romLen -le 0) {
    Fail "$RomBin is empty."
}
elseif ($romLen % 131072 -ne 0) {
    Note "$RomBin is $romLen bytes, not 128 KB-aligned (sizebnd normally pads this)."
}
else {
    Pass ("$RomBin is $([int]($romLen / 1024)) KB, 128 KB-aligned.")
}

Write-Host ""
if ($script:failed) {
    Write-Host "Guardrails FAILED." -ForegroundColor Red
    exit 1
}
Write-Host "All guardrails passed." -ForegroundColor Green
exit 0
