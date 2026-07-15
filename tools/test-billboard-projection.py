"""Regression checks for the shared billboard/wall projection contract."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parent.parent
RAYCAST = ROOT / "src" / "raycast.h"
BILLBOARD = ROOT / "src" / "billboard.c"
BILLBOARD_INTERNAL = ROOT / "src" / "billboard_internal.h"
PROJECTOR = ROOT / "src" / "billboard_projection.c"
SCENE = ROOT / "src" / "renderer_scene.c"

VIEW_W = 160
CENTER_X = 80
CENTER_Y = 60
PROJ_X = 180
PROJ_Y = 120
CAMERA_HEIGHT = 128
SCALE_SHIFT = 12
WORLD_GEOMETRY_SCALE = 3
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


def projected_q12(value: int, scale: int) -> int:
    product = value * scale
    return -(abs(product) >> SCALE_SHIFT) if product < 0 else product >> SCALE_SHIFT


def project_patch(forward: int, center: int, width: int, height: int,
                  left_offset: int, top_offset: int) -> tuple[int, int, int, int, int]:
    sx = (PROJ_X << SCALE_SHIFT) // forward
    sy = (PROJ_Y << SCALE_SHIFT) // forward
    origin_y = CENTER_Y + projected_q12(CAMERA_HEIGHT, sy)
    left = center - projected_q12(left_offset * WORLD_GEOMETRY_SCALE, sx)
    right = center + projected_q12(
        (width - left_offset) * WORLD_GEOMETRY_SCALE, sx) - 1
    # Sprite width and height share one focal scale. Only its world-floor origin
    # uses the wall camera's vertical projection.
    top = origin_y - projected_q12(top_offset * WORLD_GEOMETRY_SCALE, sx)
    bottom = origin_y + projected_q12(
        (height - top_offset) * WORLD_GEOMETRY_SCALE, sx) - 1
    return left, max(left, right), top, max(top, bottom), origin_y


def transform_component(basis: int, delta: int) -> tuple[int, str]:
    """Model the guarded native-word path and its exact wide fallback."""
    path = "word" if -32768 <= delta <= 32767 else "wide"
    return basis * delta, path


def early_culled(side: int, forward: int, width: int,
                 left_offset: int, scale: int) -> bool:
    left_extent = left_offset * scale
    right_extent = (width - left_offset) * scale
    left_numerator = (side - left_extent) * PROJ_X
    right_numerator = (side + right_extent) * PROJ_X
    left_clip = -(CENTER_X + 2) * forward
    right_clip = (VIEW_W - CENTER_X + 2) * forward
    return right_numerator < left_clip or left_numerator >= right_clip


def geometry_key(type_id: int, visual: int, frame: int) -> int:
    geometry_frame = frame if visual == 19 else 0
    return ((type_id & 0x1F) | ((visual & 0x1F) << 5) |
            ((geometry_frame & 0x0F) << 10))


def cache_hit(cached: tuple[int, int, int, int],
              current: tuple[int, int, int, int]) -> bool:
    """Camera generation, object X/Y, and geometry key must all match."""
    return cached == current


def main() -> int:
    raycast = RAYCAST.read_text(encoding="utf-8")
    billboard = BILLBOARD.read_text(encoding="utf-8")
    billboard_internal = BILLBOARD_INTERNAL.read_text(encoding="utf-8")
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
    if "columns[wall_col].depth" not in scene:
        raise ValueError("billboard rasterizer must use the rendered wall block depth")
    if "billboard_depth_visible(object, columns[wall_col].depth)" not in scene:
        raise ValueError("billboard rasterizer no longer applies typed wall-depth visibility")
    if "next_wall_col" in scene:
        raise ValueError("billboard rasterizer still samples the next wall block")
    if "RAY_CAMERA_HEIGHT" not in billboard or "BILLBOARD_SCALE_SHIFT 12" not in billboard:
        raise ValueError("world billboards are not using the shared Q12 render camera")
    if "((s32)RAY_CAMERA_HEIGHT * RAY_PROJ_Y) / forward" not in billboard:
        raise ValueError("enemy baseline is no longer anchored to the rendered floor plane")
    if "PLAYER_EYE_HEIGHT * RAY_PROJ_Y" in billboard:
        raise ValueError("enemy projection is using gameplay eye height instead of the render camera")
    if "#define BILLBOARD_WORLD_GEOMETRY_SCALE 3" not in billboard_internal:
        raise ValueError("world billboard scale no longer matches 256-unit BSP walls")
    if "geometry.top_offset, scale_x_q12" not in billboard:
        raise ValueError("sprite height is no longer using the width focal scale")
    if "FREEDOOM_BILLBOARD_WORLD_GEOMETRY" not in billboard:
        raise ValueError("world billboard projection is not source-geometry driven")
    if "object->atlas_w << 8" not in scene or "object->atlas_h << 16" not in scene:
        raise ValueError("billboard rasterizer is not sampling the generated atlas crop")
    if "BILLBOARD_ENEMY_WORLD_WIDTH" not in billboard or "uses_wad_origin = FALSE" not in billboard:
        raise ValueError("enemy legacy geometry is no longer isolated")
    if "#define BILLBOARD_ENEMY_WORLD_WIDTH 63" not in billboard_internal or \
            "#define BILLBOARD_ENEMY_WORLD_HEIGHT 189" not in billboard_internal:
        raise ValueError("grounded enemy geometry no longer preserves its former head line and aspect")
    if ("billboard_mul_basis_delta" not in billboard or
            "muls.w %1,%0" not in billboard or
            "return (s32)basis * delta;" not in billboard):
        raise ValueError("guarded native MULS.W transform or exact fallback is missing")
    if ("left_numerator" not in billboard or "right_numerator" not in billboard or
            "RAY_VIEW_CENTER_X + 2" not in billboard):
        raise ValueError("conservative pre-division frustum rejection is missing")
    cache_tokens = [
        "billboard_projection_cache_begin", "s_cache_player_x == player->x",
        "s_cache_player_y == player->y", "s_cache_player_angle == player->angle",
        "cache->object_x == object->x", "cache->object_y == object->y",
        "cache->geometry_key == geometry_key",
        "visual == BILLBOARD_VISUAL_BARREL_EXPLODING",
    ]
    if any(token not in projector for token in cache_tokens):
        raise ValueError("billboard measurement cache invalidation contract changed")

    # Three successive turns preserve the exact wall/billboard horizontal law.
    # The changing side coordinate models a fixed prop as the camera rotates.
    for side in (24, 42, 61):
        billboard_x = project_x(side, 192)
        wall_x = CENTER_X + int(side * PROJ_X / 192)
        if billboard_x != wall_x:
            raise ValueError("billboard and wall disagree during turn sequence")

    # Signed-word boundaries use native MULS.W; only genuinely wide deltas take
    # the exact 32-bit fallback. Both paths produce the same mathematical value.
    for delta, expected_path in (
            (-32769, "wide"), (-32768, "word"), (-1, "word"),
            (0, "word"), (32767, "word"), (32768, "wide")):
        product, path = transform_component(-1536, delta)
        if product != -1536 * delta or path != expected_path:
            raise ValueError("native/fallback camera transform boundary diverged")

    # Differentially compare the cheap reject with the exact projected span.
    # Any authored sprite that reaches the viewport must survive the early test.
    geometries = ((23, 10, WORLD_GEOMETRY_SCALE),
                  (28, 13, WORLD_GEOMETRY_SCALE),
                  (32, 16, 1), (48, 24, 1))
    for forward in (33, 48, 64, 96, 128, 192, 384, 768, 1535):
        sx = (PROJ_X << SCALE_SHIFT) // forward
        for width, left_offset, scale in geometries:
            for side in range(-1024, 1025, 7):
                center = project_x(side, forward)
                left = center - projected_q12(left_offset * scale, sx)
                right = center + projected_q12(
                    (width - left_offset) * scale, sx) - 1
                touches = max(left, right) >= 0 and left < VIEW_W
                if touches and early_culled(side, forward, width, left_offset, scale):
                    raise ValueError("early frustum rejection lost an edge sprite")

    # Camera changes invalidate every measurement. Object movement and geometry
    # changes invalidate only that object's entry; enemy animation reuses fixed
    # geometry while each barrel-explosion shape gets a distinct key.
    enemy_key_0 = geometry_key(5, 2, 0)
    enemy_key_3 = geometry_key(5, 2, 3)
    barrel_key_0 = geometry_key(7, 19, 0)
    barrel_key_3 = geometry_key(7, 19, 3)
    if enemy_key_0 != enemy_key_3 or barrel_key_0 == barrel_key_3:
        raise ValueError("animation geometry cache key is over/under-invalidating")
    base = (4, 100, 200, enemy_key_0)
    if not cache_hit(base, base):
        raise ValueError("unchanged projection did not reuse its measurement")
    if (cache_hit(base, (5, 100, 200, enemy_key_0)) or
            cache_hit(base, (4, 101, 200, enemy_key_0)) or
            cache_hit(base, (4, 100, 200, geometry_key(5, 3, 0)))):
        raise ValueError("camera, movement, or geometry change reused stale projection")

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

    near_barrel = project_patch(192, CENTER_X, 23, 32, 10, 28)
    far_barrel = project_patch(384, CENTER_X, 23, 32, 10, 28)
    if not ((near_barrel[1] - near_barrel[0]) > (far_barrel[1] - far_barrel[0]) and
            (near_barrel[3] - near_barrel[2]) > (far_barrel[3] - far_barrel[2])):
        raise ValueError("native barrel size does not shrink with depth")
    if near_barrel[0] == CENTER_X - (near_barrel[1] - CENTER_X):
        raise ValueError("asymmetric Doom left offset was lost")
    if near_barrel[3] < near_barrel[4]:
        raise ValueError("barrel no longer extends below its Doom origin")
    if (near_barrel[1] - near_barrel[0] + 1) < 40:
        raise ValueError("barrel regressed to tiny 1:1 WAD geometry")
    barrel_w = near_barrel[1] - near_barrel[0] + 1
    barrel_h = near_barrel[3] - near_barrel[2] + 1
    if abs((barrel_h * 23) - (barrel_w * 32)) > 32:
        raise ValueError("barrel source aspect was crushed into a bucket")

    blue_key = project_patch(192, CENTER_X, 14, 16, 7, 19)
    if blue_key[3] >= blue_key[4]:
        raise ValueError("blue key top offset no longer lifts it above the floor origin")

    # Precise projected bounds, rather than a symmetric half-width, drive edge
    # clipping and span occlusion.
    edge_clip = project_patch(128, -4, 28, 19, 13, 19)
    edge_depths = [0x7FFF] * VIEW_W
    if edge_clip[1] < 0 or not span_visible(128, edge_clip[0], edge_clip[1], edge_depths):
        raise ValueError("partially on-screen item was incorrectly clipped")

    print("ok    native/fallback projection, conservative cull, cache invalidation, and span z-test")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(f"error {error}", file=sys.stderr)
        raise SystemExit(1)
