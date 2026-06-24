param(
    [string]$TexturePath = "res\freedoom\stonew1.png",
    [string]$WallBrownTexturePath = "res\freedoom\brown1.png",
    [string]$WallGrayTexturePath = "res\freedoom\graywide.png",
    [string]$WallMetalTexturePath = "res\freedoom\shawn02.png",
    [string]$WallBrickTexturePath = "res\freedoom\brbrick.png",
    [string]$WallTechTexturePath = "res\freedoom\bigwall.png",
    [string]$DoorTexturePath = "res\freedoom\aqdoor01.png",
    [string]$LockedDoorTexturePath = "res\freedoom\aqdoor02.png",
    [string]$SwitchTexturePath = "res\freedoom\comp01_1.png",
    [string]$HudPath = "res\freedoom\stbar.png",
    [string]$WeaponIdlePath = "res\freedoom\pisga0.png",
    [string]$WeaponFirePath = "res\freedoom\pisgb0.png",
    [string]$BillboardPath = "res\freedoom\bon1a0.png",
    [string]$BillboardKeyPath = "res\freedoom\bkeya0.png",
    [string]$BillboardDecorPath = "res\freedoom\bar1a0.png",
    [string]$BillboardEnemyPath = "res\freedoom\possa1.png",
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
$BillboardSourcePath = Join-Path $Root $BillboardPath
$BillboardKeySourcePath = Join-Path $Root $BillboardKeyPath
$BillboardDecorSourcePath = Join-Path $Root $BillboardDecorPath
$BillboardEnemySourcePath = Join-Path $Root $BillboardEnemyPath
$OutPath = Join-Path $Root "src\generated_assets.h"
$BillboardOutPath = Join-Path $Root "src\generated_billboard_assets.h"
$HudOutPath = Join-Path $Root "src\generated_hud_assets.h"
$WeaponOverlayW = 40
$WeaponOverlayH = 120
$WeaponDrawW = 18
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
    @(0xE0, 0xE0, 0xE0),
    @(0x20, 0x20, 0x20),
    @(0x40, 0x40, 0x40),
    @(0x68, 0x68, 0x68),
    @(0x90, 0x90, 0x90),
    @(0xB8, 0xB8, 0xB8),
    @(0xE0, 0xE0, 0xE0),
    @(0x20, 0x30, 0x50),
    @(0x30, 0x68, 0xA0),
    @(0x50, 0xA0, 0xD0),
    @(0xE0, 0xE0, 0x70),
    @(0x90, 0x40, 0x40),
    @(0xC0, 0x68, 0x48),
    @(0x30, 0x50, 0x20),
    @(0x50, 0x70, 0x30)
)

function Get-NearestPaletteIndex([System.Drawing.Color]$Color) {
    $bestIndex = 0
    $bestDistance = [int]::MaxValue

    for ($i = 0; $i -lt $palette.Count; $i++) {
        $dr = [int]$Color.R - $palette[$i][0]
        $dg = [int]$Color.G - $palette[$i][1]
        $db = [int]$Color.B - $palette[$i][2]
        $distance = ($dr * $dr) + ($dg * $dg) + ($db * $db)

        if ($distance -lt $bestDistance) {
            $bestDistance = $distance
            $bestIndex = $i
        }
    }

    return $bestIndex
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
    if (($Color.A -lt 128) -or (($Color.R -lt 12) -and ($Color.G -lt 12) -and ($Color.B -lt 12))) {
        return 0
    }

    if ($FireFrame -and ($Color.R -gt 180) -and ($Color.G -gt 120)) {
        return 11
    }

    if (($Color.R -gt $Color.G + 24) -and ($Color.R -gt $Color.B + 32)) {
        if ($Color.R -gt 160) { return 13 }
        return 12
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
$billboardRows = Convert-Image $BillboardSourcePath 16 16 $true
$billboardKeyRows = Convert-Image $BillboardKeySourcePath 16 16 $true
$billboardDecorRows = Convert-Image $BillboardDecorSourcePath 16 16 $true
$billboardEnemyRows = Convert-Image $BillboardEnemySourcePath 16 16 $true
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
// Enemy billboard source: $relativeBillboardEnemySource
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

static const u8 FREEDOOM_BILLBOARD_ENEMY_TEXTURE[16][16] = {
$($billboardEnemyRows -join ",`r`n")
};

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

// Generated by tools/convert-freedoom-assets.ps1.
// HUD source: $relativeHudSource
// Weapon idle source: $relativeWeaponIdleSource
// Weapon fire source: $relativeWeaponFireSource
// Generated at: $generatedAt
static const u32 FREEDOOM_HUD_TILES[FREEDOOM_HUD_TILE_COUNT][8] = {
$($hudTiles -join ",`r`n")
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
