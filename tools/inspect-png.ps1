# Inspect a PNG: print dimensions and an ASCII luminance map.
# Handy for locating features (labels, slots, sprites) before wiring pixel/tile
# coordinates into the renderer, without eyeballing the raw image.
#
#   .\tools\inspect-png.ps1 res\originaldoom\graphics\STBAR.png
#   .\tools\inspect-png.ps1 res\originaldoom\graphics\STBAR.png -Cols 32 -Rows 12
#   .\tools\inspect-png.ps1 face.png -Zoom 3 -SaveZoom out\face_zoom.png
#
# -Cols/-Rows control the ASCII grid resolution (each cell averages the source
# region it covers, so -Cols 32 gives one column per Mega Drive tile on a
# 256-wide-equivalent bar). -SaveZoom writes a nearest-neighbour upscale for a
# clearer look in an image viewer.
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Path,
    [int]$Cols = 80,
    [int]$Rows = 24,
    [int]$Zoom = 0,
    [string]$SaveZoom = ""
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Path)) {
    throw "Image not found: $Path"
}

Add-Type -AssemblyName System.Drawing
$full = (Resolve-Path $Path).Path
$img = [System.Drawing.Bitmap]::new($full)

try {
    "Image: $full"
    "Size : $($img.Width) x $($img.Height)"
    ""

    $chars = ' .:-=+*#%@'
    for ($ry = 0; $ry -lt $Rows; $ry++) {
        $line = ''
        for ($cx = 0; $cx -lt $Cols; $cx++) {
            # Average the block of source pixels this cell covers so thin features
            # are not missed by point sampling.
            $x0 = [int]($cx * $img.Width / $Cols)
            $x1 = [Math]::Max($x0 + 1, [int](($cx + 1) * $img.Width / $Cols))
            $y0 = [int]($ry * $img.Height / $Rows)
            $y1 = [Math]::Max($y0 + 1, [int](($ry + 1) * $img.Height / $Rows))
            $sum = 0
            $count = 0
            for ($sy = $y0; $sy -lt $y1 -and $sy -lt $img.Height; $sy++) {
                for ($sx = $x0; $sx -lt $x1 -and $sx -lt $img.Width; $sx++) {
                    $p = $img.GetPixel($sx, $sy)
                    $sum += [int](($p.R * 30 + $p.G * 59 + $p.B * 11) / 100)
                    $count++
                }
            }
            $lum = if ($count -gt 0) { [int]($sum / $count) } else { 0 }
            $idx = [int]($lum * ($chars.Length - 1) / 255)
            $line += $chars[$idx]
        }
        "{0,3} {1}" -f $ry, $line
    }

    if (($Zoom -gt 0) -and ($SaveZoom -ne "")) {
        $w = $img.Width * $Zoom
        $h = $img.Height * $Zoom
        $bmp = [System.Drawing.Bitmap]::new($w, $h)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
        $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
        $g.DrawImage($img, 0, 0, $w, $h)
        $g.Dispose()
        $bmp.Save((Join-Path (Get-Location) $SaveZoom))
        $bmp.Dispose()
        ""
        "Saved ${Zoom}x zoom to $SaveZoom"
    }
} finally {
    $img.Dispose()
}
