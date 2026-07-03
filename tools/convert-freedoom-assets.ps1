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
$FaceSourcePath = Join-Path $Root $FacePath
$BillboardSourcePath = Join-Path $Root $BillboardPath
$BillboardKeySourcePath = Join-Path $Root $BillboardKeyPath
$BillboardDecorSourcePath = Join-Path $Root $BillboardDecorPath
$BillboardEnemySourcePath = Join-Path $Root $BillboardEnemyPath
$OutPath = Join-Path $Root "src\generated_assets.h"
$BillboardOutPath = Join-Path $Root "src\generated_billboard_assets.h"
$HudOutPath = Join-Path $Root "src\generated_hud_assets.h"
$WeaponOverlayW = 160
$WeaponOverlayH = 120
$WeaponDrawW = 72
$WeaponDrawH = 54
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

Add-Type -AssemblyName System.Drawing

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

function Convert-Image([string]$Path, [int]$Width, [int]$Height, [bool]$UseAlphaTransparency) {
    $image = [System.Drawing.Bitmap]::new($Path)
    $rows = New-Object System.Collections.Generic.List[string]

    try {
        for ($y = 0; $y -lt $Height; $y++) {
            $values = New-Object System.Collections.Generic.List[string]

            for ($x = 0; $x -lt $Width; $x++) {
                $srcX = [Math]::Min($image.Width - 1, [int](($x * $image.Width) / $Width))
                $srcY = [Math]::Min($image.Height - 1, [int](($y * $image.Height) / $Height))
                $pixel = $image.GetPixel($srcX, $srcY)
                $index = 0

                if ((-not $UseAlphaTransparency) -or ($pixel.A -ge 128)) {
                    $index = Get-NearestPaletteIndex $pixel
                    if ($UseAlphaTransparency -and ($index -eq 0)) {
                        $index = 2
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

function Get-WeaponPaletteIndex([System.Drawing.Color]$Color, [bool]$FireFrame) {
    # Transparency is decided ONLY by alpha. Dark opaque pixels (the pistol's
    # metal/shadow) must stay solid, otherwise the gun renders see-through.
    if ($Color.A -lt 128) {
        return 0
    }

    if ($FireFrame -and ($Color.R -gt 180) -and ($Color.G -gt 120)) {
        return 11
    }

    # Warm/skin-dominant (the pistol hand). Map to BROWN tones (13 lit, 10 shadow),
    # never to the saturated red index 12 - a shadowed hand was turning red.
    if (($Color.R -gt $Color.G + 24) -and ($Color.R -gt $Color.B + 32)) {
        if ($Color.R -gt 160) { return 13 }
        return 10
    }

    if (($Color.B -gt $Color.R + 12) -and ($Color.B -gt $Color.G + 8)) {
        return 8
    }

    $lum = [int](($Color.R * 30 + $Color.G * 59 + $Color.B * 11) / 100)
    if ($lum -lt 55) { return 2 }
    if ($lum -lt 90) { return 3 }
    if ($lum -lt 130) { return 4 }
    if ($lum -lt 175) { return 5 }
    if ($lum -lt 215) { return 6 }
    return 7
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

$wallRows = Convert-Image $SourcePath 16 16 $false
$wallBrownRows = Convert-Image $WallBrownSourcePath 16 16 $false
$wallGrayRows = Convert-Image $WallGraySourcePath 16 16 $false
$wallMetalRows = Convert-Image $WallMetalSourcePath 16 16 $false
$wallBrickRows = Convert-Image $WallBrickSourcePath 16 16 $false
$wallTechRows = Convert-Image $WallTechSourcePath 16 16 $false
$doorRows = Convert-Image $DoorSourcePath 16 16 $false
$lockedDoorRows = Convert-Image $LockedDoorSourcePath 16 16 $false
$switchRows = Convert-Image $SwitchSourcePath 16 16 $false
$weaponIdleRows = Convert-WeaponOverlay $WeaponIdleSourcePath $false
$weaponFireRows = Convert-WeaponOverlay $WeaponFireSourcePath $true
$hudTiles = Convert-HudTiles $HudSourcePath
$faceTiles = Convert-FaceFrames
$faceFrameCount = $FaceFrameNames.Count
$facePaletteRgb = ($facePalette | ForEach-Object {
    "0x{0:X2}{1:X2}{2:X2}" -f $_[0], $_[1], $_[2]
}) -join ", "
$billboardRows = Convert-Image $BillboardSourcePath 16 16 $true
$billboardKeyRows = Convert-Image $BillboardKeySourcePath 16 16 $true
$billboardDecorRows = Convert-Image $BillboardDecorSourcePath 16 16 $true
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
$relativeFaceSource = $FacePath.Replace("\", "/")
$relativeBillboardSource = $BillboardPath.Replace("\", "/")
$relativeBillboardKeySource = $BillboardKeyPath.Replace("\", "/")
$relativeBillboardDecorSource = $BillboardDecorPath.Replace("\", "/")
$relativeBillboardEnemySource = $BillboardEnemyPath.Replace("\", "/")
$generatedAt = (Get-Date).ToString("yyyy-MM-ddTHH:mm:ssK")

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
// Weapon fire source: $relativeWeaponFireSource
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

if ($BillboardOnly) {
    Write-Host "Generated $BillboardOutPath from $BillboardPath, $BillboardKeyPath and $BillboardDecorPath" -ForegroundColor Green
    return
}

$content = @"
#ifndef MEGALDOOM_GENERATED_ASSETS_H
#define MEGALDOOM_GENERATED_ASSETS_H

#include <genesis.h>

// Generated by tools/convert-freedoom-assets.ps1.
// Stone wall source: $relativeSource
// Brown wall source: $relativeWallBrownSource
// Gray wall source: $relativeWallGraySource
// Metal wall source: $relativeWallMetalSource
// Brick wall source: $relativeWallBrickSource
// Tech wall source: $relativeWallTechSource
// Door source: $relativeDoorSource
// Locked door source: $relativeLockedDoorSource
// Switch source: $relativeSwitchSource
// Generated at: $generatedAt
static const u8 FREEDOOM_WALL_TEXTURE[16][16] = {
$($wallRows -join ",`r`n")
};

static const u8 FREEDOOM_WALL_BROWN_TEXTURE[16][16] = {
$($wallBrownRows -join ",`r`n")
};

static const u8 FREEDOOM_WALL_GRAY_TEXTURE[16][16] = {
$($wallGrayRows -join ",`r`n")
};

static const u8 FREEDOOM_WALL_METAL_TEXTURE[16][16] = {
$($wallMetalRows -join ",`r`n")
};

static const u8 FREEDOOM_WALL_BRICK_TEXTURE[16][16] = {
$($wallBrickRows -join ",`r`n")
};

static const u8 FREEDOOM_WALL_TECH_TEXTURE[16][16] = {
$($wallTechRows -join ",`r`n")
};

static const u8 FREEDOOM_DOOR_TEXTURE[16][16] = {
$($doorRows -join ",`r`n")
};

static const u8 FREEDOOM_LOCKED_DOOR_TEXTURE[16][16] = {
$($lockedDoorRows -join ",`r`n")
};

static const u8 FREEDOOM_SWITCH_TEXTURE[16][16] = {
$($switchRows -join ",`r`n")
};

#endif
"@

try {
    Set-Content -Path $OutPath -Value $content -NoNewline
    Write-Host "Generated $OutPath from $TexturePath" -ForegroundColor Green
} catch {
    Write-Warning "Could not update $OutPath. The existing wall texture header was left unchanged. $($_.Exception.Message)"
}
Write-Host "Generated $BillboardOutPath from $BillboardPath, $BillboardKeyPath and $BillboardDecorPath" -ForegroundColor Green
