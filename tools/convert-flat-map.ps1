param(
    [string]$WadPath = "DOOM1.WAD",
    [string]$Map = "E1M1",
    [string]$MapOutPath = "",
    [string]$AssetsOutPath = "src\generated_assets.h",
    [string]$PlanOutPath = ""
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")

if (-not [IO.Path]::IsPathRooted($WadPath)) { $WadPath = Join-Path $Root $WadPath }
if (-not $MapOutPath) { $MapOutPath = Join-Path $Root ("src\generated_{0}_map.c" -f $Map.ToLower()) }
elseif (-not [IO.Path]::IsPathRooted($MapOutPath)) { $MapOutPath = Join-Path $Root $MapOutPath }
if (-not [IO.Path]::IsPathRooted($AssetsOutPath)) { $AssetsOutPath = Join-Path $Root $AssetsOutPath }
if (-not $PlanOutPath) { $PlanOutPath = Join-Path $Root ("out\{0}-flat-plan.json" -f $Map.ToLower()) }
elseif (-not [IO.Path]::IsPathRooted($PlanOutPath)) { $PlanOutPath = Join-Path $Root $PlanOutPath }

& python (Join-Path $PSScriptRoot "wad-flat-playable.py") `
    --wad $WadPath --map $Map --out $PlanOutPath
if ($LASTEXITCODE -ne 0) {
    throw "Flat-map progression preflight failed; generated geometry was not replaced."
}

& python (Join-Path $PSScriptRoot "wad-map-extract.py") `
    --wad $WadPath --map $Map --out $MapOutPath --assets-out $AssetsOutPath
if ($LASTEXITCODE -ne 0) {
    throw "Textured BSP extraction failed."
}

Write-Host "Flat textured map conversion succeeded." -ForegroundColor Green
Write-Host "  plan: $PlanOutPath"
Write-Host "  map:  $MapOutPath"
Write-Host "Build without -SectorRenderer to use the single-level textured renderer."
