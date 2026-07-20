param(
    [string]$HudHeader = "src\generated_hud_assets.h",
    [string]$WorldHeader = "src\generated_assets.h",
    [string]$OutputHeader = "src\generated_renderer_assets.h"
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$HudPath = Join-Path $Root $HudHeader
$WorldPath = Join-Path $Root $WorldHeader
$OutPath = Join-Path $Root $OutputHeader

if (-not (Test-Path $HudPath)) {
    throw "Generated HUD header not found: $HudPath"
}
if (-not (Test-Path $WorldPath)) {
    throw "Generated world header not found: $WorldPath"
}

function Get-DefineValue([string]$Text, [string]$Name) {
    $match = [regex]::Match($Text, "#define\s+$Name\s+(\d+)")
    if (-not $match.Success) { throw "Could not find $Name in $WorldPath" }
    return [int]$match.Groups[1].Value
}

function Get-ArrayValues([string]$Text, [string]$Name) {
    $pattern = "static const u8 $Name\[[^=]+\]\s*=\s*\{(?<body>.*?)\};"
    $match = [regex]::Match($Text, $pattern, [Text.RegularExpressions.RegexOptions]::Singleline)
    if (-not $match.Success) {
        throw "Could not find $Name in $HudPath"
    }

    $values = New-Object System.Collections.Generic.List[int]
    foreach ($number in [regex]::Matches($match.Groups["body"].Value, '\b\d+\b')) {
        [void]$values.Add([int]$number.Value)
    }
    return @($values)
}

function Format-ByteRow([int[]]$Values) {
    return "    {" + (($Values | ForEach-Object { $_.ToString() }) -join ", ") + "}"
}

function Format-U32Row([uint32[]]$Values) {
    return "    {" + (($Values | ForEach-Object { "0x{0:X8}" -f $_ }) -join ", ") + "}"
}

function Format-FlatU16([int[]]$Values) {
    return "    " + (($Values | ForEach-Object { $_.ToString() }) -join ", ")
}

function Format-FlatU32([uint32[]]$Values) {
    return "    " + (($Values | ForEach-Object { "0x{0:X8}" -f $_ }) -join ", ")
}

function New-PairTileRows {
    $lines = New-Object System.Collections.Generic.List[string]
    for ($left = 0; $left -lt 16; $left++) {
        for ($right = 0; $right -lt 16; $right++) {
            $digits = (($left.ToString("X")) * 4) + (($right.ToString("X")) * 4)
            $row = [uint32]::Parse($digits, [Globalization.NumberStyles]::HexNumber)
            [void]$lines.Add(((Format-U32Row @($row, $row, $row, $row, $row, $row, $row, $row)) + ","))
        }
    }
    return $lines
}

function New-WallSamplingRows {
    $lines = New-Object System.Collections.Generic.List[string]
    [void]$lines.Add(((Format-ByteRow (1..120 | ForEach-Object { 0 })) + ","))
    for ($height = 1; $height -le 120; $height++) {
        $row = New-Object System.Collections.Generic.List[int]
        for ($relY = 0; $relY -lt 120; $relY++) {
            [void]$row.Add(([int][Math]::Floor(($relY * 32) / $height) -band 0xFF))
        }
        [void]$lines.Add(((Format-ByteRow @($row)) + ","))
    }
    return $lines
}

function New-WeaponOps([int[]]$Pixels) {
    $dst = New-Object System.Collections.Generic.List[int]
    $value = New-Object System.Collections.Generic.List[uint32]
    $clearMask = New-Object System.Collections.Generic.List[uint32]
    # Single source of truth: the draw box is owned by convert-freedoom-assets.ps1
    # (which writes FREEDOOM_WEAPON_DRAW_* into generated_hud_assets.h and scales
    # the sprite into that box). Reading the defines back here keeps the per-tile
    # ops in sync with the baked pixels instead of duplicating 44/66/72/54.
    $drawX = Get-DefineValue $hudText "FREEDOOM_WEAPON_DRAW_X"
    $drawY = Get-DefineValue $hudText "FREEDOOM_WEAPON_DRAW_Y"
    $drawW = Get-DefineValue $hudText "FREEDOOM_WEAPON_DRAW_W"
    $drawH = Get-DefineValue $hudText "FREEDOOM_WEAPON_DRAW_H"
    $tileXStart = $drawX -shr 3
    $tileXEnd = ($drawX + $drawW - 1) -shr 3

    for ($y = $drawY; $y -lt ($drawY + $drawH); $y++) {
        $tileY = $y -shr 3
        $rowY = $y % 8
        for ($tileX = $tileXStart; $tileX -le $tileXEnd; $tileX++) {
            $x0 = $tileX * 8
            $xBegin = [Math]::Max($x0, $drawX)
            $xStop = [Math]::Min($x0 + 8, $drawX + $drawW)
            [uint32]$packed = 0
            [uint32]$mask = 0
            for ($x = $xBegin; $x -lt $xStop; $x++) {
                $texel = $Pixels[($y * $viewPxW) + $x]
                if ($texel -ne 0) {
                    $shift = (7 - ($x % 8)) * 4
                    $packed = $packed -bor ([uint32]($texel -band 0x0F) -shl $shift)
                    $mask = $mask -bor ([uint32]0x0F -shl $shift)
                }
            }
            if ($packed -ne 0) {
                [void]$dst.Add((($tileY * $viewTileW + $tileX) * 8) + $rowY)
                [void]$value.Add($packed)
                [void]$clearMask.Add($mask)
            }
        }
    }

    # Self-assert: each generated mask must equal the runtime nibble-expansion
    # the old draw_weapon_overlay reconstructed, so the optimized table-lookup
    # draw loop is image-exact. The old expression is (t<<4)-t in unsigned 32-bit
    # (t = nonzero-nibble markers); reproduce the u32 wraparound via int64 + mask.
    for ($i = 0; $i -lt $value.Count; $i++) {
        $val = $value[$i]
        $t = [uint32](($val -bor ($val -shr 1) -bor ($val -shr 2) -bor ($val -shr 3)) -band 0x11111111u)
        $shifted = ($t -shl 4) -band 0xFFFFFFFFu
        $expected = [uint32]((([int64]$shifted) - ([int64]$t)) -band 0xFFFFFFFFu)
        if ($clearMask[$i] -ne $expected) {
            throw ("Weapon clear-mask mismatch at op {0}: generated=0x{1:X8} expected=0x{2:X8}" -f $i, $clearMask[$i], $expected)
        }
    }

    $count = $dst.Count
    return [pscustomobject]@{
        Count = $count
        Dst = $dst.ToArray()
        Value = $value.ToArray()
        Mask = $clearMask.ToArray()
    }
}

function New-WeaponTileSet([int[]]$IdlePixels, [int[]]$FirePixels) {
    $drawX = Get-DefineValue $hudText "FREEDOOM_WEAPON_DRAW_X"
    $drawY = Get-DefineValue $hudText "FREEDOOM_WEAPON_DRAW_Y"
    $drawW = Get-DefineValue $hudText "FREEDOOM_WEAPON_DRAW_W"
    $drawH = Get-DefineValue $hudText "FREEDOOM_WEAPON_DRAW_H"
    $tileX = $drawX -shr 3
    $tileY = $drawY -shr 3
    $tileW = (($drawX + $drawW - 1) -shr 3) - $tileX + 1
    $tileH = (($drawY + $drawH - 1) -shr 3) - $tileY + 1
    $unique = New-Object System.Collections.Generic.List[object]
    $indexByRows = @{}
    $maps = @()

    foreach ($pixels in @($IdlePixels, $FirePixels)) {
        $map = New-Object System.Collections.Generic.List[int]
        for ($localY = 0; $localY -lt $tileH; $localY++) {
            for ($localX = 0; $localX -lt $tileW; $localX++) {
                [uint32[]]$rows = @(0, 0, 0, 0, 0, 0, 0, 0)
                for ($row = 0; $row -lt 8; $row++) {
                    [uint32]$packed = 0
                    $py = (($tileY + $localY) * 8) + $row
                    for ($col = 0; $col -lt 8; $col++) {
                        $px = (($tileX + $localX) * 8) + $col
                        $texel = 0
                        if ($px -ge 0 -and $px -lt $viewPxW -and
                            $py -ge 0 -and $py -lt $viewPxH) {
                            $texel = $pixels[($py * $viewPxW) + $px] -band 0x0F
                        }
                        $packed = [uint32](($packed -shl 4) -bor $texel)
                    }
                    $rows[$row] = $packed
                }
                $key = ($rows | ForEach-Object { $_.ToString("X8") }) -join ""
                if ($key -eq ("00000000" * 8)) {
                    [void]$map.Add(0xFFFF)
                } else {
                    if (-not $indexByRows.ContainsKey($key)) {
                        $indexByRows[$key] = $unique.Count
                        [void]$unique.Add($rows)
                    }
                    [void]$map.Add([int]$indexByRows[$key])
                }
            }
        }
        $maps += ,$map.ToArray()
    }

    return [pscustomobject]@{
        TileX = $tileX
        TileY = $tileY
        TileW = $tileW
        TileH = $tileH
        Tiles = $unique.ToArray()
        Maps = $maps
    }
}

function New-OverlayOps([string]$Kind, [int]$Color) {
    $masks = @{}
    $values = @{}

    function Add-Pixel([int]$X, [int]$Y) {
        # Column-major u32 element offset into the flat g_view_tiles[][8] array.
        # g_view_tiles is laid out as [tile_x * VIEW_TILE_H + tile_y][row]
        # (see view_tile_index in renderer_internal.h). Each tile owns 8 u32
        # rows, so the flat element offset is tile_index*8 + (Y%8), matching
        # draw_overlay_ops which does (u32*)base + op->dst.
        $tileX = $X -shr 3
        $tileY = $Y -shr 3
        $tileIndex = ($tileX * $viewTileH) + $tileY
        $dst = ($tileIndex * 8) + ($Y % 8)
        $shift = (7 - ($X % 8)) * 4
        $mask = [uint32]0x0F -shl $shift
        $value = [uint32]$Color -shl $shift
        if (-not $masks.ContainsKey($dst)) {
            [void]($masks[$dst] = [uint32]0)
            [void]($values[$dst] = [uint32]0)
        }
        [void]($masks[$dst] = [uint32]($masks[$dst] -bor $mask))
        [void]($values[$dst] = [uint32]($values[$dst] -bor $value))
    }

    if ($Kind -eq "damage") {
        for ($x = 0; $x -lt $viewPxW; $x++) {
            for ($t = 0; $t -lt 3; $t++) {
                Add-Pixel $x $t
                Add-Pixel $x ($viewPxH - 1 - $t)
            }
        }
        for ($y = 0; $y -lt $viewPxH; $y++) {
            for ($t = 0; $t -lt 8; $t++) {
                Add-Pixel $t $y
                Add-Pixel ($viewPxW - 1 - $t) $y
            }
        }
    } else {
        for ($x = 16; $x -le 32; $x++) {
            Add-Pixel $x 1
            Add-Pixel $x 2
            Add-Pixel $x 117
            Add-Pixel $x 118
        }
        for ($x = 127; $x -le 143; $x++) {
            Add-Pixel $x 1
            Add-Pixel $x 2
            Add-Pixel $x 117
            Add-Pixel $x 118
        }
    }

    $ordered = @($masks.Keys | Sort-Object {[int]$_})
    $maxOps = 384
    $dst = New-Object System.Collections.Generic.List[int]
    $mask = New-Object System.Collections.Generic.List[uint32]
    $value = New-Object System.Collections.Generic.List[uint32]
    foreach ($key in $ordered) {
        [void]$dst.Add([int]$key)
        [void]$mask.Add($masks[$key])
        [void]$value.Add($values[$key])
    }
    $count = $dst.Count
    return [pscustomobject]@{
        Count = $count
        Dst = $dst.ToArray()
        Mask = $mask.ToArray()
        Value = $value.ToArray()
    }
}

function Format-OverlayRow([int[]]$Dst, [uint32[]]$Mask, [uint32[]]$Value, [int]$Index) {
    return ("    {{ {0}, 0x{1:X8}, 0x{2:X8} }}" -f $Dst[$Index], $Mask[$Index], $Value[$Index])
}

$hudText = Get-Content -Raw $HudPath
$worldText = Get-Content -Raw $WorldPath

# View geometry, derived from the HUD header so this generator can never drift
# from the C layout. FREEDOOM_WEAPON_W/H ARE the viewport pixel dimensions (the
# weapon overlay canvas == the view), and the tile-grid dimensions
# (VIEW_TILE_W/H in renderer_internal.h == RAY_VIEW_TILE_W/H in raycast.h) are
# viewPxW/8 and viewPxH/8. g_view_tiles is COLUMN-MAJOR:
# [tile_x * VIEW_TILE_H + tile_y] (see view_tile_index), so the damage/low-health
# overlay ops now emit that column-major flat index.
$viewPxW   = Get-DefineValue $hudText "FREEDOOM_WEAPON_W"
$viewPxH   = Get-DefineValue $hudText "FREEDOOM_WEAPON_H"
$viewTileW = [int]($viewPxW / 8)
$viewTileH = [int]($viewPxH / 8)
$damageColor = Get-DefineValue $worldText "MEGALDOOM_WORLD_COLOR_DAMAGE"
$warningColor = Get-DefineValue $worldText "MEGALDOOM_WORLD_COLOR_WARNING"
$idlePixels = Get-ArrayValues $hudText "FREEDOOM_WEAPON_IDLE"
$firePixels = Get-ArrayValues $hudText "FREEDOOM_WEAPON_FIRE"
$weaponTiles = New-WeaponTileSet $idlePixels $firePixels
$damageOps = New-OverlayOps "damage" $damageColor
$lowHealthOps = New-OverlayOps "low_health" $warningColor

$lines = New-Object System.Collections.Generic.List[string]
[void]$lines.Add("#ifndef MEGALDOOM_GENERATED_RENDERER_ASSETS_H")
[void]$lines.Add("#define MEGALDOOM_GENERATED_RENDERER_ASSETS_H")
[void]$lines.Add("")
[void]$lines.Add("#include <genesis.h>")
[void]$lines.Add("")
[void]$lines.Add("// Generated by tools/generate-renderer-assets.ps1 from generated_hud_assets.h.")
[void]$lines.Add("// All data in this header is immutable and therefore stored in cartridge ROM.")
[void]$lines.Add("")
[void]$lines.Add("static const u8 MEGALDOOM_BILLBOARD_REMAP[6][16] = {")
[void]$lines.Add("    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},")
[void]$lines.Add("    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},")
[void]$lines.Add("    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},")
[void]$lines.Add("    {0, $damageColor, $damageColor, $damageColor, $damageColor, $damageColor, $damageColor, $damageColor, $damageColor, $damageColor, $damageColor, $damageColor, $damageColor, $damageColor, $damageColor, $damageColor},")
[void]$lines.Add("    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},")
[void]$lines.Add("    {0, $damageColor, 2, $damageColor, 2, $damageColor, 2, $damageColor, 2, $damageColor, 2, $damageColor, 2, $damageColor, 2, $damageColor},")
[void]$lines.Add("};")
[void]$lines.Add("")
[void]$lines.Add("static const u32 MEGALDOOM_PAIR_TILES[256][8] = {")
foreach ($line in (New-PairTileRows)) { [void]$lines.Add($line) }
[void]$lines.Add("};")
[void]$lines.Add("")
[void]$lines.Add("static const u8 MEGALDOOM_WALL_TEX_Y_BY_HEIGHT[121][120] = {")
foreach ($line in (New-WallSamplingRows)) { [void]$lines.Add($line) }
[void]$lines.Add("};")
[void]$lines.Add("")
[void]$lines.Add("#define MEGALDOOM_WEAPON_TILE_X $($weaponTiles.TileX)")
[void]$lines.Add("#define MEGALDOOM_WEAPON_TILE_Y $($weaponTiles.TileY)")
[void]$lines.Add("#define MEGALDOOM_WEAPON_TILE_W $($weaponTiles.TileW)")
[void]$lines.Add("#define MEGALDOOM_WEAPON_TILE_H $($weaponTiles.TileH)")
[void]$lines.Add("#define MEGALDOOM_WEAPON_TILE_COUNT $($weaponTiles.Tiles.Count)")
[void]$lines.Add("static const u32 MEGALDOOM_WEAPON_TILES[MEGALDOOM_WEAPON_TILE_COUNT][8] = {")
foreach ($tile in $weaponTiles.Tiles) {
    [void]$lines.Add(((Format-U32Row $tile) + ","))
}
[void]$lines.Add("};")
[void]$lines.Add("static const u16 MEGALDOOM_WEAPON_TILEMAP[2][MEGALDOOM_WEAPON_TILE_W * MEGALDOOM_WEAPON_TILE_H] = {")
[void]$lines.Add(("    {" + (($weaponTiles.Maps[0] | ForEach-Object { $_.ToString() }) -join ", ") + "},"))
[void]$lines.Add(("    {" + (($weaponTiles.Maps[1] | ForEach-Object { $_.ToString() }) -join ", ") + "},"))
[void]$lines.Add("};")
[void]$lines.Add("")
[void]$lines.Add("typedef struct {")
[void]$lines.Add("    u16 dst;")
[void]$lines.Add("    u32 clear_mask;")
[void]$lines.Add("    u32 value;")
[void]$lines.Add("} MegalDoomOverlayRowOp;")
[void]$lines.Add("")
[void]$lines.Add("#define MEGALDOOM_OVERLAY_OP_MAX 512")
[void]$lines.Add("static const MegalDoomOverlayRowOp MEGALDOOM_DAMAGE_OVERLAY_OPS[] = {")
for ($i = 0; $i -lt $damageOps.Count; $i++) {
    [void]$lines.Add(((Format-OverlayRow $damageOps.Dst $damageOps.Mask $damageOps.Value $i) + ","))
}
[void]$lines.Add("};")
[void]$lines.Add("")
[void]$lines.Add("static const MegalDoomOverlayRowOp MEGALDOOM_LOW_HEALTH_OVERLAY_OPS[] = {")
for ($i = 0; $i -lt $lowHealthOps.Count; $i++) {
    [void]$lines.Add(((Format-OverlayRow $lowHealthOps.Dst $lowHealthOps.Mask $lowHealthOps.Value $i) + ","))
}
[void]$lines.Add("};")
[void]$lines.Add("")
[void]$lines.Add("static const u16 MEGALDOOM_OVERLAY_OP_COUNT[2] = {$($damageOps.Count), $($lowHealthOps.Count)};")
[void]$lines.Add("")
[void]$lines.Add("#endif")

Set-Content -Path $OutPath -Value ($lines -join "`r`n") -NoNewline
Write-Host "Generated $OutPath from $HudPath" -ForegroundColor Green
