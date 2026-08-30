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
import world_palette
# wall_bake_filters owns the runtime texture's dimensions and the generic
# lattice filters (contrast, blur, edge detection, resizing, churn) that never
# need to know about PAL3 or Doom map data. Imported by name, not by module,
# because this file's own code -- and every external caller of world_assets.X
# -- refers to them unqualified; see wall_bake_filters.py's module docstring.
from wall_bake_filters import (
    WALL_TEX_WIDTH, WALL_TEX_HEIGHT, WALL_TEX_DISPLAY_WIDTH,
    WALL_CHURN_LIMIT, WALL_TARGET_SPREAD, WALL_MAX_CONTRAST_GAIN,
    WALL_SMOOTH_WEIGHT,
    _has_short_period_vertical_banding, _contrast_normalize,
    _pair_split, _pair_join, _tap, _blur_axis, _horizontal_smooth,
    _quantize, _smooth, _spatial_smooth, _edge_aware_spatial_smooth,
    _edge_mask, _cleanup_isolated_indices,
    _resize_index_rows_x, _resize_columns_x, _resize_index_rows,
    horizontal_churn, pair_column_churn, pair_column_texels,
    packed_pair_byte,
)

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSET_ROOT = os.path.join(PROJECT_ROOT, "res", "originaldoom")

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
    """Apply WALL_TONE_GAMMA's lift to a texel without touching its hue.

    The curve is evaluated on the BRIGHTEST channel and the resulting factor
    is applied to all three equally, so the channel ratios -- which is what
    hue and chroma are -- pass through unchanged.

    Raising each channel to the power independently, which is what this used
    to do, pulls them together instead: a channel sitting at 0.4 of the
    brightest lands at 0.4**0.55 = 0.60 of it, so every texel drifts toward
    grey. Measured over the E1M1 wall sources that is an 11-16% chroma loss
    on precisely the materials the quantizer then had no choice but to send
    to the neutral ramp (STARTAN3 -15%, STARG3 -16%, BROWN1 -11%), and it
    happened BEFORE the quantizer ever saw them -- so no amount of
    WALL_CHROMA_LOSS_WEIGHT could recover it. Scaling uniformly instead
    *raises* chroma against the source by 33-101% at the same lightness,
    which matters because the VDP's 3-bit lattice is nearly empty in the
    0.010-0.050 chroma band where Doom's wall art lives: a texel a little
    more saturated has a warm rung to land on, one a little less has only
    grey. Map-wide, E1M1's wall area rendering pure neutral falls 40.7% ->
    26.4%.

    Materials that are genuinely grey cannot be tinted by this, structurally
    and at any gamma: when r == g == b the brightest channel IS every
    channel, so the uniform factor is the per-channel one and the texel stays
    exactly neutral. LITE3, SUPPORT2, STARGR1 and DOORSTOP measure 100%
    neutral before and after, which is what says the new colour is being
    reached by genuinely warm texels rather than leaking as a cast.

    Because the factor is derived from the brightest channel, no channel can
    exceed 255: the brightest maps exactly as the old curve mapped it, and
    every other channel is smaller. There is nothing to clip.
    """
    brightest = max(max(0, min(255, channel)) for channel in rgb)
    if brightest <= 0:
        return (0, 0, 0)
    lifted = 255.0 * ((brightest / 255.0) ** WALL_TONE_GAMMA)
    scale = lifted / brightest
    return tuple(int(round(max(0, min(255, channel)) * scale)) for channel in rgb)


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


# === Sky ceiling table =====================================================
# The ceiling run of a sky sector is sourced from a 2D table instead of the
# 4-row flat pattern: SKY_TILE_COLUMNS tile columns of SKY_CEILING_ROWS packed
# rows each, stored COLUMN-MAJOR so that one tile column's rows are contiguous.
# That layout is the whole trick -- it means the pack hot path never learns the
# table is 2D. renderer_hotpath.s still loads a plain row-indexed ceiling
# pointer once per lane; build_bsp_tilemap just points it at a different column
# per tile, so the asm is untouched and the indoor path is bit-identical.
#
# An earlier version of this table averaged each SKY1 row across its full width
# down to ONE index, on the theory that a silhouette which cannot slide reads as
# paint rather than as distance. That was wrong twice over. It destroyed the
# mountains, which are the only thing that makes SKY1 legible as sky at all; and
# because SKY1's lower half averages to a mid grey, it quantized 27 of the 64
# rows to GLOBAL_CEILING_INDEX -- byte-identical to the indoor ceiling, so a
# third of the "sky" was literally the ceiling the player had just walked out
# from under. The silhouette is kept now, and it DOES slide: see
# sky_column_rows in renderer_flats.c.
#
# Still no dither. Every pixel is SKY1's own nearest palette index, so the
# banding that remains is the texture's own, not a Bayer pattern laid over it.
# Quantized into the existing 16 world colours; the sky adds no palette entry
# and therefore cannot recolour walls, sprites, the weapon or items.
#
# The SECOND version of this table was also wrong: it quantized through
# nearest_palette_index with no candidate restriction, and SKY1 is confirmed
# pure greyscale (32768 px sampled, 1383 non-neutral, all anti-aliasing noise,
# max channel 203). WALL_CHROMA_LOSS_WEIGHT only penalises a candidate for
# having LESS chroma than the source -- for an exactly-grey source (chroma 0)
# that can never fire, so the search ran over all 16 colours by raw Oklab
# distance. Dark greys common in the mountain silhouette (11,11,11 and
# 19,19,19, both far more frequent than mid-tones in SKY1) quantized to index
# 1, PAL3's dark red (0x24,0,0), because that red sits closer in the
# `1.25*dL^2 + da^2 + db^2` metric than any true neutral at that lightness --
# not a fringe case, a systematic mis-map across the darkest third of the
# image. That produced the maroon, structureless block the mountains actually
# looked like on screen. The sky is now restricted to PAL3's true neutral
# entries only (SKY_NEUTRAL_INDICES below); it needs no chroma reasoning at
# all because the source has none.
#
# 64 rows, matching PACK_CEILING_ROW_COUNT: walls are centred on the viewport,
# so a ceiling run never starts below row (VIEW_PIXEL_H - 1) / 2 == 59, and a
# power of two lets the pack post mask with an immediate.
SKY_CEILING_ROWS = 64
# Screen row of the horizon. Walls are centred on the viewport
# (top = (VIEW_PIXEL_H - wall_h) / 2), so the ceiling run can never extend past
# the midline; the sky is compressed into those rows and the remainder holds
# the horizon colour so a clipped wall never samples an undefined row.
SKY_HORIZON_ROW = 60
# Tile columns spanning one full revolution. The viewport is RAY_VIEW_TILE_W==20
# tiles across a 90-degree FOV (RAY_PROJ_X == RAY_VIEW_CENTER_X, so
# tan(halfFOV) == 1), so a 360-degree turn sweeps 20 * 360/90 == 80 tiles.
# Sizing the table to exactly that gives true 1:1 parallax and wraps the sky
# once per revolution. Deliberately not rounded to a power of two: a wider table
# would drift SLOWER than the world and a narrower one FASTER, and faster than
# the world is the one behaviour that cannot read as distance. The index wraps
# once per tile column (20 per frame), not per pixel, so a compare-and-subtract
# costs nothing measurable and buys the correct rate.
SKY_TILE_COLUMNS = 80


def solid_flat_row(index):
    """Pack one screen row that is a single palette index across all 8 pixels.

    Spelled as an explicit nibble fold rather than a multiply by a repunit
    constant: tools/test-sector-map.py forbids that literal anywhere in this
    file so a packed WALL byte can never be one index doubled instead of two
    real texels. A flat row genuinely is one index eight times, but the
    guardrail is a source-text check and is worth more than the shorter
    spelling -- do not "simplify" this back.
    """
    nibble = index & 0xF
    word = 0
    for _ in range(8):
        word = (word << 4) | nibble
    return word


def build_flat_ceiling_rows(sector_visuals):
    """Return SKY_CEILING_ROWS packed words for the ordinary (indoor) ceiling.

    The runtime picks a ceiling by pointing at one of two ROM tables, so the
    indoor ceiling has to exist in the same row-indexed shape as the sky. That
    is only representable in ROM while every sector shares one ceiling triple,
    which is how emit_world_assets builds sector_visuals today (see
    GLOBAL_CEILING_INDEX). Assert it instead of trusting it: a future
    per-sector ceiling has to become a RAM table again, and that should be a
    loud extractor failure, not a silently wrong ceiling on half the map.
    """
    ceilings = {tuple(visual[0:3]) for visual in sector_visuals}
    if len(ceilings) != 1:
        raise SystemExit(
            "ROM ceiling table needs one level-wide ceiling, found %d distinct: %s"
            % (len(ceilings), sorted(ceilings)))
    primary, secondary, coverage = ceilings.pop()
    if coverage != 0:
        raise SystemExit(
            "ROM ceiling table cannot express a Bayer-dithered ceiling "
            "(primary %d, secondary %d, coverage %d)"
            % (primary, secondary, coverage))
    return [solid_flat_row(primary)] * SKY_CEILING_ROWS


def pack_flat_row_pixels(indices):
    """Pack eight palette indices into one 4bpp screen row, MSB-first.

    The nibble fold is spelled out for the same reason solid_flat_row's is:
    tools/test-sector-map.py forbids the repunit-multiply shorthand anywhere in
    this file, so that a packed WALL byte can never be one index doubled instead
    of two real texels. Do not "simplify" this either.
    """
    word = 0
    for index in indices:
        word = (word << 4) | (index & 0xF)
    return word


# SKY1 is pure greyscale, so it only ever needs PAL3's true neutral entries --
# restricting to them sidesteps chroma reasoning entirely instead of relying on
# a penalty that cannot fire for a zero-chroma source (see the bug note above).
SKY_NEUTRAL_INDICES = [index for index, color in enumerate(FROZEN_WORLD_PALETTE)
                        if color[0] == color[1] == color[2]]


def build_sky_ceiling_rows(palette):
    """Return SKY_TILE_COLUMNS * SKY_CEILING_ROWS packed words, column-major.

    Index as [column * SKY_CEILING_ROWS + row]. The runtime hands the pack stage
    &table[column * SKY_CEILING_ROWS] and every consumer downstream keeps
    treating it as the plain row-indexed table it was before this became 2D.
    """
    with Image.open(flat_path("F_SKY1")) as image:
        opaque = _fill_transparent_with_average(image)
        width, height = opaque.size
        pixels = list(opaque.get_flattened_data())

    # Deliberately NOT tone_curve'd. That lift exists to recover the sector
    # light compensation the renderer never applies to WALL texels; the sky is
    # not a wall and carries no sector light, so lifting it only blows the
    # zenith out to near-white.
    #
    # Quantizing is the expensive part (nearest_palette_index over 16 colours
    # with a chroma penalty), and the table asks for 80*64*8 == 40960 pixels
    # while SKY1 itself has far fewer distinct source texels than that, so
    # memoize per source pixel rather than per emitted one.
    quantized = {}

    def sample(source_x, source_y):
        key = (source_x, source_y)
        cached = quantized.get(key)
        if cached is None:
            pixel = pixels[(source_y * width) + source_x]
            cached = nearest_palette_index(tuple(pixel[0:3]), palette,
                                            SKY_NEUTRAL_INDICES)
            quantized[key] = cached
        return cached

    # SKY1 is one full revolution wide and the table is one full revolution
    # wide, so screen pixel -> texture column is a straight proportional map.
    screen_width = SKY_TILE_COLUMNS * 8
    words = []
    for column in range(SKY_TILE_COLUMNS):
        for y in range(SKY_CEILING_ROWS):
            if y < SKY_HORIZON_ROW:
                source_y = min((y * height) // SKY_HORIZON_ROW, height - 1)
            else:
                # Below the horizon a wall always covers the ceiling, but the
                # pack post reads eight rows unmasked past a tile boundary, so
                # these have to hold something defined; SKY1's bottom row is the
                # continuation that cannot seam.
                source_y = height - 1
            row = []
            for pixel in range(8):
                source_x = (((column * 8) + pixel) * width) // screen_width
                row.append(sample(min(source_x, width - 1), source_y))
            words.append(pack_flat_row_pixels(row))
    return words


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


# E1M2's three broad stone panels quantize mostly to the global floor rung.
# Retain that best match where possible, but sparsely move the least-cost
# texels to their next-best rung so the wall never becomes a flat solid field.
FLAT_AVOIDING_NEUTRAL_MATERIALS = ("STONE2", "STONE3", "SW1STON1")
FLAT_AVOIDING_MAX_SHARE = 0.49

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
# quantized). The policy this encodes has not changed: single-material brown
# corridor/panel walls go on the earth ramp, two-tone materials whose grey
# metalwork is real do not. Pulling STARTAN3 in for family consistency with
# STARTAN1 was tried and rejected on measurement, and forcing EXITDOOR onto the
# ramp repaints its grey metalwork brown -- worse than the problem solved.
#
# Re-derived from 0.35 when tone_curve stopped desaturating: every material's
# earth share rises once hue survives the tone lift, so the same 0.35 would have
# silently reclassified STARTAN3 (0.317 -> 0.490) and painted its grey supports
# brown -- exactly the failure the paragraph above rejects. The threshold is the
# midpoint of the widest gap in E1M1's distribution, which is also where that
# policy line falls: DOOR1 at 0.651 is the last single-material brown, STARTAN3
# at 0.490 the first two-tone wall, and nothing sits in between. The gap is
# wider than the one 0.35 used to sit in (0.161 against 0.087), so the split is
# less marginal than before, not more.
#
# One material changes class: LITEBLU3 (0.405) leaves the earth ramp. That is a
# fix rather than a cost -- the ramp excludes PAL3's only blue (index 2), so
# classifying a blue light panel as earth was banning it from its own colour.
EARTH_MIN_SHARE = 0.57
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


def material_name_for(path):
    """The uppercase material name a texture path stands for."""
    return os.path.splitext(os.path.basename(path))[0].upper()


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
    # Both metrics have to clear the ceiling: the pair lattice because that is
    # what can crawl in motion, and the displayed grid because that is what
    # reads as salt-and-pepper standing still. Detail is granted only where it
    # is neither.
    if (pair_column_churn(rows, active_height) <= WALL_CHURN_LIMIT and
            horizontal_churn(rows, active_height) <= WALL_CHURN_LIMIT):
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
        # gain_ratio 0 disables dithering outright, for every wall. The Bayer
        # pattern is defined in texture space but the runtime samples each
        # column at a distance-dependent rate through
        # MEGALDOOM_WALL_TEX_Y_BY_HEIGHT, so the pattern never survives
        # resampling as a blend -- it arrives as noise that crawls when the
        # camera moves. Measured over all 23 E1M1 materials, dropping it lowers
        # mean churn from 37% to 31% and *improves* per-texel accuracy, because
        # the averaging the dither pays for is never actually reconstructed on
        # screen.
        #
        # The neutral bucket used to be exempt, on sprite-tuned defaults
        # (73, 0.65), justified by "it rarely fires anyway -- best_mix takes its
        # low-chroma early return on exactly-neutral texels". That was only true
        # because the old per-channel tone_curve desaturated every source into
        # the early return. Once tone_curve stopped destroying hue the exemption
        # armed itself: STONE3's vertical churn went 0.267 -> 0.393, straight
        # through WALL_CHURN_LIMIT, as the dither finally started firing on a
        # large flat stone wall -- which is the one artifact playtesting has
        # already rejected outright. The runtime resamples every wall the same
        # way regardless of material family, so there is one rule, not two.
        pair_max_delta, gain_ratio = WALL_PAIR_MAX_DELTA, 0.0
        if is_neutral_material:
            allowed = [index for index in range(0, WORLD_COLOR_DAMAGE)
                       if world_palette.is_neutral(palette[index])]
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
    sky_rows = build_sky_ceiling_rows(palette)
    flat_ceiling_rows = build_flat_ceiling_rows(sector_visuals)
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
        "// Sky ceiling table: SKY1 sampled into packed 8-pixel screen rows,",
        "// COLUMN-MAJOR -- [column * ROW_COUNT + row], so one tile column's",
        "// rows are contiguous and the pack stage can keep treating a column",
        "// as the flat row-indexed table it always was. TILE_COLUMNS spans one",
        "// full revolution, so the sky wraps once per turn at 1:1 parallax.",
        "// See build_sky_ceiling_rows in tools/world_assets.py.",
        "#define MEGALDOOM_SKY_CEILING_ROW_COUNT %d" % SKY_CEILING_ROWS,
        "#define MEGALDOOM_SKY_HORIZON_ROW %d" % SKY_HORIZON_ROW,
        "#define MEGALDOOM_SKY_TILE_COLUMNS %d" % SKY_TILE_COLUMNS,
        "static const u32 MEGALDOOM_SKY_CEILING_ROWS[MEGALDOOM_SKY_TILE_COLUMNS *",
        "                                           MEGALDOOM_SKY_CEILING_ROW_COUNT] = {",
    ])
    for column in range(SKY_TILE_COLUMNS):
        base = column * SKY_CEILING_ROWS
        lines.append("    // tile column %d" % column)
        for i in range(0, SKY_CEILING_ROWS, 8):
            lines.append("    " + " ".join(
                "0x%08X," % word for word in sky_rows[base + i:base + i + 8]))
    lines.extend([
        "};",
        "",
        "// The indoor counterpart, in the same row-indexed shape so the pack",
        "// stage selects a ceiling by swapping ONE pointer between two ROM",
        "// tables -- no per-frame table build and no work RAM. Valid only",
        "// because every sector shares one ceiling triple; emit_world_assets",
        "// asserts that before emitting rather than letting it drift.",
        "static const u32 MEGALDOOM_FLAT_CEILING_ROWS[MEGALDOOM_SKY_CEILING_ROW_COUNT] = {",
    ])
    for i in range(0, SKY_CEILING_ROWS, 8):
        lines.append("    " + " ".join(
            "0x%08X," % flat_ceiling_rows[i + k] for k in range(8)))
    lines.extend([
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
