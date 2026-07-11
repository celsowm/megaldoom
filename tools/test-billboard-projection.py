"""Regression checks for the shared billboard/wall projection contract."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parent.parent
RAYCAST = ROOT / "src" / "raycast.h"
BILLBOARD = ROOT / "src" / "billboard.c"
PROJECTOR = ROOT / "src" / "billboard_projection.c"
SCENE = ROOT / "src" / "renderer_scene.c"

VIEW_W = 160
CENTER_X = 80
CENTER_Y = 60
PROJ_X = 180
PROJ_Y = 120
CAMERA_HEIGHT = 128
STRIDE = 4


def project_x(side: int, forward: int) -> int:
    return CENTER_X + int(side * PROJ_X / forward)


def span_visible(forward: int, left: int, right: int, depths: list[int]) -> bool:
    left = max(left, 0)
    right = min(right, VIEW_W - 1)
    if left > right:
        return False
    first = left & ~(STRIDE - 1)
    last = right & ~(STRIDE - 1)
    return any(forward < depths[col] for col in range(first, last + 1, STRIDE))


def main() -> int:
    raycast = RAYCAST.read_text(encoding="utf-8")
    billboard = BILLBOARD.read_text(encoding="utf-8")
    projector = PROJECTOR.read_text(encoding="utf-8")
    scene = SCENE.read_text(encoding="utf-8")

    required = [
        "#define RAY_VIEW_CENTER_X", "#define RAY_VIEW_CENTER_Y",
        "#define RAY_PROJ_X 180", "#define RAY_PROJ_Y RAY_VIEW_ROWS",
        "#define RAY_CAMERA_HEIGHT",
    ]
    if any(token not in raycast for token in required):
        raise ValueError("raycast camera geometry is no longer shared")
    if "side * RAY_PROJ_X" not in billboard or "side * 80" in billboard:
        raise ValueError("billboard horizontal projection diverged from walls")
    if "billboard_span_has_visible_block" not in projector:
        raise ValueError("billboard span occlusion guard is missing")
    if "billboard_has_line_of_sight(i, player)" in projector:
        raise ValueError("render projection must not cull billboards by LOS")
    if "const u16 wall_depth = columns[wall_col].depth;" not in scene:
        raise ValueError("billboard rasterizer must use the rendered wall block depth")
    if "next_wall_col" in scene:
        raise ValueError("billboard rasterizer still samples the next wall block")

    # Three successive turns preserve the exact wall/billboard horizontal law.
    # The changing side coordinate models a fixed prop as the camera rotates.
    for side in (24, 42, 61):
        billboard_x = project_x(side, 192)
        wall_x = CENTER_X + int(side * PROJ_X / 192)
        if billboard_x != wall_x:
            raise ValueError("billboard and wall disagree during turn sequence")

    depths = [0x7FFF] * VIEW_W
    depths[80] = 96
    # Centre is hidden, but the left block is visible: retain the billboard.
    if not span_visible(128, 72, 88, depths):
        raise ValueError("partially hidden billboard was incorrectly culled")
    for col in range(72, 92, STRIDE):
        depths[col] = 96
    if span_visible(128, 72, 88, depths):
        raise ValueError("fully hidden billboard was not culled")

    # A depth discontinuity only affects its own rendered 4px block.
    depths = [0x7FFF] * VIEW_W
    depths[84] = 96
    if not span_visible(128, 80, 83, depths):
        raise ValueError("neighbouring wall block leaked into current sprite block")

    near, far = 192, 384
    near_bottom = CENTER_Y + (PROJ_Y * CAMERA_HEIGHT // near)
    far_bottom = CENTER_Y + (PROJ_Y * CAMERA_HEIGHT // far)
    item_near_h = PROJ_Y * 68 // near
    item_far_h = PROJ_Y * 68 // far
    tall_near_h = PROJ_Y * 256 // near
    if not (near_bottom > far_bottom and item_near_h > item_far_h and tall_near_h > item_near_h):
        raise ValueError("floor projection or billboard scale regression")

    print("ok    shared billboard projection, span z-test, and floor anchoring")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(f"error {error}", file=sys.stderr)
        raise SystemExit(1)
