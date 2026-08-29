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
    # The pistol keeps dedicated parameters because it is baked by the original
    # (legacy) stretch-to-box path -- see $WeaponSet below.
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
# Generated headers live beside the module that consumes them (src/<group>/),
# not at the src/ root -- keep these in sync with the real tree or a
# regeneration silently drops stale copies next to the hand-written headers.
$OutPath = Join-Path $Root "src\bsp\generated_assets.h"
$BillboardOutPath = Join-Path $Root "src\billboard\generated_billboard_assets.h"
$BillboardGeometryOutPath = Join-Path $Root "src\billboard\generated_billboard_geometry.h"
$HudOutPath = Join-Path $Root "src\renderer\generated_hud_assets.h"
$MapOutPath = Join-Path $Root "src\bsp\generated_e1m1_map.c"
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

# The overlay is drawn as one fixed 8x5 tile rectangle on BG_A, so EVERY weapon
# must bake into the same tile-aligned box or its tiles would be clipped. That
# rectangle is view tiles (6,10)..(13,14) -> pixels x 48..111, y 80..119. The
# pistol's 50x38 box above sits inside it; the wider weapons use the full width.
# Keep these tile-aligned: generate-renderer-assets.ps1 derives MEGALDOOM_WEAPON_
# TILE_X/Y/W/H from them with a >>3, and the runtime VRAM window is sized to the
# resulting tile count.
$WeaponRectX = 48
$WeaponRectY = 80
$WeaponRectW = 64
$WeaponRectH = 40

# The five shareware weapons, in WeaponId order (src/weapons.h).
#
# Legacy = $true selects the original pistol bake: the sprite is STRETCHED into
# the fixed 50x38 box, ignoring its aspect ratio. That look is what shipped and
# what the project has iterated on, so it is reproduced bit-for-bit rather than
# unified with the others. Legacy = $false uses Convert-WeaponFrame, which keeps
# each sprite's native aspect ratio, scales idle and fire by one shared factor,
# and anchors both frames on the same Doom psprite origin so the muzzle flash
# lands on the barrel and the gun does not jump between frames.
#
# Flash = "" means the weapon has no muzzle-flash sprite (fist, chainsaw); its
# fire frame is just the attack pose.
$WeaponSet = @(
    # PAL2 is the established skin/metal ramp for Doom-guy's portrait. The
    # fist is entirely hand/forearm, so using it here fixes the PAL3 gold cast
    # without changing the shared world palette or any other weapon.
    [pscustomobject]@{ Name = "FIST";     Idle = "PUNGA0"; Fire = "PUNGC0"; Flash = "";       Legacy = $false; Palette = "Face" }
    [pscustomobject]@{ Name = "CHAINSAW"; Idle = "SAWGC0"; Fire = "SAWGA0"; Flash = "";       Legacy = $false; Palette = "World" }
    [pscustomobject]@{ Name = "PISTOL";   Idle = "PISGA0"; Fire = "PISGB0"; Flash = "PISFA0"; Legacy = $true;  Palette = "World" }
    [pscustomobject]@{ Name = "SHOTGUN";  Idle = "SHTGA0"; Fire = "SHTGB0"; Flash = "SHTFA0"; Legacy = $false; Palette = "World" }
    [pscustomobject]@{ Name = "CHAINGUN"; Idle = "CHGGA0"; Fire = "CHGGB0"; Flash = "CHGFA0"; Legacy = $false; Palette = "World" }
)

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
    @{ Name = "ARMOR_BONUS"; Path = "res\originaldoom\sprites\BON2A0.png" },
    # PAL3 deliberately has no saturated green: its former green slot is now
    # the khaki wall band used heavily by BROWNGRN. Keep ARM1 recognisably
    # olive by selecting a no-amber subset rather than recolouring the world.
    @{ Name = "GREEN_ARMOR"; Path = "res\originaldoom\sprites\ARM1A0.png"; Palette = "OliveArmor" },
    @{ Name = "BLUE_ARMOR"; Path = "res\originaldoom\sprites\ARM2A0.png" },
    @{ Name = "CLIP"; Path = "res\originaldoom\sprites\CLIPA0.png" },
    @{ Name = "AMMO_BOX"; Path = "res\originaldoom\sprites\AMMOA0.png" },
    # Shell ammo and the three collectable weapons. Deliberately NOT added to
    # WORLD_SPRITE_INPUTS in tools/wad-map-extract.py: that list is the PAL3
    # histogram, and re-weighting it would recolour every wall, item and the
    # weapon overlay. These quantize against the frozen palette instead.
    @{ Name = "SHELLS"; Path = "res\originaldoom\sprites\SHELA0.png" },
    @{ Name = "SHELL_BOX"; Path = "res\originaldoom\sprites\SBOXA0.png" },
    @{ Name = "SHOTGUN_PICKUP"; Path = "res\originaldoom\sprites\SHOTA0.png" },
    @{ Name = "CHAINGUN_PICKUP"; Path = "res\originaldoom\sprites\MGUNA0.png" },
    @{ Name = "CHAINSAW_PICKUP"; Path = "res\originaldoom\sprites\CSAWA0.png" },
    @{ Name = "CANDLE"; Path = "res\originaldoom\sprites\CANDA0.png" },
    @{ Name = "CANDELABRA"; Path = "res\originaldoom\sprites\CBRAA0.png" },
    @{ Name = "COLUMN"; Path = "res\originaldoom\sprites\COLUA0.png" },
    @{ Name = "ELEC"; Path = "res\originaldoom\sprites\ELECA0.png" },
    @{ Name = "BARREL"; Path = "res\originaldoom\sprites\BAR1A0.png" },
    @{ Name = "TREE"; Path = "res\originaldoom\sprites\TREDA0.png" }
)
foreach ($spec in $BillboardWorldSpecs) {
    if (-not (Test-Path (Join-Path $Root $spec.Path))) {
        throw "Billboard world sprite not found: $($spec.Path)"
    }
}

Add-Type -AssemblyName System.Drawing

# The WAD generator owns the wall catalog, PAL3 palette and sector colors.
# Generate it first so weapon and billboard conversion can target the same
# 16-color line used by the dynamic view tiles.
#
# --maps, not --map: the shipped atlas is the UNION over the campaign (53
# textures), and generated_e1m2_map.c indexes into it. Regenerating with a
# single map silently rebuilt a 24-texture atlas while E1M2's descriptor still
# referenced ids up to 52, so `npm run assets` on its own left the tree
# unbuildable and the only way back was knowing to run wad-map-extract.py by
# hand afterwards. The campaign list belongs in one place, and this is it.
& python (Join-Path $PSScriptRoot "wad-map-extract.py") `
    --wad (Join-Path $Root "DOOM1.WAD") `
    --maps E1M1 E1M2 `
    --map-out-dir (Split-Path -Parent $MapOutPath) `
    --assets-out $OutPath
if ($LASTEXITCODE -ne 0) {
    throw "Campaign map/world asset generation failed."
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
$worldPaletteHex = ($worldPalette | ForEach-Object {
    "{0:X2}{1:X2}{2:X2}" -f $_[0], $_[1], $_[2]
}) -join ","
$worldMixLut = (& python (Join-Path $PSScriptRoot "world-palette-lut.py") `
    --palette $worldPaletteHex) | ConvertFrom-Json
if ($LASTEXITCODE -ne 0 -or $worldMixLut.first.Count -ne 32768) {
    throw "Could not build perceptual PAL3 lookup."
}
$worldBayer = @(
    @(0, 8, 2, 10), @(12, 4, 14, 6), @(3, 11, 1, 9), @(15, 7, 13, 5)
)

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

# Weapon sprites share PAL3 with the world, but the world palette's bright
# red/amber endpoints are reserved for damage, warning and a few map materials.
# Feeding Doom's skin tones through the unrestricted perceptual mix therefore
# makes the hand dither against those vivid endpoints (especially the bright
# knuckles). Keep the global palette ABI untouched and constrain only warm
# weapon pixels to the existing dark-brown -> muted-flesh ramp.
$weaponWarmPaletteIndices = @(4, 6, 8, 12)

# ARM1 must keep its identity as the green armor without taking back PAL3's
# former green slot. Index 9 (484824) is the only olive PAL3 has, so nearest-RGB
# is useless here: it either collapses the whole vest into that one slot (flat
# blob) or reaches for the amber endpoint. Shade it as an explicit luminance
# ramp instead -- dark outline, a wide khaki body band, then neutral greys for
# the highlights. Excludes PAL3's vivid red/amber endpoints, never Bayer-dithered.
$armorOlivePaletteIndices = @(3, 7, 9, 10, 13)

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

# Native STTNUM/STTPRCNT red ramp for the transparent BG_A number overlay.
# Index 0 stays transparent; opaque source pixels are quantized to 1..15.
$hudDigitPalette = @(
    @(0x00, 0x00, 0x00),
    @(0x2F, 0x2F, 0x2F),
    @(0x43, 0x00, 0x00),
    @(0x53, 0x07, 0x07),
    @(0x5B, 0x00, 0x00),
    @(0x67, 0x00, 0x00),
    @(0x73, 0x00, 0x00),
    @(0x7F, 0x00, 0x00),
    @(0x8B, 0x00, 0x00),
    @(0x9B, 0x00, 0x00),
    @(0xA7, 0x00, 0x00),
    @(0xB3, 0x00, 0x00),
    @(0xBF, 0x00, 0x00),
    @(0xCB, 0x00, 0x00),
    @(0xD7, 0x00, 0x00),
    @(0xEF, 0x00, 0x00)
)

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

function Get-NearestOpaqueIndexInPalette([System.Drawing.Color]$Color, $Palette) {
    $bestIndex = 1
    $bestDistance = [int]::MaxValue
    for ($i = 1; $i -lt $Palette.Count; $i++) {
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

function Get-NearestWorldPaletteIndex([System.Drawing.Color]$Color,
                                      [bool]$AllowTransparent = $true,
                                      [int]$X = -1, [int]$Y = -1) {
    if ($AllowTransparent -and $Color.A -lt 128) { return 0 }
    $r5 = [int][Math]::Floor(([int]$Color.R * 31) / 255)
    $g5 = [int][Math]::Floor(([int]$Color.G * 31) / 255)
    $b5 = [int][Math]::Floor(([int]$Color.B * 31) / 255)
    $key = ($r5 * 1024) + ($g5 * 32) + $b5
    $first = [int]$worldMixLut.first[$key]
    $second = [int]$worldMixLut.second[$key]
    $coverage = [int]$worldMixLut.coverage[$key]
    if (($X -ge 0) -and ($Y -ge 0)) {
        return $(if ($worldBayer[$Y -band 3][$X -band 3] -lt $coverage) {
            $second
        } else {
            $first
        })
    }
    return $(if ($coverage -ge 8) { $second } else { $first })
}

function Get-NearestWeaponWarmIndex([System.Drawing.Color]$Color) {
    $bestIndex = $weaponWarmPaletteIndices[0]
    $bestDistance = [int]::MaxValue
    foreach ($index in $weaponWarmPaletteIndices) {
        $dr = [int]$Color.R - $palette[$index][0]
        $dg = [int]$Color.G - $palette[$index][1]
        $db = [int]$Color.B - $palette[$index][2]
        $distance = ($dr * $dr) + ($dg * $dg) + ($db * $db)
        if ($distance -lt $bestDistance) {
            $bestDistance = $distance
            $bestIndex = $index
        }
    }
    return $bestIndex
}

function Get-FistSkinIndex([System.Drawing.Color]$Color) {
    # PUNGA0/PUNGC0 are very bright at source (FFBB93, FFD3BB, FFE3D3), so a
    # nearest-RGB match against the whole face palette lands on its off-white
    # and grey slots and the hand bakes out chalky and blotchy. Walk the face
    # palette's flesh ramp by luminance instead: indices 2..8 only, which
    # excludes the chalk (13/14/15) and the portrait's reds (9/10) by
    # construction. The top band stops at F0C898, so the hand also stops being
    # blown out. Thresholds are cut against the source deciles -- PUNGA0 spans
    # 24..239 and spreads over all seven steps; PUNGC0 is the shadowed punch
    # pose and correctly sits lower in the ramp.
    $luminance = [int](($Color.R * 30 + $Color.G * 59 + $Color.B * 11) / 100)
    if ($luminance -lt 55) { return 2 }
    if ($luminance -lt 85) { return 3 }
    if ($luminance -lt 115) { return 4 }
    if ($luminance -lt 150) { return 5 }
    if ($luminance -lt 185) { return 6 }
    if ($luminance -lt 215) { return 7 }
    return 8
}

function Get-NearestArmorOliveIndex([System.Drawing.Color]$Color) {
    # ARM1's opaque pixels span luminance 17..185 (deciles 38/51/51/63/89/89/
    # 123/135/160). The bands below are cut against that distribution so index 9
    # keeps roughly the middle 60% -- enough for the sprite to still read green --
    # while the top three bands restore the shading a single-slot bake destroys.
    # Every pixel goes through the ramp, including the handful of near-black
    # greens that fail a chroma test but still belong in the shadow band.
    $luminance = [int](($Color.R * 30 + $Color.G * 59 + $Color.B * 11) / 100)
    if ($luminance -lt 40) { return 3 }
    if ($luminance -lt 115) { return 9 }
    if ($luminance -lt 150) { return 7 }
    if ($luminance -lt 175) { return 10 }
    return 13
}

function Get-PreservedAspectPlacement([int]$SourceWidth, [int]$SourceHeight,
                                      [int]$Width, [int]$Height,
                                      [bool]$BottomAlign = $false) {
    $scale = [Math]::Min($Width / $SourceWidth, $Height / $SourceHeight)
    $drawWidth = [Math]::Max(1, [int][Math]::Round($SourceWidth * $scale))
    $drawHeight = [Math]::Max(1, [int][Math]::Round($SourceHeight * $scale))
    $drawX = [int](($Width - $drawWidth) / 2)
    $drawY = if ($BottomAlign) { $Height - $drawHeight } else { [int](($Height - $drawHeight) / 2) }
    return [pscustomobject]@{
        X = $drawX; Y = $drawY; Width = $drawWidth; Height = $drawHeight
    }
}

function Convert-Image([string]$Path, [int]$Width, [int]$Height, [bool]$UseAlphaTransparency,
                       [bool]$PreserveAspect = $false, [bool]$BottomAlign = $false,
                       [bool]$NativeSize = $false, [string]$PalettePolicy = "World") {
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

    if ($NativeSize) {
        if ($image.Width -gt $Width -or $image.Height -gt $Height) {
            throw "Native sprite $Path ($($image.Width)x$($image.Height)) exceeds ${Width}x${Height} canvas"
        }
        $drawWidth = $image.Width
        $drawHeight = $image.Height
    } elseif ($PreserveAspect) {
        $placement = Get-PreservedAspectPlacement $image.Width $image.Height $Width $Height $BottomAlign
        $drawWidth = $placement.Width
        $drawHeight = $placement.Height
        $drawX = $placement.X
        $drawY = $placement.Y
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
                    $index = Get-NearestWorldPaletteIndex $avg $true $x $y
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
                        if ($PalettePolicy -eq "OliveArmor") {
                            $index = Get-NearestArmorOliveIndex $avg
                        } else {
                            $index = Get-NearestWorldPaletteIndex $avg $false $x $y
                        }
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

function Get-WeaponPaletteIndex([System.Drawing.Color]$Color, [bool]$FireFrame,
                                [int]$X = -1, [int]$Y = -1,
                                [string]$PalettePolicy = "World") {
    # Transparency is decided ONLY by alpha. Dark opaque pixels (the pistol's
    # metal/shadow) must stay solid, otherwise the gun renders see-through.
    if ($Color.A -lt 128) {
        return 0
    }

    # The fist is rendered with PAL2, whose existing portrait ramp carries the
    # skin and shadow tones needed by the bare hand. It has no muzzle flash, so
    # no PAL3 colour is ever interpreted through PAL2.
    if ($PalettePolicy -eq "Face") {
        return Get-FistSkinIndex $Color
    }

    # Muzzle flashes keep the unrestricted palette so their red/yellow/white
    # burst remains vivid. The gun and hand use only the quieter warm ramp;
    # otherwise PAL3's red/amber endpoints turn the skin into orange noise.
    if ($FireFrame) {
        return Get-NearestWorldPaletteIndex $Color $false $X $Y
    }
    $isWarm = ($Color.R -ge ($Color.G + 18)) -and
              ($Color.G -ge $Color.B) -and
              (($Color.R - $Color.B) -ge 24)
    if ($isWarm) {
        return Get-NearestWeaponWarmIndex $Color
    }
    return Get-NearestWorldPaletteIndex $Color $false $X $Y
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
                    $index = Get-WeaponPaletteIndex $image.GetPixel($srcX, $srcY) $FireFrame $x $y
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
                            $index = Get-WeaponPaletteIndex $gp $false $x $y
                        }
                    }

                    # Muzzle flash on top -> warm ramp (fire red/yellow/white).
                    $fx = $compX - $flPlaceX
                    $fy = $compY - $flPlaceY
                    if (($fx -ge 0) -and ($fx -lt $flW) -and ($fy -ge 0) -and ($fy -lt $flH)) {
                        $fp = $flashImage.GetPixel($fx, $fy)
                        if ($fp.A -ge 128) {
                            $index = Get-WeaponPaletteIndex $fp $true $x $y
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

# --- General (non-legacy) weapon bake -------------------------------------
#
# Doom positions every psprite by its WAD leftOffset/topOffset against one
# shared anchor, so anchor space is the natural common frame for a weapon's idle
# pose, attack pose and muzzle flash. Anchor coordinate of sprite pixel (px,py)
# is (px - leftOffset, py - topOffset); the offsets are negative, so this pushes
# the sprite down and right of the origin exactly as the game does.
#
# Get-WeaponPlacement frames ONE pose (the gun plus, if present, its muzzle
# flash, unioned in anchor space) into the fixed overlay rectangle:
#
#   * uniform scale, chosen so the pose spans the rectangle's full 64px width --
#     the width is the honest constraint, since the rectangle is only 40px tall
#     while Doom psprites are 60-120px and mostly hand and forearm;
#   * horizontally centred;
#   * bottom-pinned, but never allowed to push its own top above the rectangle.
#     A short pose (the fist) therefore sits on the bottom edge like Doom's, and
#     a tall one (the shotgun, the chaingun) keeps its barrel and loses the arm
#     off the bottom of the viewport -- exactly what Doom's 320x200 screen does
#     to the same sprites.
#
# Each pose is framed independently rather than sharing the idle pose's scale.
# Sharing sounds more correct, but Doom's attack sprites are up to twice the
# idle's size (SHTGB0 is 119x121 against SHTGA0's 79x60), so a shared scale
# either shrinks the idle to nothing or clips the attack in half. The shipped
# pistol bake already frames its two poses independently, so this also keeps the
# five weapons consistent with each other.
function Get-WeaponPlacement([string[]]$Names) {
    $ax0 = [double]::MaxValue; $ay0 = [double]::MaxValue
    $ax1 = [double]::MinValue; $ay1 = [double]::MinValue
    foreach ($name in $Names) {
        if ([string]::IsNullOrEmpty($name)) { continue }
        $offset = Get-SpriteOffset $name
        if (-not $offset) { throw "No sprite offsets recorded for $name" }
        $ax0 = [Math]::Min($ax0, -$offset.leftOffset)
        $ay0 = [Math]::Min($ay0, -$offset.topOffset)
        $ax1 = [Math]::Max($ax1, -$offset.leftOffset + $offset.width)
        $ay1 = [Math]::Max($ay1, -$offset.topOffset + $offset.height)
    }
    $scale = $WeaponRectW / ($ax1 - $ax0)
    $height = ($ay1 - $ay0) * $scale
    $top = [Math]::Max($WeaponRectY, $WeaponRectY + $WeaponRectH - $height)
    return [pscustomobject]@{
        Scale = $scale
        OriginX = $WeaponRectX - ($ax0 * $scale)
        OriginY = $top - ($ay0 * $scale)
    }
}

# Bake one frame: the gun sprite, optionally with a muzzle flash composited over
# it. Returns C initialiser rows for the whole 160x120 overlay canvas
# (0 = transparent) so the tile baker can slice the overlay rectangle out of it.
function Convert-WeaponFrame([string]$GunName, [string]$FlashName,
                             [string]$PalettePolicy = "World") {
    $placement = Get-WeaponPlacement @($GunName, $FlashName)
    $layers = New-Object System.Collections.Generic.List[object]
    foreach ($name in @($GunName, $FlashName)) {
        if ([string]::IsNullOrEmpty($name)) { continue }
        $offset = Get-SpriteOffset $name
        $path = Join-Path $Root "res\originaldoom\sprites\$name.png"
        if (-not (Test-Path $path)) { throw "Weapon sprite not found: $path" }
        [void]$layers.Add([pscustomobject]@{
            Image = [System.Drawing.Bitmap]::new($path)
            AX = -$offset.leftOffset
            AY = -$offset.topOffset
            IsFlash = ($name -eq $FlashName -and -not [string]::IsNullOrEmpty($FlashName))
        })
    }

    $rows = New-Object System.Collections.Generic.List[string]
    try {
        $invScale = 1.0 / $placement.Scale
        for ($y = 0; $y -lt $WeaponOverlayH; $y++) {
            $values = New-Object System.Collections.Generic.List[string]
            for ($x = 0; $x -lt $WeaponOverlayW; $x++) {
                $index = 0
                if (($x -ge $WeaponRectX) -and ($x -lt ($WeaponRectX + $WeaponRectW)) -and
                    ($y -ge $WeaponRectY) -and ($y -lt ($WeaponRectY + $WeaponRectH))) {
                    # Screen pixel -> anchor space -> per-layer sprite pixel.
                    $ax = ($x - $placement.OriginX) * $invScale
                    $ay = ($y - $placement.OriginY) * $invScale
                    # Later layers (the flash) paint over earlier ones.
                    foreach ($layer in $layers) {
                        $sx = [int][Math]::Floor($ax - $layer.AX)
                        $sy = [int][Math]::Floor($ay - $layer.AY)
                        if (($sx -ge 0) -and ($sx -lt $layer.Image.Width) -and
                            ($sy -ge 0) -and ($sy -lt $layer.Image.Height)) {
                            $pixel = $layer.Image.GetPixel($sx, $sy)
                            if ($pixel.A -ge 128) {
                                if ($layer.IsFlash) {
                                    $index = Get-NearestWorldPaletteIndex $pixel $false $x $y
                                } else {
                                    $index = Get-WeaponPaletteIndex $pixel $false $x $y $PalettePolicy
                                }
                            }
                        }
                    }
                }
                $values.Add($index.ToString())
            }
            $rows.Add("    {" + ($values -join ", ") + "}")
        }
    } finally {
        foreach ($layer in $layers) { $layer.Image.Dispose() }
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
    $Width = 320
    $Height = 32

    try {
        if (($image.Width -ne $Width) -or ($image.Height -ne $Height)) {
            throw "Native HUD source must be exactly ${Width}x${Height}: $Path is $($image.Width)x$($image.Height)"
        }
        for ($tileY = 0; $tileY -lt ($Height / 8); $tileY++) {
            for ($tileX = 0; $tileX -lt ($Width / 8); $tileX++) {
                $rows = New-Object System.Collections.Generic.List[string]

                for ($row = 0; $row -lt 8; $row++) {
                    $packed = [uint32]0

                    for ($col = 0; $col -lt 8; $col++) {
                        $x = ($tileX * 8) + $col
                        $y = ($tileY * 8) + $row
                        $index = Get-HudPaletteIndex $image.GetPixel($x, $y)

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

# Doom-guy face (STF*). Every 24px portrait is centred in the native 32px-wide
# status-bar recess. The four pixels on each side are baked into the frame, so
# runtime placement is tile-aligned while visible content is pixel-centred.
$FaceTileW = 4
$FaceTileH = 4
$FaceSourceW = 24
$FaceContentPadX = 4
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
    # Centre the 24px Doom portrait inside the 32x32 HUD recess. A 3-tile
    # block can only sit four pixels to either side of the recess centre.
    $offsetX = [Math]::Max(0, [int](($blockW - $image.Width) / 2))
    $offsetY = [Math]::Max(0, [int](($blockH - $image.Height) / 2))

    try {
        if ($image.Width -ne $FaceSourceW) {
            throw "Face frame must be exactly $FaceSourceW pixels wide: $Path is $($image.Width)"
        }
        if ($offsetX -ne $FaceContentPadX -or
            ((2 * $offsetX) + $image.Width) -ne $blockW) {
            throw "Face frame is not pixel-centred in its $blockW-pixel cell: $Path"
        }
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
    $uniqueTiles = New-Object System.Collections.Generic.List[string]
    $frameMaps = New-Object System.Collections.Generic.List[string]
    $tileLookup = @{}
    foreach ($name in $FaceFrameNames) {
        $framePath = Join-Path $Root (Join-Path $FaceGraphicsDir "$name.png")
        if (-not (Test-Path $framePath)) {
            throw "Face frame source not found: $framePath"
        }
        $indices = New-Object System.Collections.Generic.List[string]
        foreach ($tile in (Convert-FaceFrame $framePath)) {
            if (-not $tileLookup.ContainsKey($tile)) {
                $tileLookup[$tile] = $uniqueTiles.Count
                $uniqueTiles.Add($tile)
            }
            $indices.Add($tileLookup[$tile].ToString())
        }
        $frameMaps.Add("    {" + ($indices -join ", ") + "}")
    }
    return [pscustomobject]@{ Tiles = $uniqueTiles; FrameMaps = $frameMaps }
}

# Native Doom status-bar numbers. Runtime composition uses a transparent 16x16
# canvas per glyph so variable source widths remain available for right-aligned
# placement while all sampling stays within fixed array bounds.
$HudDigitNames = @("STTNUM0", "STTNUM1", "STTNUM2", "STTNUM3", "STTNUM4",
                   "STTNUM5", "STTNUM6", "STTNUM7", "STTNUM8", "STTNUM9",
                   "STTPRCNT")
$HudDigitCanvasW = 16
$HudDigitCanvasH = 16
$hudDigitBlocks = New-Object System.Collections.Generic.List[string]
$hudDigitWidths = New-Object System.Collections.Generic.List[string]
foreach ($name in $HudDigitNames) {
    $path = Join-Path $Root (Join-Path $FaceGraphicsDir "$name.png")
    if (-not (Test-Path $path)) {
        throw "HUD digit source not found: $path"
    }
    $image = [System.Drawing.Bitmap]::new($path)
    try {
        if ($image.Width -gt $HudDigitCanvasW -or $image.Height -gt $HudDigitCanvasH) {
            throw "HUD digit $name exceeds ${HudDigitCanvasW}x${HudDigitCanvasH}"
        }
        $rows = New-Object System.Collections.Generic.List[string]
        for ($y = 0; $y -lt $HudDigitCanvasH; $y++) {
            $values = New-Object System.Collections.Generic.List[string]
            for ($x = 0; $x -lt $HudDigitCanvasW; $x++) {
                $index = 0
                if ($x -lt $image.Width -and $y -lt $image.Height) {
                    $pixel = $image.GetPixel($x, $y)
                    if ($pixel.A -ge 128) {
                        $index = Get-NearestOpaqueIndexInPalette $pixel $hudDigitPalette
                    }
                }
                $values.Add($index.ToString())
            }
            $rows.Add("        {" + ($values -join ", ") + "}")
        }
        $hudDigitBlocks.Add("    {`r`n" + ($rows -join ",`r`n") + "`r`n    }")
        $hudDigitWidths.Add($image.Width.ToString())
    } finally {
        $image.Dispose()
    }
}

$weaponIdleBlocks = New-Object System.Collections.Generic.List[string]
$weaponFireBlocks = New-Object System.Collections.Generic.List[string]
$weaponSourceComments = New-Object System.Collections.Generic.List[string]
foreach ($weapon in $WeaponSet) {
    if ($weapon.Legacy) {
        $idleRows = Convert-WeaponOverlay $WeaponIdleSourcePath $false
        $fireRows = Convert-WeaponOverlayFire $WeaponFireSourcePath $WeaponFlashSourcePath
    } else {
        $idleRows = Convert-WeaponFrame $weapon.Idle "" $weapon.Palette
        $fireRows = Convert-WeaponFrame $weapon.Fire $weapon.Flash $weapon.Palette
    }
    [void]$weaponIdleBlocks.Add("  {" + "`r`n" + ($idleRows -join ",`r`n") + "`r`n  }")
    [void]$weaponFireBlocks.Add("  {" + "`r`n" + ($fireRows -join ",`r`n") + "`r`n  }")
    $flashNote = $(if ([string]::IsNullOrEmpty($weapon.Flash)) { "no flash" } else { $weapon.Flash })
    [void]$weaponSourceComments.Add(
        "//   $($weapon.Name): idle $($weapon.Idle), fire $($weapon.Fire) + $flashNote" +
        $(if ($weapon.Legacy) { " (legacy stretch bake)" } else { "" }))
}
$hudTiles = Convert-HudTiles $HudSourcePath
$faceFrames = Convert-FaceFrames
$faceTiles = $faceFrames.Tiles
$faceFrameMaps = $faceFrames.FrameMaps
$faceFrameCount = $FaceFrameNames.Count
$facePaletteRgb = ($facePalette | ForEach-Object {
    "0x{0:X2}{1:X2}{2:X2}" -f $_[0], $_[1], $_[2]
}) -join ", "
$hudDigitPaletteRgb = ($hudDigitPalette | ForEach-Object {
    "0x{0:X2}{1:X2}{2:X2}" -f $_[0], $_[1], $_[2]
}) -join ", "
$billboardRows = Convert-Image $BillboardSourcePath 16 16 $true
$billboardKeyRows = Convert-Image $BillboardKeySourcePath 16 16 $true
$billboardDecorRows = Convert-Image $BillboardDecorSourcePath 16 16 $true
$billboardWorldW = 24
$billboardWorldH = 48
# How many entries at the FRONT of $BillboardWorldSpecs are collectibles. Only
# those get column-post tables (the sparse-sprite raster path); the props and
# the barrel after them are dense enough to stay on the row packer. Keep this
# equal to the number of collectible specs listed above CANDLE.
$billboardPickupCount = 16
$billboardWorldBlocks = New-Object System.Collections.Generic.List[string]
$billboardGeometryBlocks = New-Object System.Collections.Generic.List[string]
$billboardPickupPostOffsetBlocks = New-Object System.Collections.Generic.List[string]
$billboardPickupPosts = New-Object System.Collections.Generic.List[string]
$billboardPickupUsePosts = New-Object System.Collections.Generic.List[string]
$billboardWorldIndex = 0
foreach ($spec in $BillboardWorldSpecs) {
    # Keep the preserved-aspect patch bottom-aligned inside the compact atlas.
    # Runtime placement uses the separate WAD origin metadata below, so this
    # padding never changes the sprite's world size or floor relationship.
    $spriteName = [IO.Path]::GetFileNameWithoutExtension($spec.Path)
    $offset = Get-SpriteOffset $spriteName
    if (-not $offset) {
        throw "Sprite geometry not found in _offsets.json: $spriteName"
    }
    $placement = Get-PreservedAspectPlacement $offset.width $offset.height `
        $billboardWorldW $billboardWorldH $true
    $rows = Convert-Image (Join-Path $Root $spec.Path) $billboardWorldW $billboardWorldH `
        $true $true $true $false $spec.Palette
    $billboardWorldBlocks.Add("    {" + "`r`n" + ($rows -join ",`r`n") + "`r`n    }")
    $billboardGeometryBlocks.Add(
        "    {$($offset.width), $($offset.height), $($offset.leftOffset), $($offset.topOffset), " +
        "$($placement.X), $($placement.Y), $($placement.Width), $($placement.Height)}"
    )
    if ($billboardWorldIndex -lt $billboardPickupCount) {
        $matrix = @()
        foreach ($row in $rows) {
            $matrix += ,@([regex]::Matches($row, '\d+') | ForEach-Object { [int]$_.Value })
        }
        $offsets = New-Object System.Collections.Generic.List[string]
        $opaqueCount = 0
        for ($x = 0; $x -lt $billboardWorldW; $x++) {
            $offsets.Add($billboardPickupPosts.Count.ToString())
            $y = 0
            while ($y -lt $billboardWorldH) {
                while (($y -lt $billboardWorldH) -and ($matrix[$y][$x] -eq 0)) { $y++ }
                if ($y -ge $billboardWorldH) { break }
                $top = $y
                while (($y -lt $billboardWorldH) -and ($matrix[$y][$x] -ne 0)) {
                    $opaqueCount++
                    $y++
                }
                $billboardPickupPosts.Add("    {$top, $($y - $top)}")
            }
        }
        $offsets.Add($billboardPickupPosts.Count.ToString())
        $billboardPickupPostOffsetBlocks.Add("    {" + ($offsets -join ", ") + "}")
        $cropArea = [Math]::Max(1, $placement.Width * $placement.Height)
        $billboardPickupUsePosts.Add($(if (($opaqueCount * 100) -lt ($cropArea * 80)) { "1" } else { "0" }))
    }
    $billboardWorldIndex++
}
# Enemy (zombieman) animation frames, all scaled into the same 24x48 box.
# Order MUST match the pose indices in src/billboard/billboard_internal.h:
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
# Native Doom impact/explosion frames live in fixed transparent canvases while
# retaining their individual picture-header geometry. This keeps later BEXP
# frames large instead of squeezing every pose into the enemy atlas.
$BarrelExplosionFrameNames = @("BEXPA0", "BEXPB0", "BEXPC0", "BEXPD0", "BEXPE0")
$barrelExplosionW = 60
$barrelExplosionH = 53
$barrelExplosionFrameBlocks = New-Object System.Collections.Generic.List[string]
$barrelExplosionGeometryBlocks = New-Object System.Collections.Generic.List[string]
foreach ($name in $BarrelExplosionFrameNames) {
    $barrelExplosionFramePath = Join-Path $Root (Join-Path $EnemySpritesDir "$name.png")
    if (-not (Test-Path $barrelExplosionFramePath)) {
        throw "Barrel explosion frame source not found: $barrelExplosionFramePath"
    }
    $offset = Get-SpriteOffset $name
    $barrelExplosionFrameRows = Convert-Image $barrelExplosionFramePath $barrelExplosionW $barrelExplosionH $true $false $false $true
    $barrelExplosionFrameBlocks.Add("    {" + "`r`n" + ($barrelExplosionFrameRows -join ",`r`n") + "`r`n    }")
    $barrelExplosionGeometryBlocks.Add("    {$($offset.width), $($offset.height), $($offset.leftOffset), $($offset.topOffset)}")
}

$barrelExplosionFrameCount = $BarrelExplosionFrameNames.Count

$PuffFrameNames = @("PUFFA0", "PUFFB0", "PUFFC0", "PUFFD0")
$puffW = 15
$puffH = 15
$puffFrameBlocks = New-Object System.Collections.Generic.List[string]
$puffGeometryBlocks = New-Object System.Collections.Generic.List[string]
foreach ($name in $PuffFrameNames) {
    $path = Join-Path $Root (Join-Path $EnemySpritesDir "$name.png")
    $offset = Get-SpriteOffset $name
    $rows = Convert-Image $path $puffW $puffH $true $false $false $true
    $puffFrameBlocks.Add("    {" + "`r`n" + ($rows -join ",`r`n") + "`r`n    }")
    $puffGeometryBlocks.Add("    {$($offset.width), $($offset.height), $($offset.leftOffset), $($offset.topOffset)}")
}

$BloodFrameNames = @("BLUDA0", "BLUDB0", "BLUDC0")
$bloodW = 12
$bloodH = 11
$bloodFrameBlocks = New-Object System.Collections.Generic.List[string]
$bloodGeometryBlocks = New-Object System.Collections.Generic.List[string]
foreach ($name in $BloodFrameNames) {
    $path = Join-Path $Root (Join-Path $EnemySpritesDir "$name.png")
    $offset = Get-SpriteOffset $name
    $rows = Convert-Image $path $bloodW $bloodH $true $false $false $true
    $bloodFrameBlocks.Add("    {" + "`r`n" + ($rows -join ",`r`n") + "`r`n    }")
    $bloodGeometryBlocks.Add("    {$($offset.width), $($offset.height), $($offset.leftOffset), $($offset.topOffset)}")
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

// Doom-style opaque column posts for the 11 collectible world textures.
// Offsets address FREEDOOM_BILLBOARD_PICKUP_POSTS; each post is {top, length}.
#define FREEDOOM_BILLBOARD_PICKUP_TEXTURE_COUNT $billboardPickupCount
#define FREEDOOM_BILLBOARD_PICKUP_POST_COUNT $($billboardPickupPosts.Count)
static const u16 FREEDOOM_BILLBOARD_PICKUP_POST_OFFSETS
    [FREEDOOM_BILLBOARD_PICKUP_TEXTURE_COUNT][FREEDOOM_BILLBOARD_WORLD_W + 1] = {
$($billboardPickupPostOffsetBlocks -join ",`r`n")
};
static const u8 FREEDOOM_BILLBOARD_PICKUP_POSTS
    [FREEDOOM_BILLBOARD_PICKUP_POST_COUNT][2] = {
$($billboardPickupPosts -join ",`r`n")
};
// Dense sprites stay on the row/tile packer; sparse sprites use column posts.
static const u8 FREEDOOM_BILLBOARD_PICKUP_USE_POSTS
    [FREEDOOM_BILLBOARD_PICKUP_TEXTURE_COUNT] = {
    $($billboardPickupUsePosts -join ", ")
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

// Barrel explosion sequence (BEXPA0..E0): 5 frames matched to the engine's
// BILLBOARD_VISUAL_BARREL_EXPLODING visual. Rendered via the same frame-index
// path as the enemy frames in renderer_scene.c::get_billboard_texture().
#define FREEDOOM_BILLBOARD_BARREL_EXPLOSION_FRAME_COUNT $barrelExplosionFrameCount
#define FREEDOOM_BILLBOARD_BARREL_EXPLOSION_W $barrelExplosionW
#define FREEDOOM_BILLBOARD_BARREL_EXPLOSION_H $barrelExplosionH
static const u8 FREEDOOM_BILLBOARD_BARREL_EXPLOSION_FRAMES[FREEDOOM_BILLBOARD_BARREL_EXPLOSION_FRAME_COUNT][$barrelExplosionH][$barrelExplosionW] = {
$($barrelExplosionFrameBlocks -join ",`r`n")
};
static const s16 FREEDOOM_BILLBOARD_BARREL_EXPLOSION_GEOMETRY[FREEDOOM_BILLBOARD_BARREL_EXPLOSION_FRAME_COUNT][4] = {
$($barrelExplosionGeometryBlocks -join ",`r`n")
};

#define FREEDOOM_BILLBOARD_PUFF_FRAME_COUNT $($PuffFrameNames.Count)
#define FREEDOOM_BILLBOARD_PUFF_W $puffW
#define FREEDOOM_BILLBOARD_PUFF_H $puffH
static const u8 FREEDOOM_BILLBOARD_PUFF_FRAMES[FREEDOOM_BILLBOARD_PUFF_FRAME_COUNT][$puffH][$puffW] = {
$($puffFrameBlocks -join ",`r`n")
};
static const s16 FREEDOOM_BILLBOARD_PUFF_GEOMETRY[FREEDOOM_BILLBOARD_PUFF_FRAME_COUNT][4] = {
$($puffGeometryBlocks -join ",`r`n")
};

#define FREEDOOM_BILLBOARD_BLOOD_FRAME_COUNT $($BloodFrameNames.Count)
#define FREEDOOM_BILLBOARD_BLOOD_W $bloodW
#define FREEDOOM_BILLBOARD_BLOOD_H $bloodH
static const u8 FREEDOOM_BILLBOARD_BLOOD_FRAMES[FREEDOOM_BILLBOARD_BLOOD_FRAME_COUNT][$bloodH][$bloodW] = {
$($bloodFrameBlocks -join ",`r`n")
};
static const s16 FREEDOOM_BILLBOARD_BLOOD_GEOMETRY[FREEDOOM_BILLBOARD_BLOOD_FRAME_COUNT][4] = {
$($bloodGeometryBlocks -join ",`r`n")
};

#endif
"@

Set-Content -Path $BillboardOutPath -Value $billboardContent -NoNewline

$billboardGeometryContent = @"
#ifndef MEGALDOOM_GENERATED_BILLBOARD_GEOMETRY_H
#define MEGALDOOM_GENERATED_BILLBOARD_GEOMETRY_H

#include <genesis.h>

// Generated by tools/convert-freedoom-assets.ps1 from the Doom picture headers
// in res/originaldoom/sprites/_offsets.json and the 24x48 atlas placement.
#define FREEDOOM_BILLBOARD_WORLD_GEOMETRY_COUNT $($BillboardWorldSpecs.Count)
#define FREEDOOM_BILLBOARD_GEOMETRY_SOURCE_W 0
#define FREEDOOM_BILLBOARD_GEOMETRY_SOURCE_H 1
#define FREEDOOM_BILLBOARD_GEOMETRY_LEFT_OFFSET 2
#define FREEDOOM_BILLBOARD_GEOMETRY_TOP_OFFSET 3
#define FREEDOOM_BILLBOARD_GEOMETRY_ATLAS_X 4
#define FREEDOOM_BILLBOARD_GEOMETRY_ATLAS_Y 5
#define FREEDOOM_BILLBOARD_GEOMETRY_ATLAS_W 6
#define FREEDOOM_BILLBOARD_GEOMETRY_ATLAS_H 7
#define FREEDOOM_BILLBOARD_GEOMETRY_FIELD_COUNT 8

// $((($BillboardWorldSpecs | ForEach-Object { $_.Name }) -join ", "))
static const s16 FREEDOOM_BILLBOARD_WORLD_GEOMETRY
    [FREEDOOM_BILLBOARD_WORLD_GEOMETRY_COUNT]
    [FREEDOOM_BILLBOARD_GEOMETRY_FIELD_COUNT] = {
$($billboardGeometryBlocks -join ",`r`n")
};

#endif
"@

Set-Content -Path $BillboardGeometryOutPath -Value $billboardGeometryContent -NoNewline

$hudContent = @"
#ifndef MEGALDOOM_GENERATED_HUD_ASSETS_H
#define MEGALDOOM_GENERATED_HUD_ASSETS_H

#include <genesis.h>

#define FREEDOOM_HUD_PIXEL_W 320
#define FREEDOOM_HUD_PIXEL_H 32
#define FREEDOOM_HUD_TILE_W 40
#define FREEDOOM_HUD_TILE_H 4
#define FREEDOOM_HUD_TILE_COUNT 160
#define FREEDOOM_WEAPON_W $WeaponOverlayW
#define FREEDOOM_WEAPON_H $WeaponOverlayH
#define FREEDOOM_WEAPON_COUNT $($WeaponSet.Count)
// The pistol's own stretch box (legacy bake), kept for reference.
#define FREEDOOM_WEAPON_DRAW_X $WeaponDrawX
#define FREEDOOM_WEAPON_DRAW_Y $WeaponDrawY
#define FREEDOOM_WEAPON_DRAW_W $WeaponDrawW
#define FREEDOOM_WEAPON_DRAW_H $WeaponDrawH
// The tile-aligned rectangle EVERY weapon bakes into. This, not DRAW_*, is what
// the tile baker slices, so all weapons share one 8x5 BG_A tilemap rect.
#define FREEDOOM_WEAPON_RECT_X $WeaponRectX
#define FREEDOOM_WEAPON_RECT_Y $WeaponRectY
#define FREEDOOM_WEAPON_RECT_W $WeaponRectW
#define FREEDOOM_WEAPON_RECT_H $WeaponRectH
#define FREEDOOM_FACE_TILE_W $FaceTileW
#define FREEDOOM_FACE_TILE_H $FaceTileH
#define FREEDOOM_FACE_FRAME_TILES $($FaceTileW * $FaceTileH)
#define FREEDOOM_FACE_FRAME_COUNT $faceFrameCount
#define FREEDOOM_FACE_TILE_COUNT $($faceTiles.Count)
#define FREEDOOM_FACE_SOURCE_W $FaceSourceW
#define FREEDOOM_FACE_CONTENT_PAD_X $FaceContentPadX
#define FREEDOOM_HUD_DIGIT_COUNT $($HudDigitNames.Count)
#define FREEDOOM_HUD_DIGIT_PERCENT 10
#define FREEDOOM_HUD_DIGIT_CANVAS_W $HudDigitCanvasW
#define FREEDOOM_HUD_DIGIT_CANVAS_H $HudDigitCanvasH

// Portrait frame indices (order matches the baker's frame list and
// compute_face_frame() in renderer_hud.c). Bracket 0 = high HP, 4 = low HP.
// FACE_FRAME_ST(bracket, dir): dir 0 = look right, 1 = centre, 2 = look left.
#define FACE_FRAME_ST(bracket, dir) (((bracket) * 3) + (dir))
#define FACE_FRAME_OUCH(bracket)    (15 + (bracket))
#define FACE_FRAME_DEAD             20

// Generated by tools/convert-freedoom-assets.ps1.
// HUD source: $relativeHudSource
// Weapon sources: see the per-weapon comment above FREEDOOM_WEAPON_IDLE below.
// Face source: $relativeFaceSource (animated set baked from res/originaldoom/graphics/STF*.png)
// Generated at: $generatedAt
static const u32 FREEDOOM_HUD_TILES[FREEDOOM_HUD_TILE_COUNT][8] = {
$($hudTiles -join ",`r`n")
};

// Dedicated 16-colour palette for the portrait (load into PAL2). Skin/brown/red
// ramp so the face does not turn gold under the shared world palette. Index 1 is
// the recessed-slot fill. Values are 0xRRGGBB; pass through RGB24_TO_VDPCOLOR.
static const u32 FREEDOOM_FACE_PALETTE[16] = { $facePaletteRgb };

// Native STTNUM/STTPRCNT palette and transparent pixel canvases for the BG_A
// number compositor. Widths preserve Doom's variable-width right alignment.
static const u32 FREEDOOM_HUD_DIGIT_PALETTE[16] = { $hudDigitPaletteRgb };
static const u8 FREEDOOM_HUD_DIGIT_WIDTHS[FREEDOOM_HUD_DIGIT_COUNT] = {
    $($hudDigitWidths -join ", ")
};
static const u8 FREEDOOM_HUD_DIGITS
    [FREEDOOM_HUD_DIGIT_COUNT]
    [FREEDOOM_HUD_DIGIT_CANVAS_H]
    [FREEDOOM_HUD_DIGIT_CANVAS_W] = {
$($hudDigitBlocks -join ",`r`n")
};

// Deduplicated Doom-guy portrait tiles. The frame map preserves each 4x4 cell
// while identical background/art tiles share one VRAM slot across expressions.
static const u32 FREEDOOM_FACE_TILES[FREEDOOM_FACE_TILE_COUNT][8] = {
$($faceTiles -join ",`r`n")
};
static const u16 FREEDOOM_FACE_FRAME_TILE_IDS
    [FREEDOOM_FACE_FRAME_COUNT]
    [FREEDOOM_FACE_FRAME_TILES] = {
$($faceFrameMaps -join ",`r`n")
};

// Weapon overlay canvases, in WeaponId order (src/weapons.h):
$($weaponSourceComments -join "`r`n")
// These full-canvas arrays are an intermediate: tools/generate-renderer-assets.ps1
// reads them back and bakes the deduplicated per-weapon VRAM tilesets that the
// runtime actually uses. No .c file references them.
static const u8 FREEDOOM_WEAPON_IDLE[FREEDOOM_WEAPON_COUNT][FREEDOOM_WEAPON_H][FREEDOOM_WEAPON_W] = {
$($weaponIdleBlocks -join ",`r`n")
};

static const u8 FREEDOOM_WEAPON_FIRE[FREEDOOM_WEAPON_COUNT][FREEDOOM_WEAPON_H][FREEDOOM_WEAPON_W] = {
$($weaponFireBlocks -join ",`r`n")
};

#endif
"@

Set-Content -Path $HudOutPath -Value $hudContent -NoNewline

& (Join-Path $PSScriptRoot "generate-renderer-assets.ps1")
if (-not $?) {
    throw "Renderer asset generation failed."
}

if ($BillboardOnly) {
    Write-Host "Generated $BillboardOutPath and $BillboardGeometryOutPath" -ForegroundColor Green
    return
}
& python (Join-Path $PSScriptRoot "generate-frontend-assets.py") --force
if ($LASTEXITCODE -ne 0) {
    throw "Frontend asset generation failed."
}
Write-Host "Generated $OutPath and $MapOutPath from DOOM1.WAD" -ForegroundColor Green
Write-Host "Generated $BillboardOutPath from $BillboardPath, $BillboardKeyPath and $BillboardDecorPath" -ForegroundColor Green
Write-Host "Generated $BillboardGeometryOutPath from Doom sprite offsets" -ForegroundColor Green
