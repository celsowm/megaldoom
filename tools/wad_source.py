"""The one pinned fingerprint for the campaign's source IWAD.

Doom Registered v1.9 (id Software, 1995-02-01, 11,159,840 bytes) -- the
3-episode commercial IWAD, deliberately NOT shareware (doom1.wad, md5
f0cefca49926d00903cf57551d901abe) and NOT The Ultimate Doom (4-episode
doom.wad, md5 c4fe9fd920207691a9f493668e0a2083). Confirmed against the
libretro-database and DoomWiki checksum lists:
    md5    1cd63c5ddff1bf8ce844237f580e9cf3
    sha256 FF2C301B8719465A6E386A512BFA319931B7F64EA517D337C5A47AFE03951902

Every tool that reads DOOM1.WAD (tools/wad-map-extract.py,
tools/wall_bake_preview.py, ...) checks its live sha256 against this single
constant rather than keeping its own copy, so there is exactly one place to
update if the source is ever re-verified again.
"""

EXPECTED_CAMPAIGN_WAD_SHA256 = (
    "FF2C301B8719465A6E386A512BFA319931B7F64EA517D337C5A47AFE03951902"
)
