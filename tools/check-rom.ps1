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
#   2. ROM binary present and 512 KB-aligned (every physical bank whole).
#   3. Banked layout (Sega SSF mapper, tools/md_banked.ld): the resident image
#      must end below the 0x280000 level window, each level pack must fit the
#      1.5 MB window, and the image must stay under BlastEm's MED_V2 limit
#      (16 MB, minus the 256 KB save buffer it keeps at the top).
# Exits non-zero on any hard failure so `npm run test` / CI go red.

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")

# MD main RAM: 0xFF0000-0xFFFFFF. Holds .data + .bss + stack + SGDK heap. text
# (.text/.rodata) lives in the cartridge ROM, not here, so it does NOT count.
$RamTotal = 65536
$LevelWindowBase = 0x280000
$LevelWindowBytes = 0x180000
# Re-baselined 2026-09-17 from 0x240000, which the 128-row DDA table and its
# clip-delta table (+6.6 KB, a correctness fix for the 22x16 viewport) pushed
# past by ~4.6 KB. 0x250000 leaves ~196 KB of real early warning before the hard
# failure at the level window -- about half a generated_wall_scalers.s, i.e. one
# asset of the size this project actually adds. A permanently-yellow check is one
# that stops being read, so move the line deliberately rather than living above
# it. The binding limit remains the hard 0x280000 below.
$ResidentWarnEnd = 0x250000
$RomMaxBytes = 16 * 1024 * 1024 - 256 * 1024

$RomOutPath = Join-Path $Root $RomOut
$RomBinPath = Join-Path $Root $RomBin
$SizeExe = Join-Path $Root ".toolchain/sgdk/bin/size.exe"
$ObjdumpExe = Join-Path $Root ".toolchain/sgdk/bin/objdump.exe"

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
# Per-section sizes, not `size`'s totals: `size` counts the banked wall packs
# as "text" and the MD's RAM image is only .data + .bss either way.
$sections = @{}
foreach ($line in (& $ObjdumpExe -h $RomOutPath)) {
    if ($line -match '^\s*\d+\s+(\S+)\s+([0-9a-f]{8})\s+([0-9a-f]{8})\s+([0-9a-f]{8})') {
        $sections[$Matches[1]] = [pscustomobject]@{
            Size = [Convert]::ToInt64($Matches[2], 16)
            Vma  = [Convert]::ToInt64($Matches[3], 16)
            Lma  = [Convert]::ToInt64($Matches[4], 16)
        }
    }
}
foreach ($required in @(".text", ".data", ".bss")) {
    if (-not $sections.ContainsKey($required)) {
        Write-Host "  FAIL  $RomOut has no $required section." -ForegroundColor Red
        exit 1
    }
}
[int]$text = $sections[".text"].Size
[int]$data = $sections[".data"].Size
[int]$bss = $sections[".bss"].Size
$static = $data + $bss
$free = $RamTotal - $static
$residentEnd = $sections[".data"].Lma + $data

Write-Host ""
Write-Host ("  work RAM used : {0,6} / {1} bytes  ({2:P0})" -f $static, $RamTotal, ($static / $RamTotal))
Write-Host ("  work RAM free : {0,6} bytes  (stack + SGDK heap)" -f $free)
Write-Host ("  ROM resident  : {0,7} bytes  (ends 0x{1:X6}; level window at 0x{2:X6})" -f $residentEnd, $residentEnd, $LevelWindowBase)
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
elseif ($romLen -gt $RomMaxBytes) {
    Fail ("$RomBin is $romLen bytes, above the $RomMaxBytes-byte SSF image limit " +
          "(BlastEm keeps a 256 KB save buffer at the top of 16 MB).")
}
elseif ($romLen % 0x80000 -ne 0) {
    Fail "$RomBin is $romLen bytes, not 512 KB-aligned: a mapper bank would be cut short."
}
else {
    Pass ("$RomBin is $([int]($romLen / 1024)) KB, 512 KB-aligned.")
}

# --- 3. Banked layout -------------------------------------------------------
if ($residentEnd -gt $LevelWindowBase) {
    Fail ("resident image ends at 0x{0:X6}, inside the banked level window at 0x{1:X6}." -f
          $residentEnd, $LevelWindowBase)
}
elseif ($residentEnd -gt $ResidentWarnEnd) {
    Note ("resident image ends at 0x{0:X6}; {1} bytes left before the level window." -f
          $residentEnd, ($LevelWindowBase - $residentEnd))
}
else {
    Pass ("resident image ends at 0x{0:X6} ({1} bytes below the level window)." -f
          $residentEnd, ($LevelWindowBase - $residentEnd))
}
$packs = @($sections.Keys | Where-Object { $_ -like ".wallpack*" } | Sort-Object)
if ($packs.Count -eq 0) {
    Fail "no .wallpackN sections: the level wall packs were not linked."
}
foreach ($name in $packs) {
    $pack = $sections[$name]
    $index = [int]($name.Substring(".wallpack".Length))
    $expectedLma = $LevelWindowBase + $index * $LevelWindowBytes
    if ($pack.Vma -ne $LevelWindowBase) {
        Fail ("$name links at 0x{0:X6}, not the level window 0x{1:X6}." -f $pack.Vma, $LevelWindowBase)
    }
    elseif ($pack.Lma -ne $expectedLma) {
        Fail ("$name loads at 0x{0:X6}, expected 0x{1:X6} (banks {2}-{3})." -f
              $pack.Lma, $expectedLma, (5 + 3 * $index), (7 + 3 * $index))
    }
    elseif ($pack.Size -ne $LevelWindowBytes) {
        Fail ("$name is $($pack.Size) bytes; packs are padded to the $LevelWindowBytes-byte window.")
    }
}
if (-not $script:failed) {
    Pass ("$($packs.Count) level packs at 0x{0:X6}, banks 5..{1}." -f
          $LevelWindowBase, (4 + 3 * $packs.Count))
}
# Map arrays (tools/bsp_emit.py, BSP_LEVEL_PACK) live in their own level's
# pack: E1M<n> in .wallpack<n-1>. Every pack links at the same address, so only
# the section name tells a misplaced array apart. The resident vis-program
# index tables (generated_bsp_vis.c) and the g_e1m<n>_map descriptors are not
# map arrays.
$mapArrays = 0
foreach ($line in (& $ObjdumpExe -t $RomOutPath)) {
    if ($line -match '\s(\S+)\s+[0-9a-f]{8}\s+(?:\.hidden\s+)?e1m(\d)_bsp_(\w+?)(?:\.lto_priv\.\d+)?$' -and
        $Matches[3] -notlike 'vis_program_*') {
        $mapArrays++
        $want = ".wallpack{0}" -f ([int]$Matches[2] - 1)
        if ($Matches[1] -ne $want) {
            $why = if ($Matches[1] -like ".wallpack*") { "another level's banks" } else { "resident ROM" }
            Fail ("e1m{0}_bsp_{1} is in {2} ({3}), not {4}." -f
                  $Matches[2], $Matches[3], $Matches[1], $why, $want)
        }
    }
}
if ($mapArrays -eq 0) {
    Fail "no e1mN_bsp_* map arrays found in the symbol table."
}
elseif (-not $script:failed) {
    Pass ("$mapArrays map arrays, each in its own level's pack.")
}
foreach ($other in $sections.Keys) {
    $sec = $sections[$other]
    if ($other -notlike ".wallpack*" -and $sec.Size -gt 0 -and
        $sec.Vma -lt 0x400000 -and ($sec.Vma + $sec.Size) -gt $LevelWindowBase -and
        $other -notmatch '^\.(debug|comment|stab)') {
        Fail ("$other occupies the level window (0x{0:X6}+{1})." -f $sec.Vma, $sec.Size)
    }
}

Write-Host ""
if ($script:failed) {
    Write-Host "Guardrails FAILED." -ForegroundColor Red
    exit 1
}
Write-Host "All guardrails passed." -ForegroundColor Green
exit 0
