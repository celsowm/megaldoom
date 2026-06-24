# External assets

This file records local, non-versioned external sources used by MegalDoom asset tooling.

## Freedoom

- Repository: https://github.com/freedoom/freedoom.git
- Local path: .extenals/freedoom
- Checked commit: d14dbbee3b6fbfb2c11cdb65eb61216e86d4ee85
- Short commit: d14dbbe
- Commit date: 2026-06-22T12:51:41-03:00
- Synced at: 2026-06-23T14:01:43-03:00
- License source: .extenals/freedoom/COPYING.adoc

Usage policy:

- Freedoom is used as a free asset/reference source, not as an engine base.
- The .extenals/ checkout is not versioned.
- Converted Mega Drive-ready assets generated into res/ may be versioned later with their origin documented here.
- Do not rely on a floating branch without updating this file with the exact commit.

## Converted assets

### FREEDOOM_WALL_TEXTURE

- Source: `.extenals/freedoom/patches/stonew1.png`
- Generated file: `src/generated_assets.h`
- Conversion script: `tools/convert-freedoom-assets.ps1`
- Output format: 8x8 palette-index texture using the current 16-color prototype palette.
- Source commit: `d14dbbee3b6fbfb2c11cdb65eb61216e86d4ee85`

### FREEDOOM_BILLBOARD_BONUS_TEXTURE

- Source: `.extenals/freedoom/sprites/bon1a0.png`
- Generated file: `src/generated_billboard_assets.h`
- Conversion script: `tools/convert-freedoom-assets.ps1 -BillboardOnly`
- Output format: 8x8 palette-index texture using palette index 0 as transparent.
- Source commit: `d14dbbee3b6fbfb2c11cdb65eb61216e86d4ee85`

### FREEDOOM_BILLBOARD_KEY_TEXTURE

- Source: `.extenals/freedoom/sprites/bkeya0.png`
- Generated file: `src/generated_billboard_assets.h`
- Conversion script: `tools/convert-freedoom-assets.ps1 -BillboardOnly`
- Output format: 8x8 palette-index texture using palette index 0 as transparent.
- Source commit: `d14dbbee3b6fbfb2c11cdb65eb61216e86d4ee85`

### FREEDOOM_BILLBOARD_DECOR_TEXTURE

- Source: `.extenals/freedoom/sprites/bar1a0.png`
- Generated file: `src/generated_billboard_assets.h`
- Conversion script: `tools/convert-freedoom-assets.ps1 -BillboardOnly`
- Output format: 8x8 palette-index texture using palette index 0 as transparent.
- Source commit: `d14dbbee3b6fbfb2c11cdb65eb61216e86d4ee85`
