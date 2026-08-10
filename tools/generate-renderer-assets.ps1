param(
    # Generated headers live beside the module that consumes them (src/<group>/).
    [string]$HudHeader = "src\renderer\generated_hud_assets.h",
    [string]$WorldHeader = "src\bsp\generated_assets.h",
    [string]$OutputHeader = "src\renderer\generated_renderer_assets.h",
    # Texture rows walked across a wall's on-screen height. The runtime masks
    # tex_y with WALL_TEX_DIM_MASK (63), so this is the vertical repeat count
    # times 64: 32 shows half the texture stretched over the wall, 64 shows it
    # exactly once, 128 twice. Doom tiles by world height -- E1M1's computer
    # corridor is 128 units tall against COMPUTE2's 56, so it shows ~2.3
    # copies, which is where its dense horizontal banding comes from.
    # 64 is the default: one full texture per wall. 128 matches Doom's density
    # more closely but makes distant walls shimmer, which is the artefact the
    # stride-4 experiment was reverted over.
    [int]$WallTexRowsPerWall = 64
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
            [void]$row.Add(([int][Math]::Floor(($relY * $WallTexRowsPerWall) / $height) -band 0xFF))
        }
        [void]$lines.Add(((Format-ByteRow @($row)) + ","))
    }
    return $lines
}

# One weapon's VRAM tileset: its idle and fire canvases sliced over the shared
# overlay rectangle and deduplicated AGAINST EACH OTHER ONLY. Per-weapon (rather
# than global) dedupe is what keeps the streaming window small: only one weapon
# is resident at a time, so sharing tiles across weapons would buy nothing and
# would force the window to hold the union of all five.
function New-WeaponTileSet([int[]]$IdlePixels, [int[]]$FirePixels) {
    $rectX = Get-DefineValue $hudText "FREEDOOM_WEAPON_RECT_X"
    $rectY = Get-DefineValue $hudText "FREEDOOM_WEAPON_RECT_Y"
    $rectW = Get-DefineValue $hudText "FREEDOOM_WEAPON_RECT_W"
    $rectH = Get-DefineValue $hudText "FREEDOOM_WEAPON_RECT_H"
    $tileX = $rectX -shr 3
    $tileY = $rectY -shr 3
    $tileW = (($rectX + $rectW - 1) -shr 3) - $tileX + 1
    $tileH = (($rectY + $rectH - 1) -shr 3) - $tileY + 1
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

# === Static ceiling atlas (Phase 2, Task 2) =================================
# One 8x8 tile per distinct (primary, secondary, secondary_coverage) ceiling
# key, byte-identical to what build_flat_rows()/write_repeated_flat_tile()
# produce at RAY_COL_STRIDE == 2 in renderer_scene.c. The renderer picks ONE
# RayFlatColor ceiling per frame and applies it to the whole viewport, so every
# full ceiling tile of a given color is identical -> one atlas tile covers all
# sectors sharing that key. Uploaded ONCE at level init; runtime just repoints
# cells at the resident atlas tile, so a sector change costs ZERO DMA. (Floor is
# ROM-constant and lives in its own tile, NOT in this atlas.)
$BAYER_4X4 = @(
    @(0, 8, 2, 10),
    @(12, 4, 14, 6),
    @(3, 11, 1, 9),
    @(15, 7, 13, 5)
)

function New-CeilingTile([int]$primary, [int]$secondary, [int]$coverage) {
    # Mirror pack_flat_row()/build_flat_rows() exactly: four Bayer rows (y=0..3),
    # each 8px line packed MSB-first at stride 2, then the four rows repeated
    # once (tile bands begin at y%4==0, see write_repeated_flat_tile).
    $four = New-Object System.Collections.Generic.List[uint32]
    for ($y = 0; $y -lt 4; $y++) {
        [uint32]$row = 0
        for ($x = 0; $x -lt 8; $x += 2) {
            $b0 = $BAYER_4X4[$y -band 3][$x -band 3]
            $b1 = $BAYER_4X4[$y -band 3][($x + 1) -band 3]
            $c0 = ($(if ($b0 -lt $coverage) { $secondary } else { $primary })) -band 0x0F
            $c1 = ($(if ($b1 -lt $coverage) { $secondary } else { $primary })) -band 0x0F
            [uint32]$pair = ([uint32]($c0 -shl 4)) -bor [uint32]$c1
            $row = ([uint32]($row -shl 8)) -bor $pair
        }
        [void]$four.Add($row)
    }
    $rows = New-Object System.Collections.Generic.List[uint32]
    for ($i = 0; $i -lt 2; $i++) { foreach ($r in $four) { [void]$rows.Add($r) } }
    return $rows
}

$sectorVisualCount = Get-DefineValue $worldText "FREEDOOM_SECTOR_VISUAL_COUNT"
$visMatch = [regex]::Match($worldText, "static const u8 FREEDOOM_SECTOR_VISUALS\[[^\]]*\]\[6\]\s*=\s*\{(?<body>.*?)\};", [Text.RegularExpressions.RegexOptions]::Singleline)
if (-not $visMatch.Success) { throw "Could not find FREEDOOM_SECTOR_VISUALS in $WorldPath" }
$visualRows = New-Object System.Collections.Generic.List[object]
foreach ($entry in [regex]::Matches($visMatch.Groups["body"].Value, "\{([^}]*)\}")) {
    $nums = New-Object System.Collections.Generic.List[int]
    foreach ($n in [regex]::Matches($entry.Groups[1].Value, '\b\d+\b')) { [void]$nums.Add([int]$n.Value) }
    [void]$visualRows.Add($nums)
}
# Dedup ceiling keys (primary, secondary, coverage) -> one atlas tile each.
$ceilingTiles = New-Object System.Collections.Generic.List[object]
$keyToIndex = @{}
$sectorCeilingIndex = New-Object System.Collections.Generic.List[int]
foreach ($v in $visualRows) {
    $key = "$($v[0]),$($v[1]),$($v[2])"
    if (-not $keyToIndex.ContainsKey($key)) {
        $keyToIndex[$key] = $ceilingTiles.Count
        [void]$ceilingTiles.Add((New-CeilingTile $v[0] $v[1] $v[2]))
    }
    [void]$sectorCeilingIndex.Add([int]$keyToIndex[$key])
}
$ceilingTileCount = $ceilingTiles.Count
$sectorCeilingIndexArray = $sectorCeilingIndex.ToArray()

$damageColor = Get-DefineValue $worldText "MEGALDOOM_WORLD_COLOR_DAMAGE"
$warningColor = Get-DefineValue $worldText "MEGALDOOM_WORLD_COLOR_WARNING"
# FREEDOOM_WEAPON_IDLE/FIRE are [FREEDOOM_WEAPON_COUNT][H][W]; Get-ArrayValues
# flattens the whole thing, so slice one canvas per weapon back out.
$weaponCount = Get-DefineValue $hudText "FREEDOOM_WEAPON_COUNT"
$idlePixels = Get-ArrayValues $hudText "FREEDOOM_WEAPON_IDLE"
$firePixels = Get-ArrayValues $hudText "FREEDOOM_WEAPON_FIRE"
$canvasStride = $viewPxW * $viewPxH
foreach ($name in @("IDLE", "FIRE")) {
    $actual = $(if ($name -eq "IDLE") { $idlePixels.Count } else { $firePixels.Count })
    if ($actual -ne ($canvasStride * $weaponCount)) {
        throw "FREEDOOM_WEAPON_$name has $actual values, expected $($canvasStride * $weaponCount)"
    }
}
$weaponSets = @()
for ($w = 0; $w -lt $weaponCount; $w++) {
    $start = $w * $canvasStride
    $weaponSets += ,(New-WeaponTileSet `
        $idlePixels[$start..($start + $canvasStride - 1)] `
        $firePixels[$start..($start + $canvasStride - 1)])
}
$weaponTiles = $weaponSets[0]
$weaponMaxTileCount = ($weaponSets | ForEach-Object { $_.Tiles.Count } | Measure-Object -Maximum).Maximum
foreach ($set in $weaponSets) {
    if (($set.TileX -ne $weaponTiles.TileX) -or ($set.TileY -ne $weaponTiles.TileY) -or
        ($set.TileW -ne $weaponTiles.TileW) -or ($set.TileH -ne $weaponTiles.TileH)) {
        throw "All weapons must bake into the same overlay tile rectangle"
    }
}
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
[void]$lines.Add("#define MEGALDOOM_WEAPON_COUNT $weaponCount")
[void]$lines.Add("// Only ONE weapon is resident in VRAM at a time: renderer_set_weapon() DMAs")
[void]$lines.Add("// the selected weapon's tiles into a fixed MEGALDOOM_WEAPON_MAX_TILE_COUNT")
[void]$lines.Add("// window at WEAPON_TILE_BASE. Sizing that window to the largest weapon is why")
[void]$lines.Add("// five weapons fit in the 72 tiles below the SGDK font region.")
[void]$lines.Add("#define MEGALDOOM_WEAPON_MAX_TILE_COUNT $weaponMaxTileCount")
[void]$lines.Add("static const u16 MEGALDOOM_WEAPON_TILE_COUNTS[MEGALDOOM_WEAPON_COUNT] = {")
[void]$lines.Add("    " + (($weaponSets | ForEach-Object { $_.Tiles.Count.ToString() }) -join ", "))
[void]$lines.Add("};")
[void]$lines.Add("// Unused slots in a weapon's row are zero-filled; only the first")
[void]$lines.Add("// MEGALDOOM_WEAPON_TILE_COUNTS[weapon] tiles are ever uploaded.")
[void]$lines.Add("static const u32 MEGALDOOM_WEAPON_TILES[MEGALDOOM_WEAPON_COUNT][MEGALDOOM_WEAPON_MAX_TILE_COUNT][8] = {")
foreach ($set in $weaponSets) {
    [void]$lines.Add("  {")
    foreach ($tile in $set.Tiles) {
        [void]$lines.Add(((Format-U32Row $tile) + ","))
    }
    for ($pad = $set.Tiles.Count; $pad -lt $weaponMaxTileCount; $pad++) {
        [void]$lines.Add(((Format-U32Row @(0, 0, 0, 0, 0, 0, 0, 0)) + ","))
    }
    [void]$lines.Add("  },")
}
[void]$lines.Add("};")
[void]$lines.Add("// [weapon][0] = idle pose, [weapon][1] = fire pose. 65535 = transparent cell.")
[void]$lines.Add("static const u16 MEGALDOOM_WEAPON_TILEMAP[MEGALDOOM_WEAPON_COUNT][2][MEGALDOOM_WEAPON_TILE_W * MEGALDOOM_WEAPON_TILE_H] = {")
foreach ($set in $weaponSets) {
    [void]$lines.Add("  {")
    [void]$lines.Add(("    {" + (($set.Maps[0] | ForEach-Object { $_.ToString() }) -join ", ") + "},"))
    [void]$lines.Add(("    {" + (($set.Maps[1] | ForEach-Object { $_.ToString() }) -join ", ") + "},"))
    [void]$lines.Add("  },")
}
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
[void]$lines.Add((("#define MEGALDOOM_CEILING_TILE_COUNT {0}" -f $ceilingTileCount)))
[void]$lines.Add("static const u32 MEGALDOOM_CEILING_TILES[MEGALDOOM_CEILING_TILE_COUNT][8] = {")
foreach ($tile in $ceilingTiles) {
    [void]$lines.Add(((Format-U32Row $tile) + ","))
}
[void]$lines.Add("};")
[void]$lines.Add("")
[void]$lines.Add("#define MEGALDOOM_SECTOR_VISUAL_COUNT $sectorVisualCount")
[void]$lines.Add("static const u8 MEGALDOOM_SECTOR_CEILING_TILE_INDEX[MEGALDOOM_SECTOR_VISUAL_COUNT] = {")
[void]$lines.Add(((($sectorCeilingIndexArray | ForEach-Object { $_.ToString() }) -join ", ") + ","))
[void]$lines.Add("};")
[void]$lines.Add("")
[void]$lines.Add("#endif")

Set-Content -Path $OutPath -Value ($lines -join "`r`n") -NoNewline
Write-Host "Generated $OutPath from $HudPath" -ForegroundColor Green
