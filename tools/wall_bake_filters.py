#!/usr/bin/env python3
"""Generic lattice filters for the wall bake: dimensions, contrast, blur,
edge detection, isolated-texel cleanup, resizing, and the pair-lattice churn
metrics.

Split out of world_assets.py (which used to carry these alongside palette
construction and the C emitter) because none of this code needs to know
anything about PAL3 or Doom map data -- it operates purely on [x][y] grids
of RGB tuples or palette indices, plus the WALL_TEX_* geometry of the runtime
texture. world_assets.py imports these names and re-exports them, so nothing
outside this pair changes.
"""

from collections import Counter

import raycast_constants
import world_palette

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

# Share of adjacent samples whose palette index differs. The prior "ugly walls"
# investigation concluded the defect was churn, not resolution, so this ceiling
# governs whether a material is allowed sub-texel horizontal detail at all (see
# convert_texture, which asks it of pair_column_churn -- the lattice that can
# actually alias) and bounds what the bake may emit as displayed (which
# tools/test-wall-quality.py asserts of horizontal_churn). Both callers assert
# this constant rather than restating the number.
WALL_CHURN_LIMIT = 0.35

# PAL3's neutral rungs sit ~0.13 apart in Oklab L (idx 3=0.260, 5=0.402,
# 7=0.535, 10=0.657, 13=0.776). Several E1M1 materials have a p2..p98 luminance
# spread *narrower than one rung* -- BROWN144 0.109, BROWNGRN 0.169 -- so the
# quantizer had no choice but to collapse 39-75% of their texels onto a single
# index, which then read as a solid block the same colour as the floor. The fix
# is a per-texture contrast expansion around the material's OWN median: a dark
# BROWN144 corridor stays dark (its mean luminance is preserved) but regains
# internal structure. Only L is scaled; chroma rides along, so browns stay
# brown. This is the same "levels lift" world_assets.WALL_TONE_GAMMA already
# performs globally, applied per material instead of uniformly.
WALL_TARGET_SPREAD = 0.36  # ~2.7 PAL3 rungs
WALL_MAX_CONTRAST_GAIN = 3.0

# Spatial low-pass applied after the contrast expansion, before quantizing.
# The expansion above deliberately amplifies luminance detail, and at 16 colours
# any residual pixel-level ripple lands on alternating palette rungs: measured
# as "churn" (share of horizontally adjacent texels with different indices) the
# worst materials sat at 56-71% -- essentially salt-and-pepper. RAY_COL_STRIDE 2
# then resamples that noise per column at a distance-dependent rate, which is
# what turns it into the crawling vertical streaks visible in motion. A 1-w-1
# separable kernel on the runtime texture torus (matching the wrap the runtime
# sampler uses) removes the ripple while leaving real edges intact.
#
# The horizontal half of that kernel is applied on the PAIR lattice, not on the
# display lattice, because only the pair lattice can alias. Since Fatia 1 each
# packed byte carries two adjacent texels that the hotpath's single move.b
# always draws together, in fixed left/right order, at the same screen scale
# (view tiles are never flipped). Detail *inside* a pair is therefore nailed to
# the byte: the DDA cannot resample it, so it can never crawl. Detail *between*
# pairs is what MEGALDOOM_WALL_TEX_X steps through at a distance-dependent rate,
# and that is the ripple this filter exists to remove. Blurring the display
# lattice destroyed both, which is the "pre-processing over-simplifies detail"
# complaint: it was calibrated against the 2:1 squash that Fatia 1 removed.
WALL_SMOOTH_WEIGHT = 2


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
            # world_assets.WALL_TONE_GAMMA's lift keeps the expression positive
            # today, so any darker tone curve trips it.
            target = max(0.0, median + (value - median) * gain)
            if target <= 0.0:
                result[x][y] = (0, 0, 0)
                continue
            scale = (target / value) ** 1.25
            result[x][y] = tuple(max(0, min(255, int(round(channel * scale))))
                                 for channel in color)
    return result


def _pair_split(columns):
    """Decompose a display-lattice texture into pair means and the residual.

    The mean grid is the lattice the runtime samples; the residual is the part
    of the signal that is nailed to a packed byte and therefore cannot be
    resampled. Filtering the first and restoring the second is what lets the
    low-pass remove crawl without removing detail.
    """
    pairs = len(columns) // 2
    height = len(columns[0])
    mean = [[tuple((columns[2 * p][y][c] + columns[2 * p + 1][y][c]) / 2.0
                   for c in range(3))
             for y in range(height)] for p in range(pairs)]
    residual = [[tuple(columns[x][y][c] - mean[x // 2][y][c] for c in range(3))
                 for y in range(height)] for x in range(len(columns))]
    return mean, residual


def _pair_join(mean, residual):
    return [[tuple(max(0, min(255, int(round(mean[x // 2][y][c] +
                                             residual[x][y][c]))))
                   for c in range(3))
             for y in range(len(residual[0]))] for x in range(len(residual))]


def _tap(grid, x, y, nx, ny, edge_mask):
    """A neighbour sample, clamped to the centre when the neighbour is an edge."""
    return grid[x][y] if edge_mask is not None and edge_mask[nx][ny] else grid[nx][ny]


def _blur_axis(grid, weight, edge_mask, vertical):
    """One half of the separable 1-weight-1 kernel, wrapping like the runtime.

    Both the plain and the edge-aware filter are this same kernel; the only
    difference is whether a mask clamps taps at boundaries, so there is one
    implementation rather than two that must be kept in step.
    """
    width = len(grid)
    height = len(grid[0])
    total = weight + 2
    result = [[None] * height for _ in range(width)]
    for x in range(width):
        for y in range(height):
            if edge_mask is not None and edge_mask[x][y]:
                result[x][y] = grid[x][y]
                continue
            if vertical:
                a = _tap(grid, x, y, x, (y - 1) % height, edge_mask)
                b = _tap(grid, x, y, x, (y + 1) % height, edge_mask)
            else:
                a = _tap(grid, x, y, (x - 1) % width, y, edge_mask)
                b = _tap(grid, x, y, (x + 1) % width, y, edge_mask)
            centre = grid[x][y]
            result[x][y] = tuple((a[c] + weight * centre[c] + b[c]) / total
                                 for c in range(3))
    return result


def _horizontal_smooth(columns, weight, edge_mask=None, pair_preserving=True):
    """1-weight-1 blur along x, wrapping like the runtime sampler.

    On the display lattice the filter runs on the pair means and the intra-pair
    residual is added back untouched, so the half of the signal the DDA cannot
    resample survives at full amplitude. On any other lattice -- the curated
    facade grid, and the pair-width fallback bake -- one column already *is* one
    sample, so the plain blur is the same filter. See WALL_SMOOTH_WEIGHT.
    """
    if not pair_preserving or len(columns) != WALL_TEX_DISPLAY_WIDTH:
        return _quantize(_blur_axis(columns, weight, edge_mask, False))
    mean, residual = _pair_split(columns)
    # A pair containing an edge texel is an edge pair: the mask exists to stop
    # the filter reaching across a panel boundary, and the pair is the sample.
    pair_mask = None if edge_mask is None else [
        [edge_mask[2 * p][y] or edge_mask[2 * p + 1][y]
         for y in range(len(columns[0]))]
        for p in range(len(mean))]
    return _pair_join(_blur_axis(mean, weight, pair_mask, False), residual)


def _quantize(grid):
    return [[tuple(max(0, min(255, int(round(value)))) for value in cell)
             for cell in column] for column in grid]


def _smooth(columns, weight, edge_mask=None, pair_preserving=True):
    """Separable blur, wrapping like the runtime sampler. See
    WALL_SMOOTH_WEIGHT."""
    if weight <= 0:
        raise ValueError("Wall bake smooth_weight must be positive")
    horizontal = _horizontal_smooth(columns, weight, edge_mask, pair_preserving)
    return _quantize(_blur_axis(horizontal, weight, edge_mask, True))


def _spatial_smooth(columns, weight=WALL_SMOOTH_WEIGHT):
    return _smooth(columns, weight)


def _edge_aware_spatial_smooth(columns, edge_mask, weight):
    """Apply the separable filter a second time without bleeding across edges.

    Not pair-preserving, and that is the point: its only caller runs it on the
    output of _spatial_smooth, which already carried the intra-pair residual
    through untouched. This pass is the extra simplification a curated recipe
    asks for -- it flattens what survived, everywhere except across a panel
    boundary. Preserving the residual twice would mean never filtering it at
    all, and measurably costs these materials edge retention.
    """
    return _smooth(columns, weight, edge_mask, pair_preserving=False)


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
            # No neighbour agrees at all, and the other nibble of this texel's
            # own byte does not either. A boundary is a run of texels; a sample
            # that nothing around it supports, on any lattice, is a speck. This
            # is the same island proof as the clause above, taken from "every
            # neighbour agrees with every other" to "none agrees with this one",
            # and like that clause it may act inside the edge mask -- which is
            # the only reason these survived at panel corners.
            partner = rows[y][(x ^ 1) if width == WALL_TEX_DISPLAY_WIDTH else x]
            if (all(value != rows[y][x] for value in all_values) and
                    partner != rows[y][x]):
                proposals[(x, y)] = Counter(all_values).most_common(1)[0][0]
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


def pair_column_churn(rows, active_height):
    """horizontal_churn measured between PAIR columns instead of texels.

    The two metrics answer different questions and both are needed. The eye
    sees the displayed grid, which is what horizontal_churn reports. But only a
    change from one pair to the next can crawl, because the pair column is the
    unit MEGALDOOM_WALL_TEX_X steps through at a distance-dependent rate; a
    change inside a pair is two nibbles of one byte that the hotpath always
    emits together, at one scale, in fixed order. So the question "may this
    material carry sub-texel detail at all?" has to be asked of this metric --
    asking it of the displayed grid counts the very detail being granted as a
    reason to refuse it.
    """
    return horizontal_churn([pair_column_texels(row) for row in rows],
                            active_height)


def packed_pair_byte(level, left_index, right_index):
    """One FREEDOOM_WALL_PACKED_PAIRS byte: the two pixels of a stride-2 sample.

    The hotpath writes this byte straight into a 4bpp tile row, so the high
    nibble lands on the even screen pixel and the low nibble on the odd one.
    Carrying two adjacent display texels here rather than one index twice is
    what doubles the wall's horizontal resolution for zero runtime cost.
    """
    return ((level[left_index & 0x0F] & 0x0F) << 4) | (level[right_index & 0x0F] & 0x0F)
