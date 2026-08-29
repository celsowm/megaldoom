#!/usr/bin/env python3
"""Contracts for the five-weapon arsenal: VRAM window, table shape, palette lock.

The two things that can silently break this feature are (a) a resprite pushing
the streamed weapon tileset past the VRAM the region below the SGDK font can
hold, and (b) an asset regeneration re-weighting PAL3 and recolouring the whole
game. Both are asserted here against the generated headers, so neither can land
without this test going red.
"""

from pathlib import Path
import importlib.util
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
RENDERER_ASSETS = (ROOT / "src/renderer/generated_renderer_assets.h").read_text()
HUD_ASSETS = (ROOT / "src/renderer/generated_hud_assets.h").read_text()
WORLD_ASSETS = (ROOT / "src/bsp/generated_assets.h").read_text()
INTERNAL = (ROOT / "src/renderer/renderer_internal.h").read_text()
RENDERER_C = (ROOT / "src/renderer/renderer.c").read_text()
OVERLAY_C = (ROOT / "src/renderer/renderer_frame_overlay.c").read_text()
ASSET_CONVERTER = (ROOT / "tools/convert-freedoom-assets.ps1").read_text()
WEAPONS_H = (ROOT / "src/weapons.h").read_text()
WEAPONS_C = (ROOT / "src/weapons.c").read_text()
MAIN_C = (ROOT / "src/main.c").read_text()
BILLBOARD_C = (ROOT / "src/billboard/billboard.c").read_text()
RESOURCES = (ROOT / "res/resources.res").read_text()

WEAPON_ORDER = ["FIST", "CHAINSAW", "PISTOL", "SHOTGUN", "CHAINGUN"]


def define(text: str, name: str) -> int:
    match = re.search(rf"#define {name}\s+(\d+)", text)
    assert match, f"{name} is missing"
    return int(match.group(1))


# --- The arsenal is one list, declared in three places -----------------------
enum_names = re.findall(
    r"WEAPON_(\w+)[,\s]",
    re.search(r"typedef enum \{(.*?)\} WeaponId;", WEAPONS_H, re.S).group(1))
assert enum_names == WEAPON_ORDER + ["COUNT"], enum_names
assert define(RENDERER_ASSETS, "MEGALDOOM_WEAPON_COUNT") == len(WEAPON_ORDER)
assert define(HUD_ASSETS, "FREEDOOM_WEAPON_COUNT") == len(WEAPON_ORDER)

rows = re.findall(r"^    \[WEAPON_(\w+)\] = \{\n(.*?)\n    \},$",
                  WEAPONS_C, re.S | re.M)
assert [name for name, _ in rows] == WEAPON_ORDER, [n for n, _ in rows]

for name, body in rows:
    fields = [field.strip() for field in body.replace("\n", " ").split(",")]
    ammo_type, _per_shot, pellets, spread, _melee, cooldown, flash, automatic = fields[:8]
    sfx = fields[8]
    assert ammo_type in ("AMMO_NONE", "AMMO_BULLETS", "AMMO_SHELLS"), (name, ammo_type)
    assert automatic in ("TRUE", "FALSE"), (name, automatic)
    assert int(cooldown) >= 1 and int(flash) >= 1, name
    # Every weapon must name a sound that actually exists in the ROM.
    assert f"WAV {sfx} " in RESOURCES or re.search(rf"WAV {sfx}\s", RESOURCES), (name, sfx)
    # A multi-pellet weapon needs a spread, or every pellet would hit the same
    # target and the shotgun would just be a slow pistol.
    if int(pellets) > 1:
        assert int(spread) > 0, name

# Melee weapons cost no ammo; ammo weapons cost some.
melee = {name for name, body in rows if "AMMO_NONE" in body}
assert melee == {"FIST", "CHAINSAW"}, melee

# --- VRAM: one streamed window, sized to the largest weapon ------------------
max_tiles = define(RENDERER_ASSETS, "MEGALDOOM_WEAPON_MAX_TILE_COUNT")
counts = [int(value) for value in re.search(
    r"MEGALDOOM_WEAPON_TILE_COUNTS\[MEGALDOOM_WEAPON_COUNT\] = \{\s*([\d,\s]+?)\s*\}",
    RENDERER_ASSETS, re.S).group(1).split(",")]
assert len(counts) == len(WEAPON_ORDER), counts
assert max(counts) == max_tiles, (counts, max_tiles)

# WEAPON_TILE_BASE is a chain of defines; resolve it the same way the C does.
tile_base = (16                                                    # TILE_USER_INDEX
             + 20 * 15 * 2                                         # VIEW_DYNAMIC_TILE_COUNT
             + define(INTERNAL, "PAIR_TILE_COUNT")
             + define(HUD_ASSETS, "FREEDOOM_HUD_TILE_COUNT")
             + define(HUD_ASSETS, "FREEDOOM_FACE_TILE_COUNT")
             + 78)                                                 # HUD_NUMBER_TILE_COUNT
limit = define(INTERNAL, "HUD_VRAM_SAFE_TILE_LIMIT")
assert tile_base + max_tiles <= limit, (
    f"weapon window {tile_base}..{tile_base + max_tiles} overruns the SGDK font "
    f"region at {limit}; shrink FREEDOOM_WEAPON_RECT_W/H in "
    f"tools/convert-freedoom-assets.ps1 rather than raising the limit")
# The compile-time guard must exist too, so a regeneration fails the build and
# not just this test.
assert "(WEAPON_TILE_BASE + MEGALDOOM_WEAPON_MAX_TILE_COUNT) > HUD_VRAM_SAFE_TILE_LIMIT" in RENDERER_C
assert "reload_weapon_tiles();" in RENDERER_C
assert "MEGALDOOM_WEAPON_TILE_COUNTS[g_weapon_id]" in OVERLAY_C
assert "MEGALDOOM_WEAPON_TILEMAP[g_weapon_id][variant][i]" in OVERLAY_C
# A weapon change must invalidate the variant cache, or the new weapon would
# keep drawing with the old one's tile indices.
assert "g_last_weapon_variant = -1;" in OVERLAY_C

# The fist is the only weapon rendered in PAL2. It walks the portrait palette's
# flesh ramp by luminance, keeping skin out of PAL3's amber endpoint AND out of
# PAL2's own chalk/red slots; every other weapon remains on PAL3 and retains its
# established bake.
assert "$weaponWarmPaletteIndices = @(4, 6, 8, 12)" in ASSET_CONVERTER
assert "function Get-NearestWeaponWarmIndex" in ASSET_CONVERTER
assert '$armorOlivePaletteIndices = @(3, 7, 9, 10, 13)' in ASSET_CONVERTER
assert 'Name = "FIST";     Idle = "PUNGA0"; Fire = "PUNGC0"; Flash = "";       Legacy = $false; Palette = "Face"' in ASSET_CONVERTER
assert ASSET_CONVERTER.count('Palette = "World"') == 4
assert "function Get-FistSkinIndex" in ASSET_CONVERTER
assert 'return Get-FistSkinIndex $Color' in ASSET_CONVERTER
assert 'Convert-WeaponFrame $weapon.Idle "" $weapon.Palette' in ASSET_CONVERTER
assert "if ($FireFrame)" in ASSET_CONVERTER
assert "if ($layer.IsFlash)" in ASSET_CONVERTER
assert 'const u16 palette = (g_weapon_id == WEAPON_FIST) ? PAL2 : PAL3;' in OVERLAY_C
assert 'TILE_ATTR_FULL(palette, FALSE, FALSE, FALSE,' in OVERLAY_C

# Every weapon bakes into the SAME 8x5 tile rectangle: the overlay draws one
# fixed BG_A rect, so a weapon that needed a different one would be clipped.
assert define(RENDERER_ASSETS, "MEGALDOOM_WEAPON_TILE_W") == 8
assert define(RENDERER_ASSETS, "MEGALDOOM_WEAPON_TILE_H") == 5
rect_w = define(HUD_ASSETS, "FREEDOOM_WEAPON_RECT_W")
rect_h = define(HUD_ASSETS, "FREEDOOM_WEAPON_RECT_H")
assert (define(HUD_ASSETS, "FREEDOOM_WEAPON_RECT_X") % 8 == 0) and (rect_w % 8 == 0)
assert (define(HUD_ASSETS, "FREEDOOM_WEAPON_RECT_Y") % 8 == 0) and (rect_h % 8 == 0)
assert rect_w // 8 == 8 and rect_h // 8 == 5

# --- The fist's baked pixels stay on the flesh ramp --------------------------
# Asserting the converter's ramp is not enough: what ships is the header. The
# face palette's slots 13/14/15 are near-white/grey and 9/10 are red, so a fist
# that reaches them is either chalky and blotchy (the nearest-RGB bake) or
# sunburnt. Reading the tiles back also catches a resprite that silently
# reintroduces them.
_fist_body = re.search(
    r"MEGALDOOM_WEAPON_TILES.*?\{\s*\{(.*?)\n  \},", RENDERER_ASSETS, re.S).group(1)
fist_indices = {(word >> shift) & 0xF
                for word in (int(value, 16)
                             for value in re.findall(r"0x[0-9A-Fa-f]{8}", _fist_body))
                for shift in range(0, 32, 4)}
assert not (fist_indices & {9, 10, 13, 14, 15}), (
    f"FIST bake reached the face palette's red/chalk slots: {sorted(fist_indices)}")
assert len(fist_indices - {0}) >= 5, (
    f"FIST bake lost its shading: {sorted(fist_indices)}")

# --- PAL3 is frozen ----------------------------------------------------------
# The world palette is shared by walls, items, enemies AND the weapon overlay.
# New weapon sprites are quantized against it; they must never be fed into the
# histogram that produces it (WORLD_SPRITE_INPUTS in tools/world_assets.py),
# because that would recolour the entire game.
#
# Updated 2026-08-15: PAL3 was deliberately rebalanced (dedicated dark-warm
# slot, chroma-based neutral clamp, tone curve on wall/flat inputs) to fix
# E1M1's dark brown corridors collapsing onto the floor's own grey -- see the
# "temos-problema-na-hora" wall-quality plan. This lock exists to catch
# ACCIDENTAL PAL3 drift from routine wall-only tuning, not to forbid an
# intentional, user-approved rebalance; when that happens on purpose, update
# this constant to the new FREEDOOM_WORLD_PALETTE and re-verify the weapon/
# billboard bake still reads correctly (visual check, not just this test).
#
# The literal 16 colours are pinned once, in tools/test-flat-map-recipes.py.
# This check reads the same tuple from tools/world_assets.py, so what it locks
# is that the *generated header the weapon bakes against* still matches the
# source of truth -- a third transcription of the palette would only be a third
# place to forget to update.
def _load_world_assets():
    """tools/world_assets.py holds FROZEN_WORLD_PALETTE, the single definition
    of PAL3. Loaded by path because tools/ is not a package and the module
    imports its own sibling world_palette."""
    sys.path.insert(0, str(ROOT / "tools"))
    try:
        spec = importlib.util.spec_from_file_location(
            "world_assets_weapons", ROOT / "tools" / "world_assets.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module
    finally:
        sys.path.pop(0)


world_assets_module = _load_world_assets()
FROZEN_PALETTE = ["%02X%02X%02X" % color
                  for color in world_assets_module.FROZEN_WORLD_PALETTE]
palette = re.findall(r"0x([0-9A-Fa-f]{6})", re.search(
    r"FREEDOOM_WORLD_PALETTE\[16\]\s*=\s*\{(.*?)\}", WORLD_ASSETS, re.S).group(1))
assert [entry.upper() for entry in palette] == FROZEN_PALETTE, palette
extract = (ROOT / "tools/world_assets.py").read_text()
sprite_inputs = re.search(r"WORLD_SPRITE_INPUTS = \[(.*?)\]", extract, re.S).group(1)
for lump in ("SHTG", "CHGG", "PUNG", "SAWG", "SHOTA0", "MGUNA0", "CSAWA0"):
    assert lump not in sprite_inputs, (
        f"{lump} was added to the PAL3 histogram; that recolours the whole game")

# --- Gameplay wiring ---------------------------------------------------------
for token in (
    "PlayerArsenal", "arsenal.ammo", "weapon_cycle(", "weapon_has_ammo(",
    "renderer_set_weapon(", "fire_weapon(", "WEAPON_RAISE_VBLANKS",
    "BILLBOARD_EFFECT_WEAPON", "add_ammo(&arsenal",
):
    assert token in MAIN_C, token
# Doom's pistol start, and a level reset must restore it.
assert "arsenal->ammo[AMMO_BULLETS] = WEAPON_START_BULLETS;" in MAIN_C
assert "arsenal->owned = WEAPON_START_OWNED;" in MAIN_C
assert "arsenal->current = WEAPON_PISTOL;" in MAIN_C
assert "renderer_set_weapon(arsenal->current);" in MAIN_C
# Damage is deterministic: the BlastEm route harness replays fixed input and
# compares outcomes, so a PRNG here would make combat unreproducible.
assert "random(" not in WEAPONS_C and "rand(" not in WEAPONS_C
roll = [int(value) for value in re.search(
    r"DAMAGE_ROLL\[3\] = \{([^}]*)\}", WEAPONS_C).group(1).split(",")]
assert sorted(roll) == [5, 10, 15], roll
assert sum(roll) // len(roll) == 10, roll

# --- Pickups -----------------------------------------------------------------
for thing, name in ((2001, "SHOTGUN"), (2002, "CHAINGUN"), (2005, "CHAINSAW"),
                    (2008, "SHELLS"), (2049, "SHELL_BOX")):
    assert f"case {thing}:" in BILLBOARD_C, thing
    assert f"BILLBOARD_TYPE_{name}" in BILLBOARD_C, name
# Doom's amounts, and the enemy/barrel HP they are balanced against.
assert "result.amount = 50; result.ammo_type = AMMO_BULLETS;" in BILLBOARD_C
assert "result.amount = 4;  result.ammo_type = AMMO_SHELLS;" in BILLBOARD_C
assert "result.amount = 20; result.ammo_type = AMMO_SHELLS;" in BILLBOARD_C
assert re.search(r"\{BILLBOARD_VISUAL_DUMMY,\s+BILLBOARD_EFFECT_NONE,\s+20,", BILLBOARD_C)
assert "const u16 AMMO_MAX[AMMO_TYPE_COUNT] = { 0, 200, 50 };" in WEAPONS_C

print(f"ok    weapons: {len(WEAPON_ORDER)} weapons stream through "
      f"{max_tiles} tiles at {tile_base}, PAL3 frozen")
