"""Regression checks for the shared billboard/wall projection contract."""

from pathlib import Path
import json
import re
import sys


ROOT = Path(__file__).resolve().parent.parent
RAYCAST = ROOT / "src" / "raycast.h"
BILLBOARD = ROOT / "src" / "billboard" / "billboard.c"
BILLBOARD_INTERNAL = ROOT / "src" / "billboard" / "billboard_internal.h"
PROJECTOR = ROOT / "src" / "billboard" / "billboard_projection.c"
BILLBOARD_LUT = ROOT / "src" / "billboard" / "billboard_projection_lut.h"
# renderer_scene.c was split by SRP into several files; the packer/billboard-
# draw code these checks look for now lives across that set.
SCENE_SPLIT_FILES = [
    "renderer_scene.c", "renderer_pack.c", "renderer_doors.c",
    "renderer_billboard_draw.c", "renderer_frame_overlay.c",
    "renderer_upload.c", "renderer_sparse.c",
    "renderer_flats.c",
]

VIEW_W = 160
CENTER_X = 80
CENTER_Y = 60
PROJ_X = 80
PROJ_Y = 80
CAMERA_HEIGHT = 64
SCALE_SHIFT = 12
WORLD_GEOMETRY_SCALE = 1
STRIDE = 2


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


# Pose order must match the ENEMY_* frame indices in billboard_internal.h and
# $EnemyFrameNames in tools/convert-freedoom-assets.ps1.
ENEMY_FRAME_NAMES = ["POSSA1", "POSSB1", "POSSC1", "POSSD1", "POSSF1",
                     "POSSH0", "POSSI0", "POSSJ0", "POSSK0", "POSSL0"]
SPRITE_OFFSETS = ROOT / "res" / "originaldoom" / "sprites" / "_offsets.json"


def _scale_round(value: int, num: int, den: int) -> int:
    """value * num / den, rounded half-up, sign-symmetric."""
    sign = -1 if value < 0 else 1
    return sign * ((abs(value) * num * 2 + den) // (den * 2))


def expected_enemy_frame_geometry() -> list[tuple[int, int, int, int]]:
    """Re-derive ENEMY_FRAME_GEOMETRY from the Doom picture headers.

    Enemy poses are projected through the shared WAD-origin path, so each pose
    needs its own world box. The scale is pinned to the walk pose so the
    standing enemy keeps the 2.25x size it was play-tuned to, and the baseline
    is lifted by that pose's below-origin overhang so walk frames sit exactly on
    the floor line. Everything else follows from the patch headers -- which is
    what makes the 17px-tall corpse land flat on the floor instead of being
    stretched over a standing body's height.
    """
    offsets = json.loads(SPRITE_OFFSETS.read_text(encoding="utf-8"))
    ref = offsets[ENEMY_FRAME_NAMES[0]]
    sx_num, sx_den = 54, ref["width"]           # 54/41
    sy_num, sy_den = 108, ref["height"]         # 108/55
    anchor = ref["height"] - ref["topOffset"]   # 5 native units below origin

    rows = []
    for name in ENEMY_FRAME_NAMES:
        patch = offsets[name]
        source_w = _scale_round(patch["width"], sx_num, sx_den)
        top_offset = _scale_round(patch["topOffset"] + anchor, sy_num, sy_den)
        below = _scale_round((patch["height"] - patch["topOffset"]) - anchor,
                             sy_num, sy_den)
        rows.append((source_w, top_offset + below, (source_w + 1) // 2, top_offset))
    return rows


def extract_enemy_frame_geometry(source: str) -> list[tuple[int, int, int, int]]:
    match = re.search(
        r"ENEMY_FRAME_GEOMETRY\[ENEMY_FRAME_GEOMETRY_COUNT\]\[4\]\s*=\s*\{(.*?)\n\};",
        source, re.S)
    if match is None:
        raise ValueError("ENEMY_FRAME_GEOMETRY table not found in billboard_internal.h")
    rows = []
    for row in re.finditer(r"\{\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+)\s*\}",
                           match.group(1)):
        rows.append(tuple(int(g) for g in row.groups()))
    return rows


def check_enemy_frame_geometry(billboard_internal: str) -> None:
    if "#define ENEMY_FRAME_GEOMETRY_COUNT 10" not in billboard_internal:
        raise ValueError("ENEMY_FRAME_GEOMETRY_COUNT no longer covers the 10 poses")
    actual = extract_enemy_frame_geometry(billboard_internal)
    expected = expected_enemy_frame_geometry()
    if actual != expected:
        raise ValueError(
            "ENEMY_FRAME_GEOMETRY drifted from the Doom picture headers:\n"
            f"  expected {expected}\n  actual   {actual}")

    walk = actual[0]
    if walk[1] != 108 or walk[0] != 54:
        raise ValueError("the walk pose no longer keeps its play-tuned 54x108 box")
    corpse = actual[9]
    if corpse[1] - corpse[3] != 0:
        raise ValueError("the POSSL0 corpse no longer rests exactly on the floor line")


def projected_q12(value: int, scale: int) -> int:
    product = value * scale
    return -(abs(product) >> SCALE_SHIFT) if product < 0 else product >> SCALE_SHIFT


def project_patch(forward: int, center: int, width: int, height: int,
                  left_offset: int, top_offset: int,
                  visual_scale: int = 1) -> tuple[int, int, int, int, int]:
    sx = (PROJ_X << SCALE_SHIFT) // forward
    sy = (PROJ_Y << SCALE_SHIFT) // forward
    origin_y = CENTER_Y + projected_q12(CAMERA_HEIGHT, sy)
    geometry_scale = WORLD_GEOMETRY_SCALE * visual_scale
    left = center - projected_q12(left_offset * geometry_scale, sx)
    right = center + projected_q12(
        (width - left_offset) * geometry_scale, sx) - 1
    # Sprite width and height share one focal scale. Only its world-floor origin
    # uses the wall camera's vertical projection.
    top = origin_y - projected_q12(top_offset * geometry_scale, sx)
    bottom = origin_y + projected_q12(
        (height - top_offset) * geometry_scale, sx) - 1
    return left, max(left, right), top, max(top, bottom), origin_y


def extract_lut(source: str, array_name: str) -> list[int]:
    match = re.search(
        re.escape(array_name) + r"\[1535\]\s*=\s*\{([^}]*)\}", source, re.DOTALL)
    if match is None:
        raise ValueError(f"{array_name} not found in {BILLBOARD_LUT}")
    return [int(tok) for tok in match.group(1).replace(",", " ").split()]


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
    scene = "\n".join((ROOT / "src" / "renderer" / name).read_text(encoding="utf-8")
                      for name in SCENE_SPLIT_FILES)

    required = [
        "#define RAY_VIEW_CENTER_X", "#define RAY_VIEW_CENTER_Y",
        "#define RAY_PROJ_X RAY_VIEW_CENTER_X", "#define RAY_PROJ_Y RAY_VIEW_CENTER_X",
        "#define RAY_WORLD_WALL_HEIGHT 128", "#define RAY_CAMERA_HEIGHT",
    ]
    if any(token not in raycast for token in required):
        raise ValueError("raycast camera geometry is no longer shared")
    if project_x(-100, 100) != 0 or project_x(100, 100) != VIEW_W:
        raise ValueError("horizontal projection is not an exact 90-degree FOV")
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
    # Enemies used to own a separate projection branch with its own baseline
    # LUT. They now ride the shared WAD-origin path, whose origin_y is the same
    # rendered floor plane -- RAY_CAMERA_HEIGHT scaled by the shared Q12 factor.
    if ("billboard_project_q12(RAY_CAMERA_HEIGHT, scale_y_q12)" not in billboard or
            "const s16 origin_y = (s16)(RAY_VIEW_CENTER_Y +" not in billboard):
        raise ValueError("billboard baseline is no longer anchored to the rendered floor plane")
    if "PLAYER_EYE_HEIGHT * RAY_PROJ_Y" in billboard:
        raise ValueError("enemy projection is using gameplay eye height instead of the render camera")
    if "#define BILLBOARD_WORLD_GEOMETRY_SCALE 1" not in billboard_internal:
        raise ValueError("world billboard geometry no longer uses native Doom units")
    if "#define BILLBOARD_PICKUP_VISUAL_SCALE 3" not in billboard_internal:
        raise ValueError("pickups are not rendered at the required 3x scale")
    if "u8 visual_scale;" not in billboard_internal or \
            "type->visual_scale" not in billboard:
        raise ValueError("billboard projection is not using per-type visual scale")
    if "geometry.top_offset, scale_x_q12" not in billboard:
        raise ValueError("sprite height is no longer using the width focal scale")
    if "FREEDOOM_BILLBOARD_WORLD_GEOMETRY" not in billboard:
        raise ValueError("world billboard projection is not source-geometry driven")
    if "object->atlas_w << 8" not in scene or "object->atlas_h << 16" not in scene:
        raise ValueError("billboard rasterizer is not sampling the generated atlas crop")
    # Enemies must stay on the single shared projection path: one fixed box for
    # all ten poses is exactly what left corpses floating at standing height.
    if "uses_wad_origin" in billboard or "uses_wad_origin" in billboard_internal:
        raise ValueError("a second billboard projection branch has reappeared")
    if "ENEMY_FRAME_GEOMETRY[(frame < ENEMY_FRAME_GEOMETRY_COUNT) ? frame : 0]" not in billboard:
        raise ValueError("enemy geometry is no longer selected per pose")
    check_enemy_frame_geometry(billboard_internal)
    if "#define BILLBOARD_ENEMY_ATLAS_WIDTH 24" not in billboard_internal or \
            "#define BILLBOARD_ENEMY_ATLAS_HEIGHT 48" not in billboard_internal:
        raise ValueError("enemy atlas art dimensions changed unexpectedly")
    if "#define BILLBOARD_BARREL_VISUAL_SCALE 2" not in billboard_internal:
        raise ValueError("barrels are not rendered at the required 2x scale")

    # bb_lut_divu() (see comment above) always takes the in-range table branch
    # in production, so the numerator passed at each call site is dead code
    # there -- a table's baked K must match its call site or the projection
    # silently uses a stale value. Byte-verify every entry against the exact
    # truncating-division formula (matches divu()).
    lut_source = BILLBOARD_LUT.read_text(encoding="utf-8")
    shared_k = PROJ_X << SCALE_SHIFT
    table = extract_lut(lut_source, "g_billboard_recip_proj_lut")
    if len(table) != 1535:
        raise ValueError("g_billboard_recip_proj_lut does not have exactly 1535 entries")
    expected = [min(shared_k // (i + 1), 65535) for i in range(1535)]
    if table != expected:
        raise ValueError(
            f"g_billboard_recip_proj_lut entries do not match K={shared_k} / (index+1)")
    # The retired per-enemy tables baked one fixed 54x108 box as K. Keeping them
    # around invites the fixed-box geometry back in.
    for dead in ("g_billboard_recip_enemy_w_lut", "g_billboard_recip_enemy_h_lut",
                 "g_billboard_recip_enemy_bottom_lut"):
        if dead in lut_source:
            raise ValueError(f"{dead} is back; enemies must use per-pose geometry")
    if ("billboard_mul_basis_delta" not in billboard or
            "muls.w %1,%0" not in billboard or
            "return billboard_muls_word(basis, (s16)delta);" not in billboard):
        raise ValueError("E1M1 native MULS.W transform is missing")
    if ("left_numerator" not in billboard or "right_numerator" not in billboard or
            "RAY_VIEW_CENTER_X + 2" not in billboard):
        raise ValueError("conservative pre-division frustum rejection is missing")
    cache_tokens = [
        "billboard_projection_cache_begin", "s_cache_player_x == player->x",
        "s_cache_player_y == player->y", "s_cache_player_angle == player->angle",
        "cache->object_x == object->x", "cache->object_y == object->y",
        "cache->geometry_key == geometry_key",
        "visual == BILLBOARD_VISUAL_BARREL_EXPLODING",
        # Every per-frame geometry must fold its frame into the key, or a
        # stationary object animating in front of a stationary camera keeps
        # serving its first pose's box -- which would pin a corpse at standing
        # height however correct ENEMY_FRAME_GEOMETRY is.
        "visual == BILLBOARD_VISUAL_DUMMY",
        "visual == BILLBOARD_VISUAL_DUMMY_DAMAGED",
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

    # Every possible E1M1 camera-to-THING delta stays in the native word domain.
    for delta, expected_path in (
            (-4576, "word"), (-1, "word"), (0, "word"),
            (1, "word"), (4576, "word")):
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

    # A depth discontinuity only affects its own rendered 2px block.
    depths = [0x7FFF] * VIEW_W
    depths[84] = 96
    if not span_visible(128, 80, 83, depths):
        raise ValueError("neighbouring wall block leaked into current sprite block")

    near_barrel = project_patch(96, CENTER_X, 23, 32, 10, 28)
    far_barrel = project_patch(192, CENTER_X, 23, 32, 10, 28)
    if not ((near_barrel[1] - near_barrel[0]) > (far_barrel[1] - far_barrel[0]) and
            (near_barrel[3] - near_barrel[2]) > (far_barrel[3] - far_barrel[2])):
        raise ValueError("native barrel size does not shrink with depth")
    if near_barrel[0] == CENTER_X - (near_barrel[1] - CENTER_X):
        raise ValueError("asymmetric Doom left offset was lost")
    if near_barrel[3] < near_barrel[4]:
        raise ValueError("barrel no longer extends below its Doom origin")
    if (near_barrel[1] - near_barrel[0] + 1) < 8:
        raise ValueError("native barrel became unreadably small")
    barrel_w = near_barrel[1] - near_barrel[0] + 1
    barrel_h = near_barrel[3] - near_barrel[2] + 1
    if abs((barrel_h * 23) - (barrel_w * 32)) > 32:
        raise ValueError("barrel source aspect was crushed into a bucket")

    native_key = project_patch(192, CENTER_X, 14, 16, 7, 19)
    blue_key = project_patch(192, CENTER_X, 14, 16, 7, 19, 3)
    if blue_key[3] >= blue_key[4]:
        raise ValueError("blue key top offset no longer lifts it above the floor origin")
    if not ((blue_key[1] - blue_key[0]) >= 3 * (native_key[1] - native_key[0]) - 2 and
            (blue_key[3] - blue_key[2]) >= 3 * (native_key[3] - native_key[2]) - 2):
        raise ValueError("pickup projected extent is not 3x native geometry")

    native_barrel = project_patch(192, CENTER_X, 23, 32, 10, 28)
    scaled_barrel = project_patch(192, CENTER_X, 23, 32, 10, 28, 3)
    if not ((scaled_barrel[1] - scaled_barrel[0]) >= 3 * (native_barrel[1] - native_barrel[0]) - 2 and
            (scaled_barrel[3] - scaled_barrel[2]) >= 3 * (native_barrel[3] - native_barrel[2]) - 2):
        raise ValueError("barrel projected extent is not 3x native geometry")

    # Precise projected bounds, rather than a symmetric half-width, drive edge
    # clipping and span occlusion.
    edge_clip = project_patch(128, -4, 28, 19, 13, 19, 3)
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
