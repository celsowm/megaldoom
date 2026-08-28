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


def view_tiles():
    """(RAY_VIEW_TILE_W, RAY_VIEW_TILE_H) -- the viewport in 8px tiles."""
    return (define("RAY_VIEW_TILE_W"), define("RAY_VIEW_TILE_H"))


def view_pixels():
    """(RAY_VIEW_COLS, RAY_VIEW_ROWS) -- the viewport in pixels."""
    tile_w, tile_h = view_tiles()
    return (tile_w * 8, tile_h * 8)
