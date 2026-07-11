param(
    [string]$TexturePath = "res\originaldoom\textures\STONE.png",
    [string]$WallBrownTexturePath = "res\originaldoom\textures\BROWN1.png",
    [string]$WallGrayTexturePath = "res\originaldoom\textures\GRAY7.png",
    [string]$WallMetalTexturePath = "res\originaldoom\textures\METAL1.png",
    [string]$WallBrickTexturePath = "res\originaldoom\textures\STONE2.png",
    [string]$WallTechTexturePath = "res\originaldoom\textures\TEKWALL4.png",
    [string]$DoorTexturePath = "res\originaldoom\textures\DOOR3.png",
    [string]$LockedDoorTexturePath = "res\originaldoom\textures\BIGDOOR2.png",
    [string]$SwitchTexturePath = "res\originaldoom\textures\SW1COMP.png",
    [string]$HudPath = "res\originaldoom\graphics\STBAR.png",
    [string]$WeaponIdlePath = "res\originaldoom\sprites\PISGA0.png",
    [string]$WeaponFirePath = "res\originaldoom\sprites\PISGB0.png",
    [string]$WeaponFlashPath = "res\originaldoom\sprites\PISFA0.png",
    [string]$FacePath = "res\originaldoom\graphics\STFST01.png",
    [string]$BillboardPath = "res\originaldoom\sprites\BON1A0.png",
    [string]$BillboardKeyPath = "res\originaldoom\sprites\BKEYA0.png",
    [string]$BillboardDecorPath = "res\originaldoom\sprites\BAR1A0.png",
    [string]$BillboardEnemyPath = "res\originaldoom\sprites\POSSA1.png",
    [int]$BillboardEnemyW = 24,
    [int]$BillboardEnemyH = 48,
    [switch]$BillboardOnly
)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$SourcePath = Join-Path $Root $TexturePath
$WallBrownSourcePath = Join-Path $Root $WallBrownTexturePath
$WallGraySourcePath = Join-Path $Root $WallGrayTexturePath
$WallMetalSourcePath = Join-Path $Root $WallMetalTexturePath
$WallBrickSourcePath = Join-Path $Root $WallBrickTexturePath
$WallTechSourcePath = Join-Path $Root $WallTechTexturePath
$DoorSourcePath = Join-Path $Root $DoorTexturePath
$LockedDoorSourcePath = Join-Path $Root $LockedDoorTexturePath
$SwitchSourcePath = Join-Path $Root $SwitchTexturePath
$HudSourcePath = Join-Path $Root $HudPath
$WeaponIdleSourcePath = Join-Path $Root $WeaponIdlePath
$WeaponFireSourcePath = Join-Path $Root $WeaponFirePath
$WeaponFlashSourcePath = Join-Path $Root $WeaponFlashPath
$FaceSourcePath = Join-Path $Root $FacePath
$BillboardSourcePath = Join-Path $Root $BillboardPath
$BillboardKeySourcePath = Join-Path $Root $BillboardKeyPath
$BillboardDecorSourcePath = Join-Path $Root $BillboardDecorPath
$BillboardEnemySourcePath = Join-Path $Root $BillboardEnemyPath
$OutPath = Join-Path $Root "src\generated_assets.h"
$BillboardOutPath = Join-Path $Root "src\generated_billboard_assets.h"
$HudOutPath = Join-Path $Root "src\generated_hud_assets.h"
$MapOutPath = Join-Path $Root "src\generated_e1m1_map.c"
$WeaponOverlayW = 160
$WeaponOverlayH = 120
# Pistol draw box inside the 160x120 view. Was 72x54 (~45% of the viewport in
# each axis), which read as oversized against the 3D scene. 50x38 (~31% / ~32%)
# keeps it anchored bottom-centre but visibly smaller. WeaponDrawX/Y are derived
# so the gun stays centred horizontally and pinned to the bottom of the view.
$WeaponDrawW = 50
$WeaponDrawH = 38
$WeaponDrawX = [int](($WeaponOverlayW - $WeaponDrawW) / 2)
$WeaponDrawY = $WeaponOverlayH - $WeaponDrawH

if (-not (Test-Path $SourcePath)) {
    throw "Texture source not found: $SourcePath"
}
if (-not (Test-Path $WallBrownSourcePath)) {
    throw "Brown wall texture source not found: $WallBrownSourcePath"
}
if (-not (Test-Path $WallGraySourcePath)) {
    throw "Gray wall texture source not found: $WallGraySourcePath"
}
if (-not (Test-Path $WallMetalSourcePath)) {
    throw "Metal wall texture source not found: $WallMetalSourcePath"
}
if (-not (Test-Path $WallBrickSourcePath)) {
    throw "Brick wall texture source not found: $WallBrickSourcePath"
}
if (-not (Test-Path $WallTechSourcePath)) {
    throw "Tech wall texture source not found: $WallTechSourcePath"
}
if (-not (Test-Path $DoorSourcePath)) {
    throw "Door texture source not found: $DoorSourcePath"
}
if (-not (Test-Path $LockedDoorSourcePath)) {
    throw "Locked door texture source not found: $LockedDoorSourcePath"
}
if (-not (Test-Path $SwitchSourcePath)) {
    throw "Switch texture source not found: $SwitchSourcePath"
}
if (-not (Test-Path $HudSourcePath)) {
    throw "HUD source not found: $HudSourcePath"
}
if (-not (Test-Path $WeaponIdleSourcePath)) {
    throw "Weapon idle source not found: $WeaponIdleSourcePath"
}
if (-not (Test-Path $WeaponFireSourcePath)) {
    throw "Weapon fire source not found: $WeaponFireSourcePath"
}
if (-not (Test-Path $WeaponFlashSourcePath)) {
    throw "Weapon flash source not found: $WeaponFlashSourcePath"
}
if (-not (Test-Path $FaceSourcePath)) {
    throw "Face source not found: $FaceSourcePath"
}
if (-not (Test-Path $BillboardSourcePath)) {
    throw "Billboard source not found: $BillboardSourcePath"
}
if (-not (Test-Path $BillboardKeySourcePath)) {
    throw "Billboard key source not found: $BillboardKeySourcePath"
}
if (-not (Test-Path $BillboardDecorSourcePath)) {
    throw "Billboard decor source not found: $BillboardDecorSourcePath"
}
if (-not (Test-Path $BillboardEnemySourcePath)) {
    throw "Billboard enemy source not found: $BillboardEnemySourcePath"
}

# Curated E1M1 item/prop atlas. Every entry is baked into the same compact 24x48
# billboard canvas; transparent padding preserves the original sprite silhouette
# while keeping the 68000 renderer's texture lookup branch-free.
$BillboardWorldSpecs = @(
    @{ Name = "BONUS"; Path = "res\originaldoom\sprites\BON1A0.png" },
    @{ Name = "BLUE_KEY"; Path = "res\originaldoom\sprites\BKEYA0.png" },
    @{ Name = "YELLOW_KEY"; Path = "res\originaldoom\sprites\YKEYA0.png" },
    @{ Name = "RED_KEY"; Path = "res\originaldoom\sprites\RKEYA0.png" },
    @{ Name = "STIMPACK"; Path = "res\originaldoom\sprites\STIMA0.png" },
    @{ Name = "MEDIKIT"; Path = "res\originaldoom\sprites\MEDIA0.png" },
    @{ Name = "GREEN_ARMOR"; Path = "res\originaldoom\sprites\ARM1A0.png" },
    @{ Name = "BLUE_ARMOR"; Path = "res\originaldoom\sprites\ARM2A0.png" },
    @{ Name = "CLIP"; Path = "res\originaldoom\sprites\CLIPA0.png" },
    @{ Name = "AMMO_BOX"; Path = "res\originaldoom\sprites\AMMOA0.png" },
    @{ Name = "CANDLE"; Path = "res\originaldoom\sprites\CANDA0.png" },
    @{ Name = "CANDELABRA"; Path = "res\originaldoom\sprites\CBRAA0.png" },
    @{ Name = "COLUMN"; Path = "res\originaldoom\sprites\COLUA0.png" },
    @{ Name = "BARREL"; Path = "res\originaldoom\sprites\BAR1A0.png" },
    @{ Name = "TREE"; Path = "res\originaldoom\sprites\TREDA0.png" }
)
foreach ($spec in $BillboardWorldSpecs) {
    if (-not (Test-Path (Join-Path $Root $spec.Path))) {
        throw "Billboard world sprite not found: $($spec.Path)"
    }
}

Add-Type -AssemblyName System.Drawing

# The WAD generator owns the exact E1M1 wall catalog, PAL3 palette and sector
# colors. Generate it first so weapon and billboard conversion can target the
# same 16-color line used by the dynamic view tiles.
& python (Join-Path $PSScriptRoot "wad-map-extract.py") `
    --wad (Join-Path $Root "DOOM1.WAD") `
    --map E1M1 `
    --out $MapOutPath `
    --assets-out $OutPath
if ($LASTEXITCODE -ne 0) {
    throw "E1M1 map/world asset generation failed."
}

$worldHeaderText = Get-Content -Raw $OutPath
$worldPaletteMatch = [regex]::Match(
    $worldHeaderText,
    'static const u32 FREEDOOM_WORLD_PALETTE\[16\]\s*=\s*\{(?<body>.*?)\};',
    [Text.RegularExpressions.RegexOptions]::Singleline)
if (-not $worldPaletteMatch.Success) {
    throw "Could not read FREEDOOM_WORLD_PALETTE from $OutPath"
}
$worldPalette = @([regex]::Matches($worldPaletteMatch.Groups["body"].Value, '0x[0-9A-Fa-f]{6}') |
    ForEach-Object {
        $rgb = [Convert]::ToInt32($_.Value.Substring(2), 16)
        ,@((($rgb -shr 16) -band 0xFF), (($rgb -shr 8) -band 0xFF), ($rgb -band 0xFF))
    })
if ($worldPalette.Count -ne 16) {
    throw "Expected 16 PAL3 colors, found $($worldPalette.Count)."
}

$palette = @(
    @(0x00, 0x00, 0x00),
    @(0xD8, 0xD8, 0xD8),
    @(0x18, 0x14, 0x10),
    @(0x38, 0x30, 0x30),
    @(0x58, 0x50, 0x48),
    @(0x88, 0x80, 0x78),
    @(0xB4, 0xAC, 0xA0),
    @(0xE8, 0xE0, 0xD0),
    @(0x30, 0x1E, 0x10),
    @(0x48, 0x78, 0xA8),
    @(0x78, 0x50, 0x2C),
    @(0xD8, 0xB0, 0x48),
    @(0x98, 0x28, 0x18),
    @(0xA8, 0x68, 0x38),
    @(0x48, 0x40, 0x38),
    @(0x4C, 0x60, 0x28)
)

# Dedicated 16-colour palette for the Doom-guy portrait (rendered on PAL2). The
# shared world palette turns skin gold; this ramp keeps proper flesh/brown/red
# tones. Index 1 is the recessed-slot fill so the block blends into the bar.
# Keep in sync with the PAL2 load in renderer.c (emitted as FREEDOOM_FACE_PALETTE).
$facePalette = @(
    @(0x00, 0x00, 0x00),
    @(0x38, 0x30, 0x30),
    @(0x18, 0x10, 0x0C),
    @(0x40, 0x28, 0x18),
    @(0x60, 0x3C, 0x24),
    @(0x88, 0x54, 0x30),
    @(0xB0, 0x74, 0x48),
    @(0xD8, 0x9C, 0x68),
    @(0xF0, 0xC8, 0x98),
    @(0x60, 0x20, 0x18),
    @(0x98, 0x28, 0x18),
    @(0x30, 0x20, 0x14),
    @(0x58, 0x3C, 0x24),
    @(0xC0, 0xC0, 0xB8),
    @(0x80, 0x78, 0x70),
    @(0xF0, 0xE8, 0xD8)
)
$FacePaletteFillIndex = 1

function Get-NearestIndexInPalette([System.Drawing.Color]$Color, $Palette) {
    $bestIndex = 0
    $bestDistance = [int]::MaxValue

    for ($i = 0; $i -lt $Palette.Count; $i++) {
        $dr = [int]$Color.R - $Palette[$i][0]
        $dg = [int]$Color.G - $Palette[$i][1]
        $db = [int]$Color.B - $Palette[$i][2]
        $distance = ($dr * $dr) + ($dg * $dg) + ($db * $db)

        if ($distance -lt $bestDistance) {
            $bestDistance = $distance
            $bestIndex = $i
        }
    }

    return $bestIndex
}

function Get-NearestPaletteIndex([System.Drawing.Color]$Color) {
    return Get-NearestIndexInPalette $Color $palette
}

function Get-NearestWorldPaletteIndex([System.Drawing.Color]$Color, [bool]$AllowTransparent = $true) {
    $start = if ($AllowTransparent) { 0 } else { 1 }
    $bestIndex = $start
    $bestDistance = [int]::MaxValue
    for ($i = $start; $i -lt $worldPalette.Count; $i++) {
        $dr = [int]$Color.R - $worldPalette[$i][0]
        $dg = [int]$Color.G - $worldPalette[$i][1]
        $db = [int]$Color.B - $worldPalette[$i][2]
        $distance = ($dr * $dr) + ($dg * $dg) + ($db * $db)
        if ($distance -lt $bestDistance) {
            $bestDistance = $distance
            $bestIndex = $i
        }
    }
    return $bestIndex
}

function Convert-Image([string]$Path, [int]$Width, [int]$Height, [bool]$UseAlphaTransparency,
                       [bool]$PreserveAspect = $false) {
    # Area/box downscale: each output texel is the AVERAGE of the full source
    # rectangle it covers, then quantized to the palette. Point sampling (grabbing
    # a single source pixel) aliased high-frequency Doom textures into random-looking
    # texels; averaging first preserves the texture's overall structure so a 128x128
    # STONE/BROWN reads as a real (if low-res) wall instead of static.
    $image = [System.Drawing.Bitmap]::new($Path)
    $rows = New-Object System.Collections.Generic.List[string]
    $drawWidth = $Width
    $drawHeight = $Height
    $drawX = 0
    $drawY = 0

    if ($PreserveAspect) {
        $scale = [Math]::Min($Width / $image.Width, $Height / $image.Height)
        $drawWidth = [Math]::Max(1, [int][Math]::Round($image.Width * $scale))
        $drawHeight = [Math]::Max(1, [int][Math]::Round($image.Height * $scale))
        $drawX = [int](($Width - $drawWidth) / 2)
        $drawY = [int](($Height - $drawHeight) / 2)
    }

    try {
        for ($y = 0; $y -lt $Height; $y++) {
            $values = New-Object System.Collections.Generic.List[string]

            if (($y -lt $drawY) -or ($y -ge ($drawY + $drawHeight))) {
                for ($x = 0; $x -lt $Width; $x++) { $values.Add("0") }
                $rows.Add("    {" + ($values -join ", ") + "}")
                continue
            }
            $localY = $y - $drawY

            # Floor (not [int], which rounds) and clamp the start so upscaling small
            # sprites can never push the source rect past the image edge (empty rect
            # -> divide-by-zero).
            $sy0 = [Math]::Min($image.Height - 1, [int][Math]::Floor(($localY * $image.Height) / $drawHeight))
            $sy1 = [int][Math]::Floor((($localY + 1) * $image.Height) / $drawHeight)
            if ($sy1 -le $sy0) { $sy1 = $sy0 + 1 }
            if ($sy1 -gt $image.Height) { $sy1 = $image.Height }

            for ($x = 0; $x -lt $Width; $x++) {
                if (($x -lt $drawX) -or ($x -ge ($drawX + $drawWidth))) {
                    $values.Add("0")
                    continue
                }
                $localX = $x - $drawX
                $sx0 = [Math]::Min($image.Width - 1, [int][Math]::Floor(($localX * $image.Width) / $drawWidth))
                $sx1 = [int][Math]::Floor((($localX + 1) * $image.Width) / $drawWidth)
                if ($sx1 -le $sx0) { $sx1 = $sx0 + 1 }
                if ($sx1 -gt $image.Width) { $sx1 = $image.Width }

                $sumR = 0; $sumG = 0; $sumB = 0; $sumA = 0; $count = 0
                $opR = 0; $opG = 0; $opB = 0; $opCount = 0

                for ($sy = $sy0; $sy -lt $sy1; $sy++) {
                    for ($sx = $sx0; $sx -lt $sx1; $sx++) {
                        $p = $image.GetPixel($sx, $sy)
                        $sumR += $p.R; $sumG += $p.G; $sumB += $p.B; $sumA += $p.A
                        $count++
                        if ($p.A -ge 128) {
                            $opR += $p.R; $opG += $p.G; $opB += $p.B; $opCount++
                        }
                    }
                }

                $index = 0

                if (-not $UseAlphaTransparency) {
                    $avg = [System.Drawing.Color]::FromArgb(
                        [int]($sumR / $count), [int]($sumG / $count), [int]($sumB / $count))
                    $index = Get-NearestWorldPaletteIndex $avg $true
                } else {
                    # Sprites: a texel is transparent only when the covered area is
                    # mostly transparent. Otherwise average the OPAQUE pixels so edge
                    # texels are not darkened toward black by transparent neighbours.
                    $avgA = $sumA / $count
                    if ($avgA -ge 128) {
                        if ($opCount -gt 0) {
                            $avg = [System.Drawing.Color]::FromArgb(
                                [int]($opR / $opCount), [int]($opG / $opCount), [int]($opB / $opCount))
                        } else {
                            $avg = [System.Drawing.Color]::FromArgb(
                                [int]($sumR / $count), [int]($sumG / $count), [int]($sumB / $count))
                        }
                        $index = Get-NearestWorldPaletteIndex $avg $false
                    }
                }

                $values.Add($index.ToString())
            }

            $rows.Add("    {" + ($values -join ", ") + "}")
        }
    } finally {
        $image.Dispose()
    }

    return $rows
}

function Convert-ImageTiles([string]$Path, [int]$Width, [int]$Height) {
    $image = [System.Drawing.Bitmap]::new($Path)
    $tiles = New-Object System.Collections.Generic.List[string]

    try {
        for ($tileY = 0; $tileY -lt ($Height / 8); $tileY++) {
            for ($tileX = 0; $tileX -lt ($Width / 8); $tileX++) {
                $rows = New-Object System.Collections.Generic.List[string]

                for ($row = 0; $row -lt 8; $row++) {
                    $packed = [uint32]0

                    for ($col = 0; $col -lt 8; $col++) {
                        $x = ($tileX * 8) + $col
                        $y = ($tileY * 8) + $row
                        $srcX = [Math]::Min($image.Width - 1, [int](($x * $image.Width) / $Width))
                        $srcY = [Math]::Min($image.Height - 1, [int](($y * $image.Height) / $Height))
                        $index = Get-NearestPaletteIndex $image.GetPixel($srcX, $srcY)
                        $packed = ($packed -shl 4) -bor ($index -band 0x0F)
                    }

                    $rows.Add(("0x{0:X8}" -f $packed))
                }

                $tiles.Add("    {" + ($rows -join ", ") + "}")
            }
        }
    } finally {
        $image.Dispose()
    }

    return $tiles
}

# Sprite leftOffset/topOffset from the WAD picture headers, written by
# wad-extract.py as res/originaldoom/sprites/_offsets.json. Doom weapon and
# muzzle-flash sprites are separate patches positioned by these offsets; the PNGs
# carry no offset metadata, so the fire-frame composite needs them to land the
# flash on the barrel. The hardcoded fallback covers DOOM1.WAD v1.9 pistol
# sprites if the sidecar is absent.
$SpriteOffsetsPath = Join-Path $Root "res\originaldoom\sprites\_offsets.json"
$spriteOffsets = $null
if (Test-Path $SpriteOffsetsPath) {
    $spriteOffsets = Get-Content -Raw $SpriteOffsetsPath | ConvertFrom-Json
}
function Get-SpriteOffset([string]$Name) {
    if ($spriteOffsets -and ($spriteOffsets.PSObject.Properties.Name -contains $Name)) {
        return $spriteOffsets.$Name
    }
    switch ($Name) {
        "PISGA0" { return [pscustomobject]@{ width = 57; height = 62; leftOffset = -126; topOffset = -106 } }
        "PISGB0" { return [pscustomobject]@{ width = 79; height = 82; leftOffset = -104; topOffset = -86 } }
        "PISFA0" { return [pscustomobject]@{ width = 41; height = 38; leftOffset = -140; topOffset = -66 } }
        default  { return $null }
    }
}

function Get-WeaponPaletteIndex([System.Drawing.Color]$Color, [bool]$FireFrame) {
    # Transparency is decided ONLY by alpha. Dark opaque pixels (the pistol's
    # metal/shadow) must stay solid, otherwise the gun renders see-through.
    if ($Color.A -lt 128) {
        return 0
    }

    if ($FireFrame) {
        # Muzzle-flash warm ramp, mapped onto PAL3 (FREEDOOM_WORLD_PALETTE). This
        # branch only ever sees flash pixels: the gun body is coloured by
        # Get-NearestWorldPaletteIndex directly in Convert-WeaponOverlayFire.
        #   13 = light gray (white-hot core), 15 = gold (yellow),
        #   14 = bright red, 8 = red, 2 = dark red.
        $r = [int]$Color.R; $g = [int]$Color.G; $b = [int]$Color.B
        if (($r -gt 225) -and ($g -gt 210) -and ($b -gt 160)) { return 13 }
        if (($r -gt 190) -and ($g -gt 125)) { return 15 }
        if (($r -gt 150) -and ($g -lt 100)) { return 14 }
        if (($r -gt 100) -and ($g -lt 60))  { return 8 }
        if (($r -gt 70)  -and ($g -lt 40))  { return 2 }
    }
    return Get-NearestWorldPaletteIndex $Color $false
}

function Convert-WeaponOverlay([string]$Path, [bool]$FireFrame) {
    $image = [System.Drawing.Bitmap]::new($Path)
    $rows = New-Object System.Collections.Generic.List[string]
    $drawWidth = $WeaponDrawW
    $drawHeight = $WeaponDrawH
    $offsetX = $WeaponDrawX
    $offsetY = $WeaponDrawY

    try {
        for ($y = 0; $y -lt $WeaponOverlayH; $y++) {
            $values = New-Object System.Collections.Generic.List[string]

            for ($x = 0; $x -lt $WeaponOverlayW; $x++) {
                $index = 0

                if (($x -ge $offsetX) -and ($x -lt ($offsetX + $drawWidth)) -and
                    ($y -ge $offsetY) -and ($y -lt ($offsetY + $drawHeight))) {
                    $localX = $x - $offsetX
                    $localY = $y - $offsetY
                    $srcX = [Math]::Min($image.Width - 1, [int](($localX * $image.Width) / $drawWidth))
                    $srcY = [Math]::Min($image.Height - 1, [int](($localY * $image.Height) / $drawHeight))
                    $index = Get-WeaponPaletteIndex $image.GetPixel($srcX, $srcY) $FireFrame
                }

                $values.Add($index.ToString())
            }

            $rows.Add("    {" + ($values -join ", ") + "}")
        }
    } finally {
        $image.Dispose()
    }

    return $rows
}

# Build the FIRE weapon overlay: the recoiling pistol (PISGB0) with the Doom
# muzzle-flash sprite (PISFA0) composited on top using each sprite's WAD
# leftOffset/topOffset so the flash lands on the barrel. Both sprites share one
# psprite anchor; placing each so its (leftOffset, topOffset) point sits on that
# anchor aligns the flash to the muzzle exactly as in original Doom. The shared
# canvas is then point-sampled into the same 72x54 draw box as the idle frame.
function Convert-WeaponOverlayFire([string]$GunPath, [string]$FlashPath) {
    $gunImage   = [System.Drawing.Bitmap]::new($GunPath)
    $flashImage = [System.Drawing.Bitmap]::new($FlashPath)
    $rows = New-Object System.Collections.Generic.List[string]
    $drawWidth  = $WeaponDrawW
    $drawHeight = $WeaponDrawH
    $offsetX    = $WeaponDrawX
    $offsetY    = $WeaponDrawY

    $gunOffset = Get-SpriteOffset "PISGB0"
    $flOffset  = Get-SpriteOffset "PISFA0"
    $gunW = $gunImage.Width;   $gunH = $gunImage.Height
    $flW  = $flashImage.Width; $flH  = $flashImage.Height

    # Shared psprite anchor = origin (0,0). Sprite pixel (px,py) -> (px - leftOffset, py - topOffset).
    $gunOX = -$gunOffset.leftOffset; $gunOY = -$gunOffset.topOffset
    $flOX  = -$flOffset.leftOffset;  $flOY  = -$flOffset.topOffset
    $canvasX0 = [Math]::Min($gunOX, $flOX)
    $canvasY0 = [Math]::Min($gunOY, $flOY)
    $canvasW  = [Math]::Max($gunOX + $gunW, $flOX + $flW) - $canvasX0
    $canvasH  = [Math]::Max($gunOY + $gunH, $flOY + $flH) - $canvasY0
    $gunPlaceX = $gunOX - $canvasX0; $gunPlaceY = $gunOY - $canvasY0
    $flPlaceX  = $flOX  - $canvasX0; $flPlaceY  = $flOY  - $canvasY0

    try {
        for ($y = 0; $y -lt $WeaponOverlayH; $y++) {
            $values = New-Object System.Collections.Generic.List[string]
            for ($x = 0; $x -lt $WeaponOverlayW; $x++) {
                $index = 0
                if (($x -ge $offsetX) -and ($x -lt ($offsetX + $drawWidth)) -and
                    ($y -ge $offsetY) -and ($y -lt ($offsetY + $drawHeight))) {
                    $localX = $x - $offsetX
                    $localY = $y - $offsetY
                    $compX = [Math]::Min($canvasW - 1, [int](($localX * $canvasW) / $drawWidth))
                    $compY = [Math]::Min($canvasH - 1, [int](($localY * $canvasH) / $drawHeight))

                    # Recoiling gun -> normal world palette (same colour path as idle).
                    $gx = $compX - $gunPlaceX
                    $gy = $compY - $gunPlaceY
                    if (($gx -ge 0) -and ($gx -lt $gunW) -and ($gy -ge 0) -and ($gy -lt $gunH)) {
                        $gp = $gunImage.GetPixel($gx, $gy)
                        if ($gp.A -ge 128) {
                            $index = Get-NearestWorldPaletteIndex $gp $false
                        }
                    }

                    # Muzzle flash on top -> warm ramp (fire red/yellow/white).
                    $fx = $compX - $flPlaceX
                    $fy = $compY - $flPlaceY
                    if (($fx -ge 0) -and ($fx -lt $flW) -and ($fy -ge 0) -and ($fy -lt $flH)) {
                        $fp = $flashImage.GetPixel($fx, $fy)
                        if ($fp.A -ge 128) {
                            $index = Get-WeaponPaletteIndex $fp $true
                        }
                    }
                }
                $values.Add($index.ToString())
            }
            $rows.Add("    {" + ($values -join ", ") + "}")
        }
    } finally {
        $gunImage.Dispose()
        $flashImage.Dispose()
    }
    return $rows
}

function Get-HudPaletteIndex([System.Drawing.Color]$Color) {
    $index = Get-NearestPaletteIndex $Color

    if (($index -eq 12) -or ($index -eq 13) -or ($index -eq 14) -or ($index -eq 15)) {
        $lum = [int](($Color.R * 30 + $Color.G * 59 + $Color.B * 11) / 100)
        if ($lum -lt 65) { return 2 }
        if ($lum -lt 105) { return 3 }
        if ($lum -lt 150) { return 4 }
        if ($lum -lt 195) { return 5 }
        return 6
    }

    return $index
}

function Convert-HudTiles([string]$Path) {
    $image = [System.Drawing.Bitmap]::new($Path)
    $tiles = New-Object System.Collections.Generic.List[string]
    $Width = 256
    $Height = 56

    try {
        for ($tileY = 0; $tileY -lt ($Height / 8); $tileY++) {
            for ($tileX = 0; $tileX -lt ($Width / 8); $tileX++) {
                $rows = New-Object System.Collections.Generic.List[string]

                for ($row = 0; $row -lt 8; $row++) {
                    $packed = [uint32]0

                    for ($col = 0; $col -lt 8; $col++) {
                        $x = ($tileX * 8) + $col
                        $y = ($tileY * 8) + $row
                        $index = 3

                        if (($y -ge 12) -and ($y -lt 44)) {
                            $srcX = [Math]::Min($image.Width - 1, [int](($x * $image.Width) / $Width))
                            $srcY = [Math]::Min($image.Height - 1, [int](($y - 12) * $image.Height / 32))
                            $index = Get-HudPaletteIndex $image.GetPixel($srcX, $srcY)
                        } elseif (($y -eq 10) -or ($y -eq 45)) {
                            $index = 5
                        } elseif (($y -gt 10) -and ($y -lt 45)) {
                            $index = 2
                        }

                        $packed = ($packed -shl 4) -bor ($index -band 0x0F)
                    }

                    $rows.Add(("0x{0:X8}" -f $packed))
                }

                $tiles.Add("    {" + ($rows -join ", ") + "}")
            }
        }
    } finally {
        $image.Dispose()
    }

    return $tiles
}

# Doom-guy face (STF*). Packs each sprite into a 24x32 (3x4 tile) block so it
# drops straight into the recessed face slot of the status bar. The sprite is
# CENTERED in the block; surrounding pixels are filled with the slot's recessed
# panel colour (palette index 3). Unlike Convert-HudTiles this keeps
# skin/red/brown indices (no luminance desaturation) so the portrait stays
# coloured. Convert-FaceFrame bakes one sprite -> 12 tile strings.
$FaceTileW = 3
$FaceTileH = 4
$FaceBgIndex = $FacePaletteFillIndex

# Animated portrait frames. Order MUST match the index #defines emitted below and
# the compute_face_frame() selector in renderer_hud.c:
#   15x STFST{bracket}{dir}  bracket 0..4 (high->low HP), dir 0=right 1=centre 2=left
#    5x STFOUCH{bracket}     pain grimace per HP bracket
#    1x STFDEAD0             dead
$FaceGraphicsDir = "res\originaldoom\graphics"
$FaceFrameNames = New-Object System.Collections.Generic.List[string]
for ($b = 0; $b -lt 5; $b++) {
    for ($d = 0; $d -lt 3; $d++) { $FaceFrameNames.Add("STFST$b$d") }
}
for ($b = 0; $b -lt 5; $b++) { $FaceFrameNames.Add("STFOUCH$b") }
$FaceFrameNames.Add("STFDEAD0")

function Convert-FaceFrame([string]$Path) {
    $image = [System.Drawing.Bitmap]::new($Path)
    $tiles = New-Object System.Collections.Generic.List[string]
    $blockW = $FaceTileW * 8
    $blockH = $FaceTileH * 8
    # Centre the sprite inside the 24x32 block (clamped so it never starts < 0).
    $offsetX = [Math]::Max(0, [int](($blockW - $image.Width) / 2))
    $offsetY = [Math]::Max(0, [int](($blockH - $image.Height) / 2))

    try {
        for ($tileY = 0; $tileY -lt $FaceTileH; $tileY++) {
            for ($tileX = 0; $tileX -lt $FaceTileW; $tileX++) {
                $rows = New-Object System.Collections.Generic.List[string]

                for ($row = 0; $row -lt 8; $row++) {
                    $packed = [uint32]0

                    for ($col = 0; $col -lt 8; $col++) {
                        $x = ($tileX * 8) + $col
                        $y = ($tileY * 8) + $row
                        $index = $FaceBgIndex
                        $srcX = $x - $offsetX
                        $srcY = $y - $offsetY

                        if (($srcX -ge 0) -and ($srcY -ge 0) -and
                            ($srcX -lt $image.Width) -and ($srcY -lt $image.Height)) {
                            $pixel = $image.GetPixel($srcX, $srcY)
                            if ($pixel.A -ge 128) {
                                $index = Get-NearestIndexInPalette $pixel $facePalette
                            }
                        }

                        $packed = ($packed -shl 4) -bor ($index -band 0x0F)
                    }

                    $rows.Add(("0x{0:X8}" -f $packed))
                }

                $tiles.Add("    {" + ($rows -join ", ") + "}")
            }
        }
    } finally {
        $image.Dispose()
    }

    return $tiles
}

function Convert-FaceFrames() {
    $allTiles = New-Object System.Collections.Generic.List[string]
    foreach ($name in $FaceFrameNames) {
        $framePath = Join-Path $Root (Join-Path $FaceGraphicsDir "$name.png")
        if (-not (Test-Path $framePath)) {
            throw "Face frame source not found: $framePath"
        }
        foreach ($tile in (Convert-FaceFrame $framePath)) {
            $allTiles.Add($tile)
        }
    }
    return $allTiles
}

$weaponIdleRows = Convert-WeaponOverlay $WeaponIdleSourcePath $false
$weaponFireRows = Convert-WeaponOverlayFire $WeaponFireSourcePath $WeaponFlashSourcePath
$hudTiles = Convert-HudTiles $HudSourcePath
$faceTiles = Convert-FaceFrames
$faceFrameCount = $FaceFrameNames.Count
$facePaletteRgb = ($facePalette | ForEach-Object {
    "0x{0:X2}{1:X2}{2:X2}" -f $_[0], $_[1], $_[2]
}) -join ", "
$billboardRows = Convert-Image $BillboardSourcePath 16 16 $true
$billboardKeyRows = Convert-Image $BillboardKeySourcePath 16 16 $true
$billboardDecorRows = Convert-Image $BillboardDecorSourcePath 16 16 $true
$billboardWorldW = 24
$billboardWorldH = 48
$billboardWorldBlocks = New-Object System.Collections.Generic.List[string]
foreach ($spec in $BillboardWorldSpecs) {
    $rows = Convert-Image (Join-Path $Root $spec.Path) $billboardWorldW $billboardWorldH $true $true
    $billboardWorldBlocks.Add("    {" + "`r`n" + ($rows -join ",`r`n") + "`r`n    }")
}
# Enemy (zombieman) animation frames, all scaled into the same 24x48 box.
# Order MUST match the pose indices in src/billboard_internal.h:
#   0..3 walk (POSSA1..D1), 4 attack (POSSF1), 5..9 death (POSSH0..L0, L0 = corpse).
$EnemyFrameNames = @("POSSA1", "POSSB1", "POSSC1", "POSSD1", "POSSF1",
                     "POSSH0", "POSSI0", "POSSJ0", "POSSK0", "POSSL0")
$EnemySpritesDir = "res\originaldoom\sprites"
$enemyFrameBlocks = New-Object System.Collections.Generic.List[string]
foreach ($name in $EnemyFrameNames) {
    $enemyFramePath = Join-Path $Root (Join-Path $EnemySpritesDir "$name.png")
    if (-not (Test-Path $enemyFramePath)) {
        throw "Enemy frame source not found: $enemyFramePath"
    }
    $enemyFrameRows = Convert-Image $enemyFramePath $BillboardEnemyW $BillboardEnemyH $true
    $enemyFrameBlocks.Add("    {" + "`r`n" + ($enemyFrameRows -join ",`r`n") + "`r`n    }")
}
$enemyFrameCount = $EnemyFrameNames.Count
$relativeSource = $TexturePath.Replace("\", "/")
$relativeWallBrownSource = $WallBrownTexturePath.Replace("\", "/")
$relativeWallGraySource = $WallGrayTexturePath.Replace("\", "/")
$relativeWallMetalSource = $WallMetalTexturePath.Replace("\", "/")
$relativeWallBrickSource = $WallBrickTexturePath.Replace("\", "/")
$relativeWallTechSource = $WallTechTexturePath.Replace("\", "/")
$relativeDoorSource = $DoorTexturePath.Replace("\", "/")
$relativeLockedDoorSource = $LockedDoorTexturePath.Replace("\", "/")
$relativeSwitchSource = $SwitchTexturePath.Replace("\", "/")
$relativeHudSource = $HudPath.Replace("\", "/")
$relativeWeaponIdleSource = $WeaponIdlePath.Replace("\", "/")
$relativeWeaponFireSource = $WeaponFirePath.Replace("\", "/")
$relativeWeaponFlashSource = $WeaponFlashPath.Replace("\", "/")
$relativeFaceSource = $FacePath.Replace("\", "/")
$relativeBillboardSource = $BillboardPath.Replace("\", "/")
$relativeBillboardKeySource = $BillboardKeyPath.Replace("\", "/")
$relativeBillboardDecorSource = $BillboardDecorPath.Replace("\", "/")
$relativeBillboardEnemySource = $BillboardEnemyPath.Replace("\", "/")
$generatedAt = "source-derived"

$billboardContent = @"
#ifndef MEGALDOOM_GENERATED_BILLBOARD_ASSETS_H
#define MEGALDOOM_GENERATED_BILLBOARD_ASSETS_H

#include <genesis.h>

// Generated by tools/convert-freedoom-assets.ps1.
// Bonus billboard source: $relativeBillboardSource
// Key billboard source: $relativeBillboardKeySource
// Decor billboard source: $relativeBillboardDecorSource
// Enemy billboard frames: $($EnemyFrameNames -join ", ") (from $EnemySpritesDir)
// Generated at: $generatedAt
// Palette index 0 is transparent for billboard rendering.
static const u8 FREEDOOM_BILLBOARD_BONUS_TEXTURE[16][16] = {
$($billboardRows -join ",`r`n")
};

static const u8 FREEDOOM_BILLBOARD_KEY_TEXTURE[16][16] = {
$($billboardKeyRows -join ",`r`n")
};

static const u8 FREEDOOM_BILLBOARD_DECOR_TEXTURE[16][16] = {
$($billboardDecorRows -join ",`r`n")
};

#define FREEDOOM_BILLBOARD_WORLD_W $billboardWorldW
#define FREEDOOM_BILLBOARD_WORLD_H $billboardWorldH
#define FREEDOOM_BILLBOARD_WORLD_TEXTURE_COUNT $($BillboardWorldSpecs.Count)
// $((($BillboardWorldSpecs | ForEach-Object { $_.Name }) -join ", "))
static const u8 FREEDOOM_BILLBOARD_WORLD_TEXTURES[FREEDOOM_BILLBOARD_WORLD_TEXTURE_COUNT][FREEDOOM_BILLBOARD_WORLD_H][FREEDOOM_BILLBOARD_WORLD_W] = {
$($billboardWorldBlocks -join ",`r`n")
};

#define FREEDOOM_BILLBOARD_ENEMY_W $BillboardEnemyW
#define FREEDOOM_BILLBOARD_ENEMY_H $BillboardEnemyH
#define FREEDOOM_BILLBOARD_ENEMY_FRAME_COUNT $enemyFrameCount

// Enemy poses indexed by frame: 0..3 walk, 4 attack, 5..9 death (9 = corpse).
static const u8 FREEDOOM_BILLBOARD_ENEMY_FRAMES[FREEDOOM_BILLBOARD_ENEMY_FRAME_COUNT][$BillboardEnemyH][$BillboardEnemyW] = {
$($enemyFrameBlocks -join ",`r`n")
};

// Back-compat alias: frame 0 is the standing/idle pose (POSSA1).
#define FREEDOOM_BILLBOARD_ENEMY_TEXTURE FREEDOOM_BILLBOARD_ENEMY_FRAMES[0]

#endif
"@

Set-Content -Path $BillboardOutPath -Value $billboardContent -NoNewline

$hudContent = @"
#ifndef MEGALDOOM_GENERATED_HUD_ASSETS_H
#define MEGALDOOM_GENERATED_HUD_ASSETS_H

#include <genesis.h>

#define FREEDOOM_HUD_TILE_W 32
#define FREEDOOM_HUD_TILE_H 7
#define FREEDOOM_HUD_TILE_COUNT 224
#define FREEDOOM_WEAPON_W $WeaponOverlayW
#define FREEDOOM_WEAPON_H $WeaponOverlayH
#define FREEDOOM_WEAPON_DRAW_X $WeaponDrawX
#define FREEDOOM_WEAPON_DRAW_Y $WeaponDrawY
#define FREEDOOM_WEAPON_DRAW_W $WeaponDrawW
#define FREEDOOM_WEAPON_DRAW_H $WeaponDrawH
#define FREEDOOM_FACE_TILE_W $FaceTileW
#define FREEDOOM_FACE_TILE_H $FaceTileH
#define FREEDOOM_FACE_FRAME_TILES $($FaceTileW * $FaceTileH)
#define FREEDOOM_FACE_FRAME_COUNT $faceFrameCount
#define FREEDOOM_FACE_TILE_COUNT $($FaceTileW * $FaceTileH * $faceFrameCount)

// Portrait frame indices (order matches the baker's frame list and
// compute_face_frame() in renderer_hud.c). Bracket 0 = high HP, 4 = low HP.
// FACE_FRAME_ST(bracket, dir): dir 0 = look right, 1 = centre, 2 = look left.
#define FACE_FRAME_ST(bracket, dir) (((bracket) * 3) + (dir))
#define FACE_FRAME_OUCH(bracket)    (15 + (bracket))
#define FACE_FRAME_DEAD             20

// Generated by tools/convert-freedoom-assets.ps1.
// HUD source: $relativeHudSource
// Weapon idle source: $relativeWeaponIdleSource
// Weapon fire source: $relativeWeaponFireSource (gun) + $relativeWeaponFlashSource (muzzle flash)
// Face source: $relativeFaceSource (animated set baked from res/originaldoom/graphics/STF*.png)
// Generated at: $generatedAt
static const u32 FREEDOOM_HUD_TILES[FREEDOOM_HUD_TILE_COUNT][8] = {
$($hudTiles -join ",`r`n")
};

// Dedicated 16-colour palette for the portrait (load into PAL2). Skin/brown/red
// ramp so the face does not turn gold under the shared world palette. Index 1 is
// the recessed-slot fill. Values are 0xRRGGBB; pass through RGB24_TO_VDPCOLOR.
static const u32 FREEDOOM_FACE_PALETTE[16] = { $facePaletteRgb };

// Doom-guy animated portrait: $faceFrameCount frames x $($FaceTileW * $FaceTileH) tiles, laid out
// consecutively. Frame f occupies tiles [f*12 .. f*12+11]; centred in a 3x4 block.
// Rendered with PAL2 (FREEDOOM_FACE_PALETTE).
static const u32 FREEDOOM_FACE_TILES[FREEDOOM_FACE_TILE_COUNT][8] = {
$($faceTiles -join ",`r`n")
};

static const u8 FREEDOOM_WEAPON_IDLE[FREEDOOM_WEAPON_H][FREEDOOM_WEAPON_W] = {
$($weaponIdleRows -join ",`r`n")
};

static const u8 FREEDOOM_WEAPON_FIRE[FREEDOOM_WEAPON_H][FREEDOOM_WEAPON_W] = {
$($weaponFireRows -join ",`r`n")
};

#endif
"@

Set-Content -Path $HudOutPath -Value $hudContent -NoNewline

& (Join-Path $PSScriptRoot "generate-renderer-assets.ps1")
if (-not $?) {
    throw "Renderer asset generation failed."
}

if ($BillboardOnly) {
    Write-Host "Generated $BillboardOutPath from $BillboardPath, $BillboardKeyPath and $BillboardDecorPath" -ForegroundColor Green
    return
}
Write-Host "Generated $OutPath and $MapOutPath from DOOM1.WAD" -ForegroundColor Green
Write-Host "Generated $BillboardOutPath from $BillboardPath, $BillboardKeyPath and $BillboardDecorPath" -ForegroundColor Green
