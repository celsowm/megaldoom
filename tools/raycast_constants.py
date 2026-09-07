"""Single accessor for the #defines in src/raycast.h.

The renderer header is the source of truth for view geometry, the horizontal
sampling stride and the wall texture axes. The C renderer and the asm hotpath
already consume it directly (renderer_pack_abi.h); the offline tools read it
through this module so a constant is never restated as a literal in a bake
script or asserted as a literal in a test.
"""

import os
import re

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RAYCAST_HEADER = os.path.join(PROJECT_ROOT, "src", "raycast.h")

_CACHE = {}


def _source():
    if "source" not in _CACHE:
        with open(RAYCAST_HEADER, "r", encoding="utf-8") as stream:
            _CACHE["source"] = stream.read()
    return _CACHE["source"]


def define(name):
    """Return the integer value of a plain `#define <name> <int>` in raycast.h.

    Only literal integer defines are resolved; derived ones (WALL_TEX_*_MASK,
    RAY_VIEW_COLS, ...) are deliberately not evaluated here, because a partial
    C expression evaluator would be a worse duplicate than none at all.
    """
    match = re.search(r"^#define\s+%s\s+(\d+)\s*$" % re.escape(name),
                      _source(), re.MULTILINE)
    if not match:
        raise RuntimeError("%s is missing from src/raycast.h" % name)
    return int(match.group(1))


def power_of_two_define(name):
    value = define(name)
    if value <= 0 or value & (value - 1):
        raise RuntimeError("%s must be a positive power of two" % name)
    return value


def wall_tex_dims():
    """(WALL_TEX_WIDTH, WALL_TEX_HEIGHT) as the runtime indexes them."""
    return (power_of_two_define("WALL_TEX_WIDTH"),
            power_of_two_define("WALL_TEX_HEIGHT"))


def col_stride():
    """RAY_COL_STRIDE: pixels between sampled wall columns."""
    return power_of_two_define("RAY_COL_STRIDE")


def view_size_count():
    """RAY_VIEW_SIZE_COUNT -- how many viewport presets the OPTIONS menu offers."""
    return define("RAY_VIEW_SIZE_COUNT")


def view_size_default():
    """RAY_VIEW_SIZE_DEFAULT -- the preset a fresh boot selects."""
    return define("RAY_VIEW_SIZE_DEFAULT")


def view_sizes():
    """Every (tile_w, tile_h) preset, in menu order."""
    return [(define("RAY_VIEW_SIZE_%d_W" % i), define("RAY_VIEW_SIZE_%d_H" % i))
            for i in range(view_size_count())]


def view_tiles(size_index=None):
    """(tile_w, tile_h) for one viewport preset; the default preset if unnamed.

    The viewport is runtime-selectable (see the RAY_VIEW_* note in raycast.h), so
    there is no single RAY_VIEW_TILE_W define any more. Tests that model one
    concrete frame want the default preset; tests that model a BUFFER want
    view_tiles_max(), which is what the ROM actually allocates.
    """
    if size_index is None:
        size_index = view_size_default()
    return view_sizes()[size_index]


def view_tiles_max():
    """(RAY_VIEW_TILE_W_MAX, RAY_VIEW_TILE_H_MAX) -- what every buffer is sized at."""
    return (define("RAY_VIEW_TILE_W_MAX"), define("RAY_VIEW_TILE_H_MAX"))


def view_pixels(size_index=None):
    """(cols, rows) -- one viewport preset in pixels."""
    tile_w, tile_h = view_tiles(size_index)
    return (tile_w * 8, tile_h * 8)


def view_pixels_max():
    """(RAY_VIEW_COLS_MAX, RAY_VIEW_ROWS_MAX) -- the largest viewport in pixels."""
    tile_w, tile_h = view_tiles_max()
    return (tile_w * 8, tile_h * 8)


def proj():
    """RAY_PROJ_X == RAY_PROJ_Y -- the projection scale.

    Deliberately NOT derived from the viewport width: it is a fixed constant so
    that a larger viewport widens the field instead of magnifying it, and so the
    baked billboard reciprocal LUT stays exact at every preset.
    """
    value = define("RAY_PROJ_X")
    if value != define("RAY_PROJ_Y"):
        raise RuntimeError("RAY_PROJ_X and RAY_PROJ_Y must agree")
    return value
