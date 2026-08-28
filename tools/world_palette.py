#!/usr/bin/env python3
"""Deterministic Mega Drive PAL3 quantization in perceptual colour space."""

from functools import lru_cache
import math


BAYER_4X4 = (
    (0, 8, 2, 10),
    (12, 4, 14, 6),
    (3, 11, 1, 9),
    (15, 7, 13, 5),
)


def md_color(rgb):
    return tuple(int(round(channel * 7 / 255)) * 255 // 7 for channel in rgb)


VDP_COLORS = tuple(
    (r * 255 // 7, g * 255 // 7, b * 255 // 7)
    for r in range(8) for g in range(8) for b in range(8)
)


@lru_cache(maxsize=None)
def oklab(rgb):
    linear = []
    for channel in rgb:
        value = channel / 255.0
        linear.append(value / 12.92 if value <= 0.04045 else ((value + 0.055) / 1.055) ** 2.4)
    r, g, b = linear
    l = 0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b
    m = 0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b
    s = 0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b
    l = math.copysign(abs(l) ** (1 / 3), l)
    m = math.copysign(abs(m) ** (1 / 3), m)
    s = math.copysign(abs(s) ** (1 / 3), s)
    return (
        0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
        1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
        0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s,
    )


def distance_sq(first, second, chroma_loss_weight=0.0):
    """Perceptual distance, optionally penalising desaturation asymmetrically.

    The base term slightly favours lightness accuracy: Doom walls read
    primarily through their ramps, while hue errors create the objectionable
    olive cast.

    chroma_loss_weight exists because that base term, on its own, systematically
    greys walls out. PAL3's neutral rungs sit at Oklab L 0.000/0.260/0.402/
    0.535/0.657/0.776 and its warm rungs at 0.163/0.309/0.436/0.562/0.682 --
    interleaved, so roughly half of a tan texel's possible lightnesses land
    nearer a grey than a brown. A genuine tan like STARTAN3's (155,138,118),
    chroma 0.035, then measures closer to #919191 (0.00148) than to #B6916D
    (0.00294) and quantizes flat grey, because the grey's chroma *undershoot*
    and the brown's *overshoot* cost the same while the lightness term breaks
    the tie. Weighting chroma symmetrically does not help -- it penalises the
    overshoot just as hard, and measured over E1M1 it makes STARTAN3 worse
    (70.2% -> 75.1% neutral).

    The asymmetry is the point: losing chroma reads as the wrong material,
    while carrying too much reads as vivid. Only the loss is charged, so a
    source texel that is exactly neutral pays nothing and cannot be tinted --
    which is why LITE3, SUPPORT2 and STONE2 stay 100% neutral at every weight.
    """
    a = oklab(tuple(first))
    b = oklab(tuple(second))
    cost = 1.25 * (a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2
    if chroma_loss_weight:
        loss = math.hypot(a[1], a[2]) - math.hypot(b[1], b[2])
        if loss > 0.0:
            cost += chroma_loss_weight * loss * loss
    return cost


def is_neutral(color):
    return color[0] == color[1] == color[2]


def is_green(color):
    return color[1] > color[0] + 18 and color[1] > color[2] + 18


def is_blue(color):
    return color[2] > color[0] + 18 and color[2] > color[1] + 18


def is_warm(color):
    return color[0] >= color[1] + 18 and color[1] >= color[2] and color[0] - color[2] >= 24


def is_earth(color):
    return color[0] >= color[1] + 18 and color[1] >= color[2] + 18


def is_muted_brown(color):
    return color[0] >= 109 and color[0] >= color[1] + 18 and \
        color[1] >= 36 and abs(color[1] - color[2]) <= 18


def is_olive(color):
    return abs(color[0] - color[1]) <= 18 and color[1] >= color[2] + 18


def is_saturated_red(color):
    return color[0] >= 145 and color[1] <= 36 and color[2] <= 36


def is_dark_warm(color):
    """Warm colours in the L<0.35 band -- the rung missing between the
    0x242424 and 0x484848 neutrals where most of E1M1's brown corridor
    textures (BROWN144, BROWN96, BRNBIG*, COMPUTE2) actually live. Without a
    dedicated slot here they fall inside the neutral clamp and quantize flat
    to the floor's own grey (see tools/test-wall-quality.py dark-brown checks)."""
    return is_warm(color) and oklab(color)[0] < 0.35


def chroma(color):
    _, a, b = oklab(tuple(color))
    return math.hypot(a, b)


def hue(color):
    """Oklab hue angle in degrees. Only meaningful above a chroma floor --
    every neutral lands on the same degenerate ~90 deg, so callers must gate on
    chroma() first."""
    _, a, b = oklab(tuple(color))
    return math.degrees(math.atan2(b, a)) % 360.0


def _family_sources(histogram, predicate):
    selected = [(tuple(color), weight) for color, weight in histogram.items()
                if weight > 0 and predicate(color)]
    return selected or [(tuple(color), weight) for color, weight in histogram.items() if weight > 0]


def _add_greedy_slots(selected, candidates, sources, count):
    candidates = tuple(sorted(set(candidates)))
    for _ in range(count):
        remaining = [candidate for candidate in candidates if candidate not in selected]
        if not remaining:
            return
        best = min(remaining, key=lambda candidate: (
            sum(weight * min(
                [distance_sq(source, candidate)] +
                [distance_sq(source, existing) for existing in selected])
                for source, weight in sources),
            candidate,
        ))
        selected.append(best)


def build_palette(histogram, damage, warning):
    """Build 16 VDP colours with explicit Doom material-family coverage."""
    black = (0, 0, 0)
    damage = md_color(damage)
    warning = md_color(warning)
    excluded = {black, damage, warning}
    available = [color for color in VDP_COLORS if color not in excluded]
    selected = []

    # Fixed family budgets prevent a sprite-heavy histogram from consuming the
    # concrete/metal and brown ramps needed by most wall pixels. Neutral gets 5
    # (not fewer): GRAY7/METAL1/STONE2 are quantized neutral-only (see
    # convert_texture's GRAY/METAL/STONE special case in wad-map-extract.py),
    # so an under-sized or gappy neutral ramp bands badly regardless of how
    # good the rest of the palette is.
    _add_greedy_slots(selected, [c for c in available if is_neutral(c)],
                      _family_sources(histogram, lambda c: max(c) - min(c) <= 18), 5)
    _add_greedy_slots(selected, [c for c in available if is_earth(c) and c[0] <= 145],
                      _family_sources(histogram, lambda c: is_earth(c) and c[0] <= 145), 1)
    _add_greedy_slots(selected, [c for c in available if is_earth(c) and c[0] >= 182],
                      _family_sources(histogram, lambda c: is_earth(c) and c[0] >= 182), 1)
    _add_greedy_slots(selected, [c for c in available if is_muted_brown(c)],
                      _family_sources(histogram, is_muted_brown), 1)
    _add_greedy_slots(selected, [c for c in available
                                 if is_warm(c) and not is_saturated_red(c) and c[0] <= 182],
                      _family_sources(histogram, is_warm), 1)
    # Dedicated dark-warm slot: without it, BROWN144/BROWN96/BRNBIG*/COMPUTE2
    # (source luminance ~35-39/255) fall inside the neutral clamp between
    # 0x242424 and 0x484848 and quantize flat to the floor's own grey.
    _add_greedy_slots(selected, [c for c in available if is_dark_warm(c)],
                      _family_sources(histogram, is_dark_warm), 1)
    _add_greedy_slots(selected, [c for c in available if is_blue(c)],
                      _family_sources(histogram, is_blue), 1)
    _add_greedy_slots(selected, [c for c in available if is_green(c)],
                      _family_sources(histogram, is_green), 1)
    # One unrestricted slot to round out whatever the fixed budgets above did
    # not cover, excluded from neutral so the total stays at 5 -- PAL3 was
    # previously burning 7 of 16 on grey ramps
    # (000000/242424/484848/6D6D6D/919191/B6B6B6/DADADA) while B6B6B6 and
    # DADADA have almost no source weight in E1M1 outside LITE3.
    # Olive is no longer excluded here. It was, and the frozen palette that
    # ships is the evidence that was wrong: the olive/khaki band is 15.1% of
    # E1M1's wall area and had nowhere to go, so a quarter of the map rendered
    # flat grey. The exclusion was aimed at an olive cast on neutral surfaces,
    # which allowed_indices' low-chroma clamp is what actually prevents.
    global_candidates = [color for color in available
                         if not is_blue(color) and not is_green(color) and
                         not is_saturated_red(color) and not is_neutral(color)]
    _add_greedy_slots(selected, global_candidates,
                      _family_sources(histogram, lambda _c: True), 1)

    if len(selected) != 13:
        raise RuntimeError("PAL3 adaptive slot allocation did not produce 13 colours")
    selected.sort(key=lambda c: (oklab(c)[0], c))
    return [black] + selected + [damage, warning]


def _is_low_chroma(rgb, chroma_threshold):
    # chroma_threshold is None: legacy RGB-spread clamp (used by sprite/HUD
    # conversion via world-palette-lut.py, left untouched so PAL3 rebalancing
    # for walls does not also recolor the weapon and every billboard).
    # chroma_threshold is a number: Oklab-chroma clamp used for wall/flat
    # conversion, which correctly separates real dark browns (BROWN144
    # chroma~0.026, BROWN96 chroma~0.033) from truly neutral surfaces
    # (COMPUTE2 chroma~0.008, BROWNGRN chroma~0.014) that an absolute RGB
    # spread of 36 could not tell apart.
    if chroma_threshold is None:
        return max(rgb) - min(rgb) <= 36
    return chroma(rgb) <= chroma_threshold


def allowed_indices(rgb, palette, opaque=True, chroma_threshold=None):
    indices = list(range(1 if opaque else 0, len(palette)))
    if _is_low_chroma(rgb, chroma_threshold):
        neutral = [index for index in indices if is_neutral(palette[index])]
        if neutral:
            return neutral
    if not is_green(rgb):
        indices = [index for index in indices if not is_green(palette[index])]
    if is_warm(rgb):
        warm_or_neutral = [index for index in indices
                           if is_warm(palette[index]) or is_neutral(palette[index])]
        if warm_or_neutral:
            indices = warm_or_neutral
    return indices


def nearest_index(rgb, palette, allowed=None, chroma_loss_weight=0.0):
    indices = range(len(palette)) if allowed is None else allowed
    return min(indices, key=lambda index:
               (distance_sq(rgb, palette[index], chroma_loss_weight), index))


def best_mix(rgb, palette, opaque=True, allowed=None, chroma_threshold=None,
            pair_max_delta=73, gain_ratio=0.65, chroma_loss_weight=0.0):
    """Return (primary, secondary, secondary_coverage_0_to_16).

    pair_max_delta and gain_ratio gate how readily dithering is offered: the
    defaults match the historical sprite/HUD behaviour (world-palette-lut.py
    calls this with no overrides). Wall/flat conversion passes tighter values
    to suppress the salt-and-pepper pairing that RAY_COL_STRIDE turns into a
    visible, flickering 2px checkerboard (see tools/test-wall-quality.py).
    """
    indices = allowed_indices(rgb, palette, opaque, chroma_threshold)
    if allowed is not None:
        allowed_set = set(allowed)
        indices = [index for index in indices if index in allowed_set]
    target = oklab(tuple(rgb))
    primary = nearest_index(rgb, palette, indices, chroma_loss_weight)
    primary_error = distance_sq(rgb, palette[primary], chroma_loss_weight)
    if _is_low_chroma(rgb, chroma_threshold):
        return primary, primary, 0
    best = (primary_error, primary, primary, 0)
    for position, first in enumerate(indices):
        a = oklab(tuple(palette[first]))
        for second in indices[position + 1:]:
            if max(abs(palette[first][channel] - palette[second][channel])
                   for channel in range(3)) > pair_max_delta:
                continue
            b = oklab(tuple(palette[second]))
            vector = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
            denom = sum(value * value for value in vector)
            if denom <= 1e-12:
                continue
            amount = sum((target[i] - a[i]) * vector[i] for i in range(3)) / denom
            amount = max(0.0, min(1.0, amount))
            mixed = tuple(a[i] + amount * vector[i] for i in range(3))
            error = (1.25 * (target[0] - mixed[0]) ** 2 +
                     (target[1] - mixed[1]) ** 2 + (target[2] - mixed[2]) ** 2)
            coverage = max(0, min(16, int(round(amount * 16))))
            candidate = (error, first, second, coverage)
            if candidate < best:
                best = candidate
    error, first, second, coverage = best
    # Avoid high-contrast salt-and-pepper pairs when their average only barely
    # beats the closest solid colour. Strong gains still receive dithering.
    if error >= primary_error * gain_ratio:
        return primary, primary, 0
    if coverage == 0:
        return first, first, 0
    if coverage == 16:
        return second, second, 0
    return first, second, coverage


def dither_index(rgb, palette, x, y, opaque=True, cache=None, allowed=None,
                 chroma_threshold=None, pair_max_delta=73, gain_ratio=0.65,
                 chroma_loss_weight=0.0):
    key = tuple(int(channel * 31 / 255) * 255 // 31 for channel in rgb)
    if cache is not None and key in cache:
        first, second, coverage = cache[key]
    else:
        first, second, coverage = best_mix(key, palette, opaque, allowed,
                                           chroma_threshold, pair_max_delta,
                                           gain_ratio, chroma_loss_weight)
        if cache is not None:
            cache[key] = (first, second, coverage)
    return second if BAYER_4X4[y & 3][x & 3] < coverage else first
