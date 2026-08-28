#!/usr/bin/env python3
"""Bake Doom's wall textures and flats into MegalDoom's PAL3 palette and the
FREEDOOM_WALL_TEXTURES / FREEDOOM_SECTOR_VISUALS C tables.

Knows nothing about BSP geometry -- its inputs are a texture-name usage count
and a list of sectors (each a dict with floor/ceiling names and a light
level), and its output is one generated C header plus the (texture_ids,
texture_meta, sector_visuals, palette) bsp_emit.py needs to resolve each
seg's texture id.
"""

import os
import re
from collections import Counter
from dataclasses import dataclass

from PIL import Image
import raycast_constants
import world_palette

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSET_ROOT = os.path.join(PROJECT_ROOT, "res", "originaldoom")

# Converter and runtime intentionally share the public definition in raycast.h,
# read through tools/raycast_constants.py so every offline consumer resolves it
# the same way.
WALL_TEX_WIDTH, WALL_TEX_HEIGHT = raycast_constants.wall_tex_dims()
# WALL_TEX_WIDTH is the number of PAIR COLUMNS the runtime indexes (tex_x), not
# the number of texels a row shows. Each packed byte covers the two horizontal
# pixels of one stride-2 sample, and both nibbles used to hold the same index --
# so a row displayed 128 pixels wide only ever carried 64 distinct texels.
# Baking at WALL_TEX_DISPLAY_WIDTH and packing the two nibbles independently
# gives every displayed pixel its own texel at exactly the same ROM cost and the
# same single `move.b` in the hotpath.
WALL_TEX_DISPLAY_WIDTH = 2 * WALL_TEX_WIDTH
# The curated COMPUTE2 facade is hand-authored on this square lattice. It is
# composed there and resampled onto the bake grid, so widening the bake grid
# cannot silently re-register the artwork.
COMPUTE2_FACADE_DIM = 64
# The COMPUTE2 facade's structure, named once so the bake and the preview's
# certifier cannot drift. certify_compute2_facade used to restate the rails as
# literals and pin the readouts to palette index 9, which quietly meant "the
# green one" -- so the contract broke the moment PAL3's green slot changed use,
# even though the facade itself was still correct.
COMPUTE2_RAIL_ROWS = ((0, 3), (16, 20), (34, 38), (51, 55), (62, 64))
COMPUTE2_INSTRUMENT_COLUMNS = ((3, 30), (34, 61))
COMPUTE2_READOUT_BAYS = ((6, 12), (23, 30), (42, 48))
COMPUTE2_ACCENT_INDICES = frozenset({1, 4, 6, 8, 9, 11, 12, 13, 14, 15})
# Share of horizontally adjacent texels whose palette index differs. The prior
# "ugly walls" investigation concluded the defect was churn, not resolution, so
# this ceiling governs whether a material is allowed sub-texel horizontal
# detail at all (see convert_texture). tools/test-wall-quality.py asserts the
# same constant rather than restating the number.
WALL_CHURN_LIMIT = 0.35
FALLBACK_TEXTURE = "__FALLBACK__"
FALLBACK_TEXTURE_SOURCE = "GRAY7"
WORLD_COLOR_DAMAGE = 14
WORLD_COLOR_WARNING = 15
WORLD_SPRITE_INPUTS = [
    "PISGA0", "PISGB0", "PISFA0",
    "BON1A0", "BKEYA0", "YKEYA0", "RKEYA0", "STIMA0", "MEDIA0",
    "BON2A0", "ARM1A0", "ARM2A0", "CLIPA0", "AMMOA0", "CANDA0",
    "CBRAA0", "COLUA0", "ELECA0", "BAR1A0", "TREDA0",
    "POSSA1", "POSSB1", "POSSC1", "POSSD1", "POSSF1", "POSSH0",
    "POSSI0", "POSSJ0", "POSSK0", "POSSL0",
    "BEXPA0", "BEXPB0", "BEXPC0", "BEXPD0", "BEXPE0",
    "PUFFA0", "PUFFB0", "PUFFC0", "PUFFD0", "BLUDA0", "BLUDB0", "BLUDC0",
]
WEAPON_SPRITE_NAMES = ("PISGA0", "PISGB0", "PISFA0")

# PAL3 indices are a runtime asset ABI shared by walls, flats, weapons and
# billboards.  The adaptive search produced this user-approved palette, but
# must not rerun when a map recipe adds one texture: GLOBAL_FLOOR_INDEX is 7,
# so moving a slot into 7 repaints the entire floor.  Keep the exact checked-in
# order stable; palette changes now require an explicit, separately reviewed
# update to this constant and the frozen-palette tests.
#
# Slot 9 is khaki (#484824), not the green it used to hold.  Measured over
# E1M1 weighted by wall length, the green was dead weight -- 0.11% of wall
# area, 0.53% of billboard pixels, 0.00% of the weapon overlay -- while the
# olive/khaki hue band (105-150 deg in Oklab) covers 15.1% of wall area and
# had no palette entry at all.  BROWNGRN alone is 26.8% of E1M1's walls and
# was quantizing 91.5% neutral: a material named brown-green rendering as
# plain grey.  The band was unreachable by construction, because both
# world_palette.build_palette and the frozen-palette test excluded is_olive
# outright.  With this slot the map's neutral share drops 61.9% -> 50.7%, and
# the materials that should stay grey do (COMPUTE2 97.9 -> 98.8%, LITE3 /
# SUPPORT2 / STARGR1 all 100 -> 100%), which is what says the khaki is being
# reached by genuinely olive texels rather than leaking as a cast.
FROZEN_WORLD_PALETTE = (
    (0x00, 0x00, 0x00), (0x24, 0x00, 0x00),
    (0x00, 0x00, 0x6D), (0x24, 0x24, 0x24),
    (0x48, 0x24, 0x24), (0x48, 0x48, 0x48),
    (0x6D, 0x48, 0x24), (0x6D, 0x6D, 0x6D),
    (0x91, 0x6D, 0x48), (0x48, 0x48, 0x24),
    (0x91, 0x91, 0x91), (0xFF, 0x48, 0x48),
    (0xB6, 0x91, 0x6D), (0xB6, 0xB6, 0xB6),
    (0xDA, 0x24, 0x24), (0xDA, 0xB6, 0x48),
)


def texture_macro(name):
    if name == FALLBACK_TEXTURE:
        return "MEGALDOOM_TEX_FALLBACK"
    return "MEGALDOOM_TEX_" + re.sub(r"[^A-Z0-9_]", "_", name)


def texture_path(name):
    source = FALLBACK_TEXTURE_SOURCE if name == FALLBACK_TEXTURE else name
    return os.path.join(ASSET_ROOT, "textures", source + ".png")


def flat_path(name):
    if name == "F_SKY1":
        return os.path.join(ASSET_ROOT, "textures", "SKY1.png")
    return os.path.join(ASSET_ROOT, "flats", name + ".png")


def md_color(rgb):
    """Quantize RGB to the Mega Drive's three bits per channel."""
    return world_palette.md_color(rgb)


# Tuned so E1M1's dominant wall/flat median luminance (~39/255, see
# tools/test-wall-quality.py dark-brown checks) lands near the middle of
# PAL3's ramp instead of the bottom two rungs. This is a display-space
# "levels lift", not a physically accurate linear-light gamma: it exists to
# recover the light-level compensation the renderer does NOT apply to wall
# texels (see WALL_SHADE_MODE in renderer_pack.c, which only shades by depth
# and N/S side, never by the source sector's light field). Applied uniformly
# to every wall/flat pixel, so relative differences between materials (a lit
# STARTAN3 corridor vs a dark BROWN144 one) are preserved, not equalized.
WALL_TONE_GAMMA = 0.55


def tone_curve(rgb):
    return tuple(int(round(255.0 * ((max(0, min(255, channel)) / 255.0) ** WALL_TONE_GAMMA)))
                 for channel in rgb)


def _fill_transparent_with_average(image):
    """Return an opaque RGB copy, filling any transparent area with the
    average of the image's own opaque pixels instead of black.

    Composited wall textures (TEXTURE1/2 patches over PNAMES) can leave real
    gaps -- BRNBIGC is 28% uncovered -- and Image.convert("RGB") on an RGBA
    source keeps whatever RGB the transparent pixels were initialized to
    (black, per wad-extract.py's Image.new("RGBA", ..., (0,0,0,0))). Composing
    over black instead of the texture's own tone painted 26% of BRNBIGC's
    quantized output as literal black pixels; composing over the texture's own
    average keeps the gap visually neutral instead.
    """
    if image.mode != "RGBA":
        return image.convert("RGB")
    rgba = image.convert("RGBA")
    pixels = list(rgba.get_flattened_data())
    opaque = [(r, g, b) for r, g, b, a in pixels if a >= 128]
    if not opaque or len(opaque) == len(pixels):
        return rgba.convert("RGB")
    fill = tuple(sum(p[channel] for p in opaque) // len(opaque) for channel in range(3))
    background = Image.new("RGB", rgba.size, fill)
    background.paste(rgba, (0, 0), rgba)
    return background


def image_average(path):
    with Image.open(path) as image:
        opaque = _fill_transparent_with_average(image)
        pixels = list(opaque.get_flattened_data())
    pixels = [tone_curve(p) for p in pixels]
    count = max(1, len(pixels))
    return tuple(sum(p[channel] for p in pixels) // count for channel in range(3))


def add_image_histogram(histogram, path, size, weight, apply_tone=False):
    with Image.open(path) as image:
        rgba = image.convert("RGBA").resize(size, Image.Resampling.BOX)
        for r, g, b, a in rgba.get_flattened_data():
            if a >= 128:
                color = tone_curve((r, g, b)) if apply_tone else (r, g, b)
                histogram[color] += weight


# Charged only when a candidate carries LESS chroma than the source, so a
# neutral texel is never tinted (see world_palette.distance_sq). 8.0 is where
# the E1M1 sweep flattens: map-wide neutral share 50.7 / 43.8 / 42.2 / 40.9 /
# 40.4% at weights 0 / 2 / 4 / 8 / 16, with STARTAN3 -- 20.9% of the map --
# going 70.2% -> 52.5% neutral and LITE3 / SUPPORT2 / STONE2 pinned at 100%
# throughout. Sprites, the weapon and billboards keep the unweighted metric:
# tools/world-palette-lut.py calls best_mix without this, so PAL3 rebalancing
# for walls cannot recolour them.
WALL_CHROMA_LOSS_WEIGHT = 8.0


def nearest_palette_index(rgb, palette, allowed=None):
    return world_palette.nearest_index(rgb, palette, allowed,
                                       WALL_CHROMA_LOSS_WEIGHT)


def build_world_palette(texture_names, texture_usage, sectors):
    # Keep the parameters in the interface: texture conversion callers still
    # describe the active catalog, and a future intentional palette redesign
    # can use that evidence offline.  Routine map conversion must be invariant.
    del texture_names, texture_usage, sectors
    return list(FROZEN_WORLD_PALETTE)


def shade_family(color):
    """The hue family a shade chain is allowed to move within.

    build_shade_map's second invariant is that fog must not shift hue, so a
    chain may only step to a darker member of its own family. Naming the
    families once here (rather than restating the filter per branch) is what
    makes adding one safe: khaki has no darker sibling in PAL3, so it saturates
    on itself instead of falling into the maroon at index 4, which is the
    cross-family fallback the docstring below calls out as the bug.
    """
    if world_palette.is_neutral(color):
        return "neutral"
    if world_palette.is_green(color):
        return "green"
    if world_palette.is_olive(color):
        return "olive"
    return "chromatic"


def build_shade_map(palette, reserved=()):
    """One step of depth darkening, as palette indices.

    Three properties this chain must have, none of which it had before depth
    shading was switched on in renderer_pack.c:

    * It never reaches index 0, and never reaches `reserved` (the two flat
      colours). Otherwise distance fog does the exact thing the global flats
      were chosen to prevent -- a far wall darkens until it IS the ceiling
      colour, or drops to pure black and reads as a hole rather than as fog.
    * The family filters are hard. They used to fall back to a cross-family
      candidate when the in-family set emptied, which sent dark greys to index
      1 (#240000) and index 11 to the damage red -- fog that shifts hue.
    * An index whose candidate set empties saturates on itself, so shading
      stops at a mid tone rather than falling through the bottom of the ramp.

    The per-step factor is 3/4 rather than 2/3: chained across the four levels
    the old factor reached 0.30x source brightness, far past Doom's own fog, and
    flattened almost every material into a single index at the far end.
    """
    forbidden = set(reserved) | {0}
    result = []
    for index, color in enumerate(palette):
        if index == 0:
            result.append(0)
            continue
        luminance = world_palette.oklab(tuple(color))[0]
        darker = [i for i, candidate in enumerate(palette)
                  if world_palette.oklab(tuple(candidate))[0] < luminance - 1e-6
                  and i not in forbidden and i < WORLD_COLOR_DAMAGE]
        family = shade_family(color)
        darker = [i for i in darker if shade_family(palette[i]) == family]
        target = tuple(channel * 3 // 4 for channel in color)
        result.append(nearest_palette_index(target, palette, darker or [index]))
    return result


def build_shade_lut(palette, levels=4, reserved=()):
    """Build deterministic progressively-darker palette-index shade levels."""
    shade_map = build_shade_map(palette, reserved)
    result = [list(range(len(palette)))]
    for _ in range(1, levels):
        result.append([shade_map[index] for index in result[-1]])
    return result


# Sources wider than the display grid still lose detail with nothing gained:
# COMPUTE2 (256x56) squashed 4:1 turned a panel of small readouts into
# horizontal mush. The cap keeps the material repeating over half the world
# distance instead. Now that the grid is WALL_TEX_DISPLAY_WIDTH, a 128-wide
# source is carried 1:1 rather than averaged 2:1, which is where the 26 of 53
# textures with 128-wide sources recover their structure.
WALL_TEX_MAX_SOURCE_WIDTH = WALL_TEX_DISPLAY_WIDTH

@dataclass(frozen=True)
class WallBakeRecipe:
    """Offline-only simplification for a detail-heavy wall material.

    The recipe never changes the runtime texture format. ``source_window``
    selects the recognisable part of a Doom texture; the remaining fields
    control an edge-aware low-pass on the fixed 64x64 bake and one conservative
    post-quantization cleanup pass.
    """

    source_window: tuple | None = None
    edge_lightness_threshold: float = 0.09
    edge_colour_threshold: float = 0.04
    smooth_weight: int = 4
    cleanup_isolated: bool = True
    # Optional detail window inside ``source_window``.  This does not change
    # the Doom/world repeat metadata: it chooses one representative facade
    # module to fill the fixed 64x64 runtime texture instead of compressing
    # several tiny modules until their controls become single-pixel noise.
    facade_window: tuple | None = None


# Only structural technological walls are curated. Doors and switches retain
# their exact old conversion, as do all brown/stone materials. The thresholds
# match the perceptual separation already used by the flat/wall collision
# certificate below: a change large enough to identify a panel boundary is
# preserved, while lower-frequency ripple inside a panel is simplified.
TECH_WALL_MATERIALS = (
    "COMPTILE", "COMPUTE2", "LITE3", "STARG3",
    "STARGR1", "STARTAN1", "STARTAN3", "SUPPORT2",
)
CURATED_WALL_MATERIALS = TECH_WALL_MATERIALS + ("TEKWALL1", "TEKWALL4")
WALL_BAKE_RECIPES = {
    name: WallBakeRecipe(
        source_window=(128, 0, 128, 56) if name == "COMPUTE2" else None,
        # The rightmost 64px module contains the large green waveform bank,
        # metal control panel and lower cabinets.  Sampling it 1:1 keeps those
        # structures legible in the 160px-wide stride-2 viewport.  The public
        # source_window remains 128px so the wall's world-space repeat does not
        # change; this is an offline facade selection only.
        facade_window=(64, 0, 64, 56) if name == "COMPUTE2" else None,
    )
    for name in CURATED_WALL_MATERIALS
}

# Compatibility/readability view used by sampled_texture_dimensions() and by
# the existing flat-map material-transfer tests. Source selection is part of
# both the checked-in baseline and the candidate; only the edge-aware bake is
# optional when producing an A/B preview.
CURATED_TEXTURE_WINDOWS = {
    name: recipe.source_window
    for name, recipe in WALL_BAKE_RECIPES.items()
    if recipe.source_window is not None
}


def sampled_texture_width(width):
    """Source width actually sampled into the display grid."""
    return min(width, WALL_TEX_MAX_SOURCE_WIDTH)


def sampled_texture_dimensions(material_name, width, height):
    """Return the source window dimensions baked for ``material_name``."""
    window = CURATED_TEXTURE_WINDOWS.get(material_name.upper())
    if window is None:
        return sampled_texture_width(width), height
    left, top, sampled_width, sampled_height = window
    if (left < 0 or top < 0 or sampled_width <= 0 or sampled_height <= 0 or
            left + sampled_width > width or top + sampled_height > height):
        raise ValueError("Invalid %s texture window %s for %dx%d source" %
                         (material_name, window, width, height))
    return sampled_width, sampled_height


# Oklab-chroma clamp for wall/flat quantization: separates real dark browns
# (BROWN144 chroma~0.026, BROWN96 chroma~0.033) from truly neutral materials
# (COMPUTE2 chroma~0.008, BROWNGRN chroma~0.014) that the old absolute RGB
# spread of 36 could not tell apart. Sprites keep the legacy RGB clamp (see
# tools/world-palette-lut.py, which calls best_mix with no override) so this
# tuning cannot recolor the weapon or billboards.
WALL_CHROMA_THRESHOLD = 0.02
# Dither pairing for textured walls, now vestigial: convert_texture passes
# gain_ratio 0, which makes best_mix always return its solid primary, so no pair
# is ever emitted and this bound is never reached. Kept because best_mix still
# requires the argument, and because it documents the tightening that came
# before the dither was removed outright -- at RAY_COL_STRIDE 2 the sprite-tuned
# 73 was producing a visible checkerboard. The GRAY/METAL/STONE neutral bucket
# still passes the sprite defaults and is unaffected.
WALL_PAIR_MAX_DELTA = 40

# PAL3's neutral rungs sit ~0.13 apart in Oklab L (idx 3=0.260, 5=0.402,
# 7=0.535, 10=0.657, 13=0.776). Several E1M1 materials have a p2..p98 luminance
# spread *narrower than one rung* -- BROWN144 0.109, BROWNGRN 0.169 -- so the
# quantizer had no choice but to collapse 39-75% of their texels onto a single
# index, which then read as a solid block the same colour as the floor. The fix
# is a per-texture contrast expansion around the material's OWN median: a dark
# BROWN144 corridor stays dark (its mean luminance is preserved) but regains
# internal structure. Only L is scaled; chroma rides along, so browns stay
# brown. This is the same "levels lift" WALL_TONE_GAMMA above already performs
# globally, applied per material instead of uniformly.
WALL_TARGET_SPREAD = 0.36  # ~2.7 PAL3 rungs
WALL_MAX_CONTRAST_GAIN = 3.0

# E1M2's three broad stone panels quantize mostly to the global floor rung.
# Retain that best match where possible, but sparsely move the least-cost
# texels to their next-best rung so the wall never becomes a flat solid field.
FLAT_AVOIDING_NEUTRAL_MATERIALS = ("STONE2", "STONE3", "SW1STON1")
FLAT_AVOIDING_MAX_SHARE = 0.49

# Spatial low-pass applied after the contrast expansion, before quantizing.
# The expansion above deliberately amplifies luminance detail, and at 16 colours
# any residual pixel-level ripple lands on alternating palette rungs: measured
# as "churn" (share of horizontally adjacent texels with different indices) the
# worst materials sat at 56-71% -- essentially salt-and-pepper. RAY_COL_STRIDE 2
# then resamples that noise per column at a distance-dependent rate, which is
# what turns it into the crawling vertical streaks visible in motion. A 1-w-1
# separable kernel on the runtime texture torus (matching the wrap the runtime
# sampler uses) removes the ripple while leaving real edges intact.
WALL_SMOOTH_WEIGHT = 2
# PAL3 carries a 5-rung earth ramp -- 1 #240000, 4 #482424, 6 #6D4824,
# 8 #916D48, 12 #B6916D -- as evenly spread in Oklab L (0.163..0.682) as the
# neutral ramp, plus 0 for genuinely black detail. Doom's brown materials were
# not reaching it: world_palette.is_warm gates on absolute RGB deltas (r>=g+18,
# r-b>=24), and after WALL_TONE_GAMMA lifts and desaturates them, BROWN96,
# BROWN144 and BRNBIGL/R fall *below* those thresholds -- only 11-15% of their
# texels still register as warm. allowed_indices then stopped restricting them
# at all, the full neutral ramp competed, and a brown wall quantized to the same
# grey as the floor. That is the "wall is almost the floor colour" complaint.
# Classifying the material by Oklab hue instead of per-pixel RGB deltas is what
# fixes it, and it also guarantees the separation structurally: an earth
# material can no longer emit GLOBAL_FLOOR_INDEX at all.
# 11 (#FF4848) is not part of the ramp's luminance ladder; it is carried so
# SW1STRTN keeps the red indicator that marks it as an operable switch. Its
# chroma (0.219) is more than triple the ramp's, so ordinary brown texels never
# select it -- only genuinely saturated red ones do.
EARTH_RAMP_INDICES = (0, 1, 4, 6, 8, 11, 12)
EARTH_HUE_RANGE = (50.0, 105.0)
EARTH_MIN_CHROMA = 0.030
# Share of texels that must sit inside the hue window for the material to be
# treated as earth. Measured after smoothing (i.e. on exactly what gets
# quantized), E1M1 splits across a wide gap: the brown corridor/panel materials
# score 48-100%, then STARTAN3 at 29%, then nothing until EXITDOOR at 17%.
# 0.35 deliberately leaves STARTAN3 out. Pulling it in for family consistency
# with STARTAN1 was tried and rejected on measurement: only 29% of its texels
# are earth-hued, and confining the other 71% to the earth ramp doubled its
# spatial_palette_error (1.10 against a 0.57 legacy baseline). Multi-material
# decorations stay out for the same reason -- forcing EXITDOOR onto the ramp
# repaints its grey metalwork brown, which is worse than the problem solved.
EARTH_MIN_SHARE = 0.35
# A near-solid wall material must stay clear of each flat colour on at least ONE
# of two axes: lightness, or colour (Oklab a/b). Failing both at once is what
# makes a wall stop reading as a wall and merge into the floor or ceiling.
# 0.09 in L is just under one PAL3 neutral rung (~0.13), so adjacent grey rungs
# still count as distinguishable; 0.04 in a/b is comfortably below PAL3's earth
# ramp chroma (~0.069), so a brown wall clears a grey flat on colour alone.
FLAT_WALL_MIN_LIGHTNESS = 0.09
FLAT_WALL_MIN_COLOUR = 0.04
# A wall material counts as "near-solid" -- and therefore able to disappear into
# a flat -- once one index covers this much of its texels.
WALL_SOLID_SHARE = 0.5
# The level-wide ceiling and floor. Both flats are now single global colours
# (see emit_world_assets): per-sector flats multiplied each material by its
# sector's light level before quantizing, so ONE material emitted up to seven
# different colours (FLOOR5_2) and crossing a doorway between two rooms with the
# same floor repainted the whole viewport. That reads as a palette glitch, not
# as lighting -- especially since walls receive no light at all. Chosen by
# maximizing the minimum Oklab distance to every near-solid wall colour and to
# each other, jointly with the shade chain (the chosen pair is excluded from
# that chain, so it changes which indices a shaded wall can go solid on -- the
# two choices have to be solved together, see certify_flat_wall_contrast).
# 0.127 is the best separation PAL3 can offer; a warm tan floor scored better on
# fidelity but sits only 0.076 from index 7, which many materials go solid on
# once shaded. Dark ceiling over a lighter floor rather than the reverse: floors
# carry the sprites and need to stay readable, ceilings should recede.
GLOBAL_CEILING_INDEX = 3
GLOBAL_FLOOR_INDEX = 7
# Distance-fog steps baked into FREEDOOM_WALL_PACKED_PAIRS. Emitted as
# FREEDOOM_WORLD_SHADE_LEVELS and used to size the shade chain, so the
# renderer can never index a plane the bake did not generate.
WORLD_SHADE_LEVELS = 4


def _has_short_period_vertical_banding(columns):
    """Detect a strong period-4-or-shorter vertical alternation, e.g. LITE3's
    pure-white band repeating every 4 texels. MEGALDOOM_WALL_TEX_Y_BY_HEIGHT's
    point-sampled vertical resampler turns that into a hard-edged grid on any
    wall short enough to skip rows (see the D4 grid artifact in the wall-
    quality plan). Row-averaged so per-column noise cannot trigger it, and
    gated tight enough that ordinary detailed materials (METAL1's slow panel
    gradient, STARTAN1's rivets) do not qualify -- this must stay a narrow
    fix, not a blanket blur, or it re-softens genuinely sharp texture detail.
    """
    dim = len(columns[0])
    row_luma = [sum(columns[x][y][c] for x in range(len(columns)) for c in range(3))
               for y in range(dim)]
    step1 = sum(abs(row_luma[y] - row_luma[y - 1]) for y in range(dim)) / dim
    step4 = sum(abs(row_luma[y] - row_luma[y - 4]) for y in range(dim)) / dim
    return step1 > 400 and step4 < step1 * 0.2


def _contrast_normalize(columns):
    """Expand a tone-curved texture's luminance spread around its own median.

    See WALL_TARGET_SPREAD. Materials that already span a wide range (COMPUTE2,
    BIGDOOR2, LITE3, DOORTRAK) resolve to gain 1.0 and pass through untouched --
    their single-index dominance comes from a genuinely uniform area in the
    source material, which must be preserved, not manufactured away.
    """
    width = len(columns)
    height = len(columns[0])
    luminance = sorted(world_palette.oklab(columns[x][y])[0]
                       for x in range(width) for y in range(height))
    quantile = lambda fraction: luminance[int(fraction * (len(luminance) - 1))]
    low, high, median = quantile(0.02), quantile(0.98), quantile(0.5)
    gain = max(1.0, min(WALL_MAX_CONTRAST_GAIN,
                        WALL_TARGET_SPREAD / max(1e-6, high - low)))
    if gain <= 1.0:
        return columns
    result = [[None] * height for _ in range(width)]
    for x in range(width):
        for y in range(height):
            color = columns[x][y]
            value = world_palette.oklab(color)[0]
            if value <= 1e-6:
                result[x][y] = color
                continue
            # Oklab L is the cube root of a linear-light mix, and world_palette
            # linearizes with sRGB's 2.4 gamma, so display-space channels scale
            # L by f**(2.4/3) = f**0.8. Multiplying L by `ratio` therefore needs
            # f = ratio**1.25. Applying f to all three channels equally keeps
            # their ratio -- i.e. the hue -- and keeps r==g==b materials exactly
            # neutral.
            # Clamped at 0: expanding around the median drives luminance
            # negative for texels far enough below it, and a negative base
            # under ** 1.25 silently returns a COMPLEX number in Python, which
            # then dies in round() several frames of stack later. Only
            # WALL_TONE_GAMMA's lift keeps the expression positive today, so
            # any darker tone curve trips it.
            target = max(0.0, median + (value - median) * gain)
            if target <= 0.0:
                result[x][y] = (0, 0, 0)
                continue
            scale = (target / value) ** 1.25
            result[x][y] = tuple(max(0, min(255, int(round(channel * scale))))
                                 for channel in color)
    return result


def _spatial_smooth(columns, weight=WALL_SMOOTH_WEIGHT):
    """Separable 1-weight-1 blur, wrapping like the runtime sampler. See
    WALL_SMOOTH_WEIGHT."""
    width = len(columns)
    height = len(columns[0])
    total = weight + 2
    horizontal = [[tuple((columns[(x - 1) % width][y][c] + weight * columns[x][y][c] +
                          columns[(x + 1) % width][y][c]) // total for c in range(3))
                   for y in range(height)] for x in range(width)]
    return [[tuple((horizontal[x][(y - 1) % height][c] + weight * horizontal[x][y][c] +
                    horizontal[x][(y + 1) % height][c]) // total for c in range(3))
             for y in range(height)] for x in range(width)]


def _edge_mask(columns, lightness_threshold, colour_threshold):
    """Mark meaningful panel/monitor boundaries on the runtime texture torus."""
    width = len(columns)
    height = len(columns[0])
    labs = [[world_palette.oklab(columns[x][y]) for y in range(height)]
            for x in range(width)]
    result = [[False] * height for _ in range(width)]
    for x in range(width):
        for y in range(height):
            here = labs[x][y]
            for nx, ny in (((x - 1) % width, y), ((x + 1) % width, y),
                           (x, (y - 1) % height), (x, (y + 1) % height)):
                other = labs[nx][ny]
                colour_delta = ((here[1] - other[1]) ** 2 +
                                (here[2] - other[2]) ** 2) ** 0.5
                if (abs(here[0] - other[0]) >= lightness_threshold or
                        colour_delta >= colour_threshold):
                    result[x][y] = True
                    break
    return result


def _edge_aware_spatial_smooth(columns, edge_mask, weight):
    """Apply a separable 1-weight-1 filter without bleeding across edges."""
    if weight <= 0:
        raise ValueError("Wall bake smooth_weight must be positive")
    width = len(columns)
    height = len(columns[0])
    total = weight + 2

    def tap(grid, x, y, nx, ny):
        return grid[x][y] if edge_mask[nx][ny] else grid[nx][ny]

    horizontal = [[None] * height for _ in range(width)]
    for x in range(width):
        for y in range(height):
            if edge_mask[x][y]:
                horizontal[x][y] = columns[x][y]
                continue
            left = tap(columns, x, y, (x - 1) % width, y)
            right = tap(columns, x, y, (x + 1) % width, y)
            horizontal[x][y] = tuple(
                (left[c] + weight * columns[x][y][c] + right[c]) // total
                for c in range(3))

    result = [[None] * height for _ in range(width)]
    for x in range(width):
        for y in range(height):
            if edge_mask[x][y]:
                result[x][y] = columns[x][y]
                continue
            above = tap(horizontal, x, y, x, (y - 1) % height)
            below = tap(horizontal, x, y, x, (y + 1) % height)
            result[x][y] = tuple(
                (above[c] + weight * horizontal[x][y][c] + below[c]) // total
                for c in range(3))
    return result


def _cleanup_isolated_indices(rows, edge_mask):
    """Replace one isolated index only when three non-edge neighbours agree."""
    width = len(rows[0])
    height = len(rows)
    proposals = {}
    for y in range(height):
        for x in range(width):
            neighbours = (
                ((x - 1) % width, y), ((x + 1) % width, y),
                (x, (y - 1) % height), (x, (y + 1) % height),
            )
            all_values = [rows[ny][nx] for nx, ny in neighbours]
            # Four agreeing neighbours prove a one-pixel island even when its
            # contrast made it enter the raw edge mask. Removing that dot does
            # not cross a boundary; it restores the surrounding region.
            if len(set(all_values)) == 1 and all_values[0] != rows[y][x]:
                proposals[(x, y)] = all_values[0]
                continue
            if edge_mask[x][y]:
                continue
            counts = Counter(
                rows[ny][nx] for nx, ny in neighbours
                if not edge_mask[nx][ny])
            if not counts:
                continue
            replacement, hits = counts.most_common(1)[0]
            if hits >= 3 and replacement != rows[y][x]:
                proposals[(x, y)] = replacement

    # Simultaneous majority replacements can otherwise create a new island:
    # two neighbours may both leave a two-pixel feature even though each local
    # proposal looked safe in isolation. Cancel every proposal participating in
    # such a conflict. This only removes proposals, so the loop terminates and
    # cannot introduce an island that was absent before the cleanup stage.
    active = set(proposals)

    def value_at(x, y):
        return proposals[(x, y)] if (x, y) in active else rows[y][x]

    while True:
        cancel = set()
        for y in range(height):
            for x in range(width):
                original = rows[y][x]
                neighbours = (
                    ((x - 1) % width, y), ((x + 1) % width, y),
                    (x, (y - 1) % height), (x, (y + 1) % height),
                )
                was_isolated = all(rows[ny][nx] != original
                                   for nx, ny in neighbours)
                current = value_at(x, y)
                is_isolated = all(value_at(nx, ny) != current
                                  for nx, ny in neighbours)
                if not was_isolated and is_isolated:
                    cancel.update(point for point in ((x, y),) + neighbours
                                  if point in active)
        if not cancel:
            break
        active.difference_update(cancel)

    cleaned = [list(row) for row in rows]
    for (x, y) in active:
        cleaned[y][x] = proposals[(x, y)]
    return cleaned


def _compose_compute2_facade_indices(source):
    """Reduce COMPUTE2 to a readable two-dimensional computer-bank facade.

    The original texture packs several narrow devices into 256x56 pixels.  At
    160x120 with stride-2 sampling, preserving every lamp is worse than losing
    detail: the panel body survives but its structure becomes coloured noise.
    Keep the selected Doom module's actual readouts, then place them inside a
    small set of continuous bays and rails.  All indices are existing PAL3
    matches from that source; this changes no runtime format or palette.
    """
    dim = COMPUTE2_FACADE_DIM
    if len(source) != dim or len(source[0]) != dim:
        raise ValueError("COMPUTE2 facade expects a %dx%d authoring grid" %
                         (dim, dim))
    panel = 10      # neutral metal panel body
    shadow = 5      # structural rails / screen surround
    black = 0       # deepest screen cavity
    highlight = 13  # metal lip
    result = [[panel] * dim for _ in range(dim)]

    def fill(x0, y0, x1, y1, value):
        for y in range(y0, y1):
            result[y][x0:x1] = [value] * (x1 - x0)

    # Four strong horizontal masses survive both distance mip-like skipping
    # and the oblique projection.  The source's previous single-pixel rails
    # disappeared precisely in the rejected corridor view.
    for rail_y0, rail_y1 in COMPUTE2_RAIL_ROWS:
        fill(0, rail_y0, dim, rail_y1, shadow)

    # Two large upper instrument windows and two waveform monitors.  Dark
    # recesses give the green/red source pixels a coherent object to belong to
    # instead of leaving them floating on a grey plane.
    for x0, x1 in COMPUTE2_INSTRUMENT_COLUMNS:
        fill(x0, 4, x1, 14, shadow)
        fill(x0 + 2, 6, x1 - 2, 12, black)
        fill(x0, 21, x1, 32, shadow)
        fill(x0 + 2, 23, x1 - 2, 30, black)
        result[14][x0:x1] = [highlight] * (x1 - x0)
        result[32][x0:x1] = [highlight] * (x1 - x0)

    # Retain coloured readouts from the selected Doom module, but only inside
    # their corresponding devices.  Neutral source pixels cannot punch holes
    # back through the newly continuous surrounds.
    accents = COMPUTE2_ACCENT_INDICES
    for y0, y1 in COMPUTE2_READOUT_BAYS[:2]:
        for y in range(y0, y1):
            for x in range(5, 59):
                if source[y][x] in accents:
                    result[y][x] = source[y][x]

    # A broad lower control bay replaces scattered mid-wall pixels.  Its
    # central divider and bottom cabinet doors are copied/simplified from the
    # same source module.
    for x0, x1 in COMPUTE2_INSTRUMENT_COLUMNS:
        fill(x0, 40, x1, 49, shadow)
        fill(x0 + 2, COMPUTE2_READOUT_BAYS[2][0], x1 - 2, COMPUTE2_READOUT_BAYS[2][1], black)
    for y in range(*COMPUTE2_READOUT_BAYS[2]):
        for x in range(5, 59):
            if source[y][x] in accents or source[y][x] == highlight:
                result[y][x] = source[y][x]

    fill(3, 56, 61, 62, shadow)
    for divider in (3, 17, 31, 32, 46, 60):
        fill(divider, 56, min(64, divider + 2), 62, black)
    # Warm cabinet handles, one per door; large enough to remain stable but
    # deliberately sparse.
    for x in (10, 24, 39, 53):
        result[58][x:x + 3] = [6, 8, 6]

    # Vertical stiles tie every horizontal band into one facade.  Highlights
    # sit on their inner edge so the wall reads as panelled even under shade.
    for x in (0, 31, 32, 63):
        fill(x, 0, x + 1, 64, shadow)
    for x in (2, 33, 61):
        for y in range(3, 62):
            if result[y][x] == panel:
                result[y][x] = highlight
    return result


def _resize_index_rows_x(rows, width):
    """Nearest-neighbour resize of an indexed [y][x] grid in U only.

    The U counterpart of _resize_index_rows; widening and narrowing are the
    same nearest-neighbour walk, so they are one function.
    """
    if not rows or width <= 0:
        raise ValueError("Cannot resize an empty indexed texture")
    source_width = len(rows[0])
    return [[row[(x * source_width) // width] for x in range(width)]
            for row in rows]


def material_name_for(path):
    """The uppercase material name a texture path stands for."""
    return os.path.splitext(os.path.basename(path))[0].upper()


def horizontal_churn(rows, active_height):
    """Share of horizontally adjacent texels that change palette index.

    Measured on the grid as displayed -- one texel per screen pixel -- because
    that is the sampling the eye actually sees, and only over the rows the
    runtime's V scale can reach.
    """
    width = len(rows[0])
    if width < 2 or active_height < 1:
        return 0.0
    changed = sum(1 for row in rows[:active_height]
                  for x in range(width - 1) if row[x] != row[x + 1])
    return changed / (active_height * (width - 1))


def pair_column_texels(row):
    """The one texel per PAIR column that FREEDOOM_WALL_TEXTURES stores.

    The packed table carries both texels of every pair; this table carries the
    even one, because its only reader is the door overlay recompositor, which
    indexes by tex_x and restyles most of what it samples. Named so the rule
    lives here rather than being restated wherever the table is checked.
    """
    return [row[2 * pair] for pair in range(WALL_TEX_WIDTH)]


def packed_pair_byte(level, left_index, right_index):
    """One FREEDOOM_WALL_PACKED_PAIRS byte: the two pixels of a stride-2 sample.

    The hotpath writes this byte straight into a 4bpp tile row, so the high
    nibble lands on the even screen pixel and the low nibble on the odd one.
    Carrying two adjacent display texels here rather than one index twice is
    what doubles the wall's horizontal resolution for zero runtime cost.
    """
    return ((level[left_index & 0x0F] & 0x0F) << 4) | (level[right_index & 0x0F] & 0x0F)


def _resize_columns_x(columns, width):
    """Nearest-neighbour resize of an indexed/RGB [x][y] grid in U only."""
    if not columns or width <= 0:
        raise ValueError("Cannot resize an empty column grid")
    source_width = len(columns)
    return [list(columns[(x * source_width) // width]) for x in range(width)]


def _resize_index_rows(rows, height):
    """Nearest-neighbour resize of an indexed [y][x] grid in V only."""
    if not rows or height <= 0:
        raise ValueError("Cannot resize an empty indexed texture")
    source_height = len(rows)
    return [list(rows[(y * source_height) // height]) for y in range(height)]


def _is_earth_material(columns):
    """True when enough texels sit inside PAL3's earth hue window. See
    EARTH_MIN_SHARE -- this is a whole-material decision, not per pixel, so a
    material keeps one coherent ramp instead of alternating between the brown
    and grey ones from texel to texel."""
    width = len(columns)
    height = len(columns[0])
    inside = sum(1 for x in range(width) for y in range(height)
                 if world_palette.chroma(columns[x][y]) >= EARTH_MIN_CHROMA
                 and EARTH_HUE_RANGE[0] <= world_palette.hue(columns[x][y])
                 <= EARTH_HUE_RANGE[1])
    return inside >= EARTH_MIN_SHARE * width * height


def convert_texture(path, palette, use_wall_bake_recipe=True,
                    diagnostics=None):
    """Bake at display resolution, but only where the material can carry it.

    Doubling the horizontal texel count is free at runtime, and for most
    materials it lowers churn as well as raising detail: the 2:1 box average it
    replaces was blurring real structure. A few high-frequency technological
    walls are the exception -- at 1:1 they exceed WALL_CHURN_LIMIT, which is the
    metric that predicted the previous round of "ugly walls" and of shimmer in
    motion. Those fall back to the pair-averaged representation they have today,
    so this change can improve a wall but never make one noisier.

    The decision is measured per material rather than listed by name, so a WAD
    or recipe change cannot leave a stale exemption behind.
    """
    rows = _convert_texture(path, palette, use_wall_bake_recipe, diagnostics)
    if len(rows[0]) != WALL_TEX_DISPLAY_WIDTH:
        return rows
    with Image.open(path) as image:
        active_height = min(
            sampled_texture_dimensions(
                material_name_for(path), image.width, image.height)[1],
            WALL_TEX_HEIGHT)
    if horizontal_churn(rows, active_height) <= WALL_CHURN_LIMIT:
        return rows
    return _resize_index_rows_x(
        _convert_texture(path, palette, use_wall_bake_recipe, diagnostics,
                         bake_width_override=WALL_TEX_WIDTH),
        WALL_TEX_DISPLAY_WIDTH)


def _convert_texture(path, palette, use_wall_bake_recipe=True,
                     diagnostics=None, bake_width_override=None):
    """Convert one wall source into the fixed runtime index grid.

    ``use_wall_bake_recipe=False`` reproduces the pre-curation conversion while
    retaining source-window selection (notably COMPUTE2). It exists solely for
    deterministic offline A/B previews and quality tests. Runtime generation
    always uses the default curated path.
    """
    with Image.open(path) as image:
        material_name = material_name_for(path)
        window = CURATED_TEXTURE_WINDOWS.get(material_name)
        if window is not None:
            left, top, sampled_width, sampled_height = window
            sampled_texture_dimensions(material_name, image.width, image.height)
            image = image.crop((left, top, left + sampled_width,
                                top + sampled_height))
        else:
            sampled = sampled_texture_width(image.width)
            if sampled != image.width:
                image = image.crop((0, 0, sampled, image.height))
        opaque = _fill_transparent_with_average(image)
        recipe = WALL_BAKE_RECIPES.get(material_name) if use_wall_bake_recipe else None
        if recipe is not None and recipe.facade_window is not None:
            left, top, width, height = recipe.facade_window
            if (left < 0 or top < 0 or width <= 0 or height <= 0 or
                    left + width > opaque.width or top + height > opaque.height):
                raise ValueError("Invalid %s facade window %s for %dx%d source" %
                                 (material_name, recipe.facade_window,
                                  opaque.width, opaque.height))
            opaque = opaque.crop((left, top, left + width, top + height))
        bake_height = min(opaque.height, WALL_TEX_HEIGHT)
        # A curated facade is hand-authored on the square COMPUTE2_FACADE_DIM
        # lattice and deliberately throws horizontal detail away, so it bakes
        # there and is widened once at the end. Everything else bakes at full
        # display resolution, which is where the 2:1 horizontal squash goes.
        bake_width = (COMPUTE2_FACADE_DIM
                      if (recipe is not None and recipe.facade_window is not None)
                      else (bake_width_override or WALL_TEX_DISPLAY_WIDTH))
        resized = opaque.resize((bake_width, bake_height), Image.Resampling.BOX)
        columns = [[resized.getpixel((x, y)) for y in range(bake_height)]
                  for x in range(bake_width)]
        if _has_short_period_vertical_banding(columns):
            # 4-tap vertical box average: wide enough to flatten a period-4
            # alternation (unlike a narrow 1-2-1 blur, which softens the edges
            # but leaves the alternation itself intact and still aliasable).
            smoothed = [[None] * bake_height for _ in range(bake_width)]
            for x in range(bake_width):
                column = columns[x]
                for y in range(bake_height):
                    taps = [column[(y + offset) % bake_height]
                            for offset in (-1, 0, 1, 2)]
                    smoothed[x][y] = tone_curve(tuple(
                        sum(tap[c] for tap in taps) // 4 for c in range(3)))
        else:
            smoothed = [[tone_curve(columns[x][y]) for y in range(bake_height)]
                       for x in range(bake_width)]
        normalized = _contrast_normalize(smoothed)
        if recipe is None:
            smoothed = _spatial_smooth(normalized)
            edge_mask = [[False] * bake_height for _ in range(bake_width)]
        else:
            # Detect structure after the established low-pass, otherwise every
            # one-texel rivet/readout becomes an "edge" and defeats the point
            # of simplifying detail that stride 2 cannot reconstruct.
            structural = _spatial_smooth(normalized)
            edge_mask = _edge_mask(
                structural, recipe.edge_lightness_threshold,
                recipe.edge_colour_threshold)
            smoothed = _edge_aware_spatial_smooth(
                structural, edge_mask, recipe.smooth_weight)
        is_neutral_material = material_name.startswith(("GRAY", "METAL", "STONE"))
        chroma_threshold = WALL_CHROMA_THRESHOLD
        if is_neutral_material:
            allowed = [index for index in range(0, WORLD_COLOR_DAMAGE)
                       if world_palette.is_neutral(palette[index])]
            # Sprite-tuned dither defaults kept for this bucket: smooth grey
            # gradients are the one case where dithering blends rather than
            # speckles. In practice it rarely fires anyway -- best_mix takes its
            # low-chroma early return on exactly-neutral texels -- so the noise
            # here was source detail, which WALL_SMOOTH_WEIGHT handles (GRAY7
            # churn 37% -> 18% with the same three structural indices).
            pair_max_delta, gain_ratio = 73, 0.65
        else:
            if _is_earth_material(smoothed):
                allowed = list(EARTH_RAMP_INDICES)
                # Disable allowed_indices' low-chroma neutral clamp for this
                # bucket. It would intersect the neutral set with the earth
                # ramp and leave only index 0, painting every desaturated texel
                # of a brown material black. The clamp exists to keep near-grey
                # pixels off the coloured ramps -- but here the material has
                # already been classified as earth as a whole, which is the
                # decision the clamp would otherwise second-guess per texel.
                chroma_threshold = -1.0
            else:
                allowed = range(0, WORLD_COLOR_DAMAGE)
            # gain_ratio 0 disables dithering outright for textured walls. The
            # Bayer pattern is defined in texture space but the runtime samples
            # each column at a distance-dependent rate through
            # MEGALDOOM_WALL_TEX_Y_BY_HEIGHT, so the pattern never survives
            # resampling as a blend -- it arrives as noise that crawls when the
            # camera moves. Measured over all 23 E1M1 materials, dropping it
            # lowers mean churn from 37% to 31% and *improves* per-texel
            # accuracy, because the averaging the dither pays for is never
            # actually reconstructed on screen.
            pair_max_delta, gain_ratio = WALL_PAIR_MAX_DELTA, 0.0
        cache = {}
        rows = [[world_palette.dither_index(smoothed[x][y], palette,
                                            x, y, False, cache,
                                            allowed, chroma_threshold,
                                            pair_max_delta, gain_ratio,
                                            WALL_CHROMA_LOSS_WEIGHT)
                 for x in range(bake_width)] for y in range(bake_height)]
        if material_name in FLAT_AVOIDING_NEUTRAL_MATERIALS:
            floor_texels = sum(value == GLOBAL_FLOOR_INDEX
                               for row in rows for value in row)
            maximum = int(bake_width * bake_height *
                          FLAT_AVOIDING_MAX_SHARE)
            if floor_texels > maximum:
                alternatives = [index for index in allowed
                                if index != GLOBAL_FLOOR_INDEX]
                candidates = []
                for y in range(bake_height):
                    for x in range(bake_width):
                        if rows[y][x] != GLOBAL_FLOOR_INDEX:
                            continue
                        replacement = nearest_palette_index(
                            smoothed[x][y], palette, alternatives)
                        penalty = (world_palette.distance_sq(
                            smoothed[x][y], palette[replacement]) -
                            world_palette.distance_sq(
                                smoothed[x][y], palette[GLOBAL_FLOOR_INDEX]))
                        candidates.append((penalty, y, x, replacement))
                candidates.sort()
                for _, y, x, replacement in candidates[:floor_texels - maximum]:
                    rows[y][x] = replacement
        if recipe is not None and recipe.facade_window is not None:
            rows = _compose_compute2_facade_indices(
                _resize_index_rows(rows, COMPUTE2_FACADE_DIM))
            rows = _resize_index_rows(rows, bake_height)
            # The semantic facade is the candidate target for preview/error
            # accounting.  Its boundaries, not the discarded micro-detail's
            # boundaries, are what edge retention must certify.
            smoothed = [[palette[rows[y][x]] for y in range(bake_height)]
                        for x in range(bake_width)]
            edge_mask = _edge_mask(
                smoothed, recipe.edge_lightness_threshold,
                recipe.edge_colour_threshold)
        if recipe is not None and recipe.cleanup_isolated:
            rows = _cleanup_isolated_indices(rows, edge_mask)
            if recipe.facade_window is not None:
                # A facade module is deliberately magnified so a few original
                # one-pixel LEDs would otherwise become conspicuous square
                # sparks.  A second majority pass may cross the raw detector's
                # micro-edges, but cannot cross a panel boundary because those
                # boundaries are multi-pixel runs.  This is the semantic
                # simplification the facade needs: rails and monitors survive;
                # isolated lamps do not.
                rows = _cleanup_isolated_indices(
                    rows, [[False] * bake_height for _ in range(bake_width)])
        # A facade baked on its authoring lattice is widened onto the display
        # grid here, with every derived grid, so nothing below this point has
        # to know which lattice the material came from.
        target_width = bake_width_override or WALL_TEX_DISPLAY_WIDTH
        if bake_width != target_width:
            rows = _resize_index_rows_x(rows, target_width)
            normalized = _resize_columns_x(normalized, target_width)
            smoothed = _resize_columns_x(smoothed, target_width)
            edge_mask = _resize_columns_x(edge_mask, target_width)
            bake_width = target_width

        # Keep short source textures native. The unused tail is padding only;
        # the runtime's per-texture V scale guarantees that it is never sampled.
        if bake_height < WALL_TEX_HEIGHT:
            rows.extend(rows[y % bake_height][:]
                        for y in range(bake_height, WALL_TEX_HEIGHT))
        if diagnostics is not None:
            def pad_columns(grid):
                return [[grid[x][y % bake_height]
                         for y in range(WALL_TEX_HEIGHT)]
                        for x in range(bake_width)]
            def pad_mask(grid):
                return [[grid[x][y % bake_height]
                         for y in range(WALL_TEX_HEIGHT)]
                        for x in range(bake_width)]
            diagnostics.update(
                material_name=material_name,
                source_window=window,
                edge_mask=pad_mask(edge_mask),
                normalized_columns=pad_columns(normalized),
                filtered_columns=pad_columns(smoothed),
            )
    return rows


def solid_wall_colors(textures, shade_lut):
    """Indices that a wall material can present as an unbroken field of colour.

    A textured wall with real spread never reads as a solid block, so only
    materials where one index covers WALL_SOLID_SHARE of the texels can be
    confused with a flat. Every shade level is examined, not just level 0:
    darkening collapses distinct indices together, so a material that is varied
    up close can still turn solid in the distance.

    `textures` maps name -> the [y][x] index grid convert_texture() returns.
    Returns {index: [(name, level, share), ...]}.
    """
    result = {}
    for name, rows in sorted(textures.items()):
        texels = [value for row in rows for value in row]
        for level, shade in enumerate(shade_lut):
            counts = Counter(shade[value & 0x0F] for value in texels)
            index, hits = counts.most_common(1)[0]
            share = hits / len(texels)
            if share >= WALL_SOLID_SHARE:
                result.setdefault(index, []).append((name, level, share))
    return result


def certify_flat_wall_contrast(palette, textures, shade_lut, ceiling, floor):
    """Fail the bake if a wall could read as the flat colour of one of the two.

    This is the contract behind the whole global-flat change: a near-solid wall
    that lands on (or perceptually next to) the ceiling or floor colour stops
    being a wall and becomes a continuation of the flat. Enforced perceptually
    in Oklab rather than as index inequality, because adjacent PAL3 rungs are
    only ~0.13 apart and index inequality alone let BROWNGRN (75% of its texels
    on one index) sit right beside the floor.

    Two axes, not one distance. A single Oklab distance is dominated by the
    lightness term (distance_sq weights L at 1.25) and therefore rejects pairs
    that are obviously distinguishable: brown #916D48 against grey #6D6D6D
    scores 0.076, below any useful single threshold, yet nobody looking at the
    screen confuses a brown wall with a grey floor. What actually makes a wall
    disappear into a flat is being close in lightness AND in colour at the same
    time, so a violation requires both. Verified against the pre-global-flat
    bake, where it still catches the original defect (see test-wall-quality.py).
    """
    solid = solid_wall_colors(textures, shade_lut)
    violations = []
    for flat_name, flat_index in (("ceiling", ceiling), ("floor", floor)):
        flat_lab = world_palette.oklab(tuple(palette[flat_index]))
        for index, sources in sorted(solid.items()):
            lab = world_palette.oklab(tuple(palette[index]))
            lightness = abs(flat_lab[0] - lab[0])
            colour = ((flat_lab[1] - lab[1]) ** 2 + (flat_lab[2] - lab[2]) ** 2) ** 0.5
            if lightness < FLAT_WALL_MIN_LIGHTNESS and colour < FLAT_WALL_MIN_COLOUR:
                violations.append(
                    "%s index %d (#%02X%02X%02X) vs wall index %d "
                    "(#%02X%02X%02X): dL=%.3f dC=%.3f, used solid by %s" % (
                        flat_name, flat_index, palette[flat_index][0],
                        palette[flat_index][1], palette[flat_index][2],
                        index, palette[index][0], palette[index][1],
                        palette[index][2], lightness, colour,
                        ", ".join("%s@L%d %.0f%%" % (n, l, s * 100)
                                  for n, l, s in sources)))
    if violations:
        raise RuntimeError(
            "Wall material indistinguishable from a flat (needs dL >= %.2f or "
            "dC >= %.2f) -- pick a different GLOBAL_CEILING_INDEX/"
            "GLOBAL_FLOOR_INDEX in world_assets.py:\n  %s"
            % (FLAT_WALL_MIN_LIGHTNESS, FLAT_WALL_MIN_COLOUR,
               "\n  ".join(violations)))
    return solid


def texture_u_scale_q12(size):
    """Map Doom's source-width repeat into the WALL_TEX_WIDTH runtime texture.

    Doom textures are not restricted to power-of-two widths (E1M2 uses a
    24-pixel-wide material).  A fixed-point scale keeps their world-space
    repeat period without requiring a division in the renderer's column loop.
    Rounding upward makes exact source-width boundaries wrap deterministically.
    """
    if size <= 0:
        raise ValueError("Invalid texture width %d" % size)
    return (WALL_TEX_WIDTH * 4096 + size - 1) // size


def texture_v_scale_q12(size):
    """Map a source-native V coordinate into the 128-row runtime lattice."""
    if size <= 0 or size > WALL_TEX_HEIGHT:
        raise ValueError("Invalid texture height %d" % size)
    return (size * 4096) // WALL_TEX_HEIGHT


def emit_world_assets(path, texture_usage, sectors, provenance=None,
                      door_texture_names=None):
    texture_names = [FALLBACK_TEXTURE] + sorted(
        name for name in texture_usage if name != FALLBACK_TEXTURE)
    for name in texture_names:
        if not os.path.isfile(texture_path(name)):
            raise FileNotFoundError("Wall texture source not found: %s" % texture_path(name))

    # PAL3 is a frozen cross-asset ABI. Adding a map texture must never renumber
    # the floor, weapon or billboard colours (see FROZEN_WORLD_PALETTE).
    palette = build_world_palette(texture_names, texture_usage, sectors)
    if len(palette) != 16:
        raise RuntimeError("World palette must contain exactly 16 colors")
    texture_ids = {name: index for index, name in enumerate(texture_names)}
    if door_texture_names is None:
        # Keep the standalone converter conservative. The map extraction CLI
        # passes the exact door-face catalog; callers that do not have map
        # geometry still get a correct (if non-sparse) asset table.
        door_texture_names = set(texture_names)
    else:
        door_texture_names = set(door_texture_names)
    unknown_doors = door_texture_names.difference(texture_ids)
    if unknown_doors:
        raise ValueError("Door texture is absent from the wall catalog: %s" %
                         ", ".join(sorted(unknown_doors)))
    door_texture_names = [name for name in texture_names
                          if name in door_texture_names]
    door_texture_indices = {
        texture_ids[name]: index for index, name in enumerate(door_texture_names)
    }
    texture_meta = {}
    converted = []
    for name in texture_names:
        source = texture_path(name)
        with Image.open(source) as image:
            width, height = image.size
        # The stored grid only holds the selected source window, so the
        # world repeat must be derived from that, not from the source width, or
        # the material would be stretched instead of repeated.
        width, height = sampled_texture_dimensions(name, width, height)
        texture_meta[name] = dict(
            width=width,
            height=height,
            u_scale_q12=texture_u_scale_q12(width),
            v_scale_q12=texture_v_scale_q12(height),
        )
        converted.append(convert_texture(source, palette))

    ceiling = (GLOBAL_CEILING_INDEX, GLOBAL_CEILING_INDEX, 0)
    floor = (GLOBAL_FLOOR_INDEX, GLOBAL_FLOOR_INDEX, 0)
    # One ceiling and one floor for the whole level, emitted identically for
    # every sector. The per-sector table shape is kept so no runtime code or
    # test changes, at a cost of six bytes per sector; the win is that
    # renderer_pack.c's flat_changed full-column invalidation, which used to
    # fire on every sector crossing, now only fires on the first frame.
    # Coverage is 0 on both: renderer_flats.c's 4x4 Bayer is anchored to screen
    # space (x is always the tile-local 0..7, y is y&3), so it never moves with
    # the camera. On a level-wide flat -- the largest continuous surface in the
    # frame -- that reads as fixed dirt on the monitor rather than as texture.
    sector_visuals = [ceiling + floor for _ in sectors]

    # The flats are chosen before the shade chain so distant walls cannot darken
    # into them; see build_shade_map's `reserved`.
    shade_map = build_shade_map(palette, (GLOBAL_CEILING_INDEX, GLOBAL_FLOOR_INDEX))
    shade_lut = build_shade_lut(palette, WORLD_SHADE_LEVELS,
                                (GLOBAL_CEILING_INDEX, GLOBAL_FLOOR_INDEX))
    certify_flat_wall_contrast(palette, dict(zip(texture_names, converted)),
                               shade_lut, GLOBAL_CEILING_INDEX, GLOBAL_FLOOR_INDEX)

    lines = [
        "#ifndef MEGALDOOM_GENERATED_ASSETS_H",
        "#define MEGALDOOM_GENERATED_ASSETS_H",
        "",
        "#include <genesis.h>",
        "#include \"raycast.h\"",
        "",
        "// Generated deterministically by tools/wad-map-extract.py.",
    ]
    if provenance is not None:
        lines.extend([
            "// Source SHA-256: %s" % provenance.wad_sha256,
            "// Flat baseline/final: %d/%d SEGs; curated material: %d linedefs / %d SEGs" % (
                provenance.baseline_seg_count, len(provenance.out_segs),
                len(provenance.curated_material_linedefs),
                provenance.curated_material_segs),
            "// Certified: exit SEG %d reachable after %d states" % (
                provenance.certificate["exit_index"],
                provenance.certificate["states"]),
        ])
    lines.extend([
        "// Exact solid-wall texture catalog for E1M1; index 0 is the fallback.",
        "#define FREEDOOM_WALL_TEXTURE_COUNT %d" % len(texture_names),
        "#define FREEDOOM_SECTOR_VISUAL_COUNT %d" % len(sector_visuals),
        "#define MEGALDOOM_WORLD_COLOR_CEILING %d" % GLOBAL_CEILING_INDEX,
        "#define MEGALDOOM_WORLD_COLOR_FLOOR %d" % GLOBAL_FLOOR_INDEX,
        "#define MEGALDOOM_WORLD_COLOR_DAMAGE %d" % WORLD_COLOR_DAMAGE,
        "#define MEGALDOOM_WORLD_COLOR_WARNING %d" % WORLD_COLOR_WARNING,
    ])
    for name in texture_names:
        lines.append("#define %s %d" % (texture_macro(name), texture_ids[name]))
    lines.extend([
        "",
        "static const u32 FREEDOOM_WORLD_PALETTE[16] = {",
        "    " + ", ".join("0x%02X%02X%02X" % color for color in palette),
        "};",
        "",
        "// One level-wide ceiling and floor, repeated per sector: primary,",
        "// secondary, Bayer coverage. Every row is identical by construction --",
        "// see GLOBAL_CEILING_INDEX in tools/world_assets.py.",
        "static const u8 FREEDOOM_SECTOR_VISUALS[FREEDOOM_SECTOR_VISUAL_COUNT][6] = {",
    ])
    for visual in sector_visuals:
        lines.append("    {%s}," % ", ".join(str(value) for value in visual))
    lines.extend([
        "};",
        "",
        "static const u8 FREEDOOM_WORLD_SHADE_MAP[16] = {",
        "    " + ", ".join(str(value) for value in shade_map),
        "};",
        "",
        "// The shade chain itself is NOT emitted: renderer_pack.c derives it by",
        "// chaining FREEDOOM_WORLD_SHADE_MAP FREEDOOM_WORLD_SHADE_LEVELS times,",
        "// so a second ROM copy could only ever disagree with what is drawn.",
        "#define FREEDOOM_WORLD_SHADE_LEVELS %d" % WORLD_SHADE_LEVELS,
        "",
        "static const u16 FREEDOOM_WALL_TEXTURE_USCALE_Q12[FREEDOOM_WALL_TEXTURE_COUNT] = {",
        "    " + ", ".join(str(texture_meta[name]["u_scale_q12"]) for name in texture_names),
        "};",
        "",
        "static const u16 FREEDOOM_WALL_TEXTURE_WIDTH[FREEDOOM_WALL_TEXTURE_COUNT] = {",
        "    " + ", ".join(str(texture_meta[name]["width"]) for name in texture_names),
        "};",
        "",
        "static const u16 FREEDOOM_WALL_TEXTURE_HEIGHT[FREEDOOM_WALL_TEXTURE_COUNT] = {",
        "    " + ", ".join(str(texture_meta[name]["height"]) for name in texture_names),
        "};",
        "",
        "static const u16 FREEDOOM_WALL_TEXTURE_VSCALE_Q12[FREEDOOM_WALL_TEXTURE_COUNT] = {",
        "    " + ", ".join(str(texture_meta[name]["v_scale_q12"]) for name in texture_names),
        "};",
        "",
        "// One texel per PAIR column, not per displayed pixel: the only reader",
        "// is the door overlay recompositor, which indexes by tex_x and restyles",
        "// most of what it samples. The full-resolution texels live in the",
        "// packed-pair table below.",
        "static const u8 FREEDOOM_WALL_TEXTURES[FREEDOOM_WALL_TEXTURE_COUNT][WALL_TEX_HEIGHT][WALL_TEX_WIDTH] = {",
    ])
    for texture_index, rows in enumerate(converted):
        lines.append("    { // %d: %s (%dx%d)" % (
            texture_index, texture_names[texture_index],
            texture_meta[texture_names[texture_index]]["width"],
            texture_meta[texture_names[texture_index]]["height"]))
        for row in rows:
            lines.append("        {" + ", ".join(
                str(value) for value in pair_column_texels(row)) + "},")
        lines.append("    },")
    lines.extend([
        "};",
        "",
        "// ROM-resident, shade-ready pairs for the shipped stride-2 wall packer.",
        "// The regular table contains every wall texture. Door framing is kept",
        "// in a second compact table because only door faces read it.",
        "// Each u8 carries the two horizontal pixels of one sampled ray as",
        "// independent nibbles (high = even pixel, low = odd), so a wall row",
        "// shows WALL_TEX_DISPLAY_WIDTH distinct texels, not WALL_TEX_WIDTH.",
        "static const u8 FREEDOOM_WALL_PACKED_PAIRS",
        "    [FREEDOOM_WORLD_SHADE_LEVELS][FREEDOOM_WALL_TEXTURE_COUNT]",
        "    [WALL_TEX_WIDTH][WALL_TEX_HEIGHT] = {",
    ])
    for level_index, level in enumerate(shade_lut):
        lines.append("    { // shade %d" % level_index)
        for texture_index, rows in enumerate(converted):
            lines.append("        { // %d: %s" %
                         (texture_index, texture_names[texture_index]))
            for tex_x in range(WALL_TEX_WIDTH):
                source_height = texture_meta[texture_names[texture_index]]["height"]
                v_scale_q12 = texture_meta[texture_names[texture_index]]["v_scale_q12"]
                colors = [packed_pair_byte(level,
                                           rows[(tex_y * v_scale_q12) >> 12][2 * tex_x],
                                           rows[(tex_y * v_scale_q12) >> 12][2 * tex_x + 1])
                          for tex_y in range(WALL_TEX_HEIGHT)]
                lines.append("            {" + ", ".join(
                    "0x%04X" % value for value in colors) + "},")
            lines.append("        },")
        lines.append("    },")
    lines.extend([
        "};",
        "",
        "#define FREEDOOM_WALL_DOOR_TEXTURE_COUNT %d" % len(door_texture_names),
        "static const u8 FREEDOOM_WALL_DOOR_TEXTURE_INDEX[FREEDOOM_WALL_TEXTURE_COUNT] = {",
        "    " + ", ".join(str(door_texture_indices.get(index, 0xFF))
                            for index in range(len(texture_names))),
        "};",
        "",
        "static const u8 FREEDOOM_WALL_DOOR_PACKED_PAIRS",
        "    [FREEDOOM_WORLD_SHADE_LEVELS][FREEDOOM_WALL_DOOR_TEXTURE_COUNT]",
        "    [WALL_TEX_WIDTH][WALL_TEX_HEIGHT] = {",
    ])
    for level_index, level in enumerate(shade_lut):
        lines.append("    { // shade %d" % level_index)
        for door_index, name in enumerate(door_texture_names):
            texture_index = texture_ids[name]
            rows = converted[texture_index]
            lines.append("        { // %d: %s" % (texture_index, name))
            for tex_x in range(WALL_TEX_WIDTH):
                colors = []
                source_height = texture_meta[name]["height"]
                v_scale_q12 = texture_meta[name]["v_scale_q12"]
                for tex_y in range(WALL_TEX_HEIGHT):
                    source_y = (tex_y * v_scale_q12) >> 12
                    left = rows[source_y][2 * tex_x] & 0x0F
                    right = rows[source_y][2 * tex_x + 1] & 0x0F
                    border = WALL_TEX_WIDTH // 16
                    safety = texture_meta[name]["height"] // 8
                    frame_height = texture_meta[name]["height"] // 16
                    # The frame and the moving safety stripe are decided per pair
                    # column, matching style_wall_texel() in renderer_doors.c;
                    # only the door's centre carries independent texels.
                    if (tex_x < border or tex_x >= WALL_TEX_WIDTH - border or
                            source_y < frame_height):
                        left = right = 0
                    elif source_y >= source_height - safety:
                        left = right = (WORLD_COLOR_WARNING
                                        if tex_x & safety else 0)
                    colors.append(packed_pair_byte(level, left, right))
                lines.append("            {" + ", ".join(
                    "0x%04X" % value for value in colors) + "},")
            lines.append("        },")
        lines.append("    },")
    lines.extend(["};", "", "#endif", ""])
    with open(path, "w", newline="\n") as fh:
        fh.write("\n".join(lines))
    return texture_ids, texture_meta, sector_visuals, palette
