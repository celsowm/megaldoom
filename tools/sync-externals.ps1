param(
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$ExternalsRoot = Join-Path $Root ".extenals"
$FreedoomDir = Join-Path $ExternalsRoot "freedoom"
$FreedoomUrl = "https://github.com/freedoom/freedoom.git"
$DocsDir = Join-Path $Root "docs"
$ExternalAssetsDoc = Join-Path $DocsDir "external-assets.md"

function Write-Step($Message) {
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Ensure-Dir($Path) {
    if (-not (Test-Path $Path)) {
        New-Item -ItemType Directory -Path $Path | Out-Null
    }
}

function Assert-ChildPath([string]$Parent, [string]$Child) {
    $parentFull = [System.IO.Path]::GetFullPath($Parent)
    $childFull = [System.IO.Path]::GetFullPath($Child)
    if (-not $childFull.StartsWith($parentFull, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to operate outside $parentFull"
    }
}

Ensure-Dir $ExternalsRoot
Ensure-Dir (Join-Path $ExternalsRoot "tools")
Ensure-Dir (Join-Path $ExternalsRoot "cache")
Ensure-Dir $DocsDir

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    throw "git was not found in PATH."
}

if ((Test-Path $FreedoomDir) -and $Force) {
    Assert-ChildPath $ExternalsRoot $FreedoomDir
    Write-Step "Removing existing Freedoom checkout"
    Remove-Item -LiteralPath $FreedoomDir -Recurse -Force
}

if (-not (Test-Path $FreedoomDir)) {
    Write-Step "Cloning Freedoom into .extenals/freedoom"
    git clone --depth 1 $FreedoomUrl $FreedoomDir
} else {
    Write-Step "Updating existing Freedoom checkout"
    git -C $FreedoomDir fetch --depth 1 origin
    git -C $FreedoomDir checkout --detach FETCH_HEAD
}

$commit = (git -C $FreedoomDir rev-parse HEAD).Trim()
$shortCommit = (git -C $FreedoomDir rev-parse --short HEAD).Trim()
$commitDate = (git -C $FreedoomDir log -1 --format=%cI).Trim()
$origin = (git -C $FreedoomDir remote get-url origin).Trim()
$syncDate = (Get-Date).ToString("yyyy-MM-ddTHH:mm:ssK")

$doc = @"
# External assets

This file records local, non-versioned external sources used by MegalDoom asset tooling.

## Freedoom

- Repository: $origin
- Local path: .extenals/freedoom
- Checked commit: $commit
- Short commit: $shortCommit
- Commit date: $commitDate
- Synced at: $syncDate
- License source: .extenals/freedoom/COPYING.adoc

Usage policy:

- Freedoom is used as a free asset/reference source, not as an engine base.
- The .extenals/ checkout is not versioned.
- Converted Mega Drive-ready assets generated into res/ may be versioned later with their origin documented here.
- Do not rely on a floating branch without updating this file with the exact commit.
"@

Set-Content -Path $ExternalAssetsDoc -Value $doc -NoNewline

Write-Host ""
Write-Host "External sources synced:" -ForegroundColor Green
Write-Host "  Freedoom: $shortCommit"
Write-Host "  Metadata: $ExternalAssetsDoc"
