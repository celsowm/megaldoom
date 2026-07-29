#!/usr/bin/env python3
"""Differential contracts for the native-word BSP renderer math."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GENERATED = (ROOT / "src/generated_e1m1_map.c").read_text()
RENDERER = (ROOT / "src/bsp_render.c").read_text()
BSP_MAP = (ROOT / "src/bsp_map.c").read_text()

FX_SHIFT = 8
FX_ONE = 1 << FX_SHIFT
ANGLE_90 = 64
NEAR = 16
PROJ_X = 80
VIEW_CENTER = 80
VIEW_COLS = 160
STRIDE = 2
LEFT_SCALE = VIEW_CENTER + STRIDE + 1
RIGHT_SCALE = VIEW_COLS + STRIDE - VIEW_CENTER


def declaration(typename, symbol):
    match = re.search(
        rf"const {typename} {symbol}\[\d+\] = \{{(.*?)\n\}};",
        GENERATED, re.S)
    assert match, symbol
    return match.group(1)


def rows(typename, symbol, count):
    result = []
    for row in re.findall(r"^\s*\{(.+)\},?$", declaration(
            typename, symbol), re.M):
        values = [int(value) for value in re.findall(r"-?\d+", row)]
        assert len(values) == count, (symbol, values)
        result.append(values)
    return result


def cdiv(numerator, denominator):
    assert denominator
    quotient = abs(numerator) // abs(denominator)
    return -quotient if (numerator < 0) != (denominator < 0) else quotient


def sin_quarter(angle):
    a = angle & (ANGLE_90 - 1)
    x = (a * FX_ONE) // ANGLE_90
    x2 = (x * x) >> FX_SHIFT
    x3 = (x2 * x) >> FX_SHIFT
    x5 = (x3 * x2) >> FX_SHIFT
    return ((402 * x) - (41 * x3) + (5 * x5)) >> FX_SHIFT


def fsin(angle):
    quadrant = (angle & 255) // ANGLE_90
    local = angle & (ANGLE_90 - 1)
    if quadrant == 0:
        return sin_quarter(local)
    if quadrant == 1:
        return sin_quarter(ANGLE_90 - 1 - local)
    if quadrant == 2:
        return -sin_quarter(local)
    return -sin_quarter(ANGLE_90 - 1 - local)


def perspective_divide(numerator, denominator):
    return cdiv(numerator, denominator)


def native_safe(px, py, bounds):
    min_x, min_y, max_x, max_y = bounds
    return (px >= max_x - 32767 and px <= min_x + 32768 and
            py >= max_y - 32767 and py <= min_y + 32768)


def render_mul(left, right, safe):
    if safe and -32768 <= left <= 32767 and -32768 <= right <= 32767:
        return left * right
    return left * right


def disk_stays_on_partition_side(cross, node_dx, node_dy, radius):
    """The runtime's L1 proof for a radius not crossing a BSP splitter."""
    return abs(cross) > radius * (abs(node_dx) + abs(node_dy))


def transform_box_reference(box, camera, basis):
    min_x, min_y, max_x, max_y = box
    px, py = camera
    fwx, fwy, rx, ry = basis
    points = ((min_x, min_y), (max_x, min_y),
              (max_x, max_y), (min_x, max_y))
    depths = []
    laterals = []
    for x, y in points:
        relx, rely = x - px, y - py
        depths.append(((relx * fwx) + (rely * fwy)) >> FX_SHIFT)
        laterals.append(((relx * rx) + (rely * ry)) >> FX_SHIFT)
    return depths, laterals


def transform_box_fast(box, camera, basis, safe):
    min_x, min_y, max_x, max_y = box
    px, py = camera
    fwx, fwy, rx, ry = basis
    relx, rely = min_x - px, min_y - py
    span_x, span_y = max_x - min_x, max_y - min_y
    depth = render_mul(relx, fwx, safe) + render_mul(rely, fwy, safe)
    lateral = render_mul(relx, rx, safe) + render_mul(rely, ry, safe)
    depth_dx = render_mul(span_x, fwx, safe)
    lateral_dx = render_mul(span_x, rx, safe)
    depth_dy = render_mul(span_y, fwy, safe)
    lateral_dy = render_mul(span_y, ry, safe)
    depths = [depth >> FX_SHIFT,
              (depth + depth_dx) >> FX_SHIFT,
              (depth + depth_dx + depth_dy) >> FX_SHIFT,
              (depth + depth_dy) >> FX_SHIFT]
    laterals = [lateral >> FX_SHIFT,
                (lateral + lateral_dx) >> FX_SHIFT,
                (lateral + lateral_dx + lateral_dy) >> FX_SHIFT,
                (lateral + lateral_dy) >> FX_SHIFT]
    return depths, laterals


def box_parts(box, camera, basis, safe):
    """Q8 base + extent terms, mirroring the C corner decomposition inputs."""
    min_x, min_y, max_x, max_y = box
    px, py = camera
    fwx, fwy, rx, ry = basis
    relx, rely = min_x - px, min_y - py
    span_x, span_y = max_x - min_x, max_y - min_y
    depth = render_mul(relx, fwx, safe) + render_mul(rely, fwy, safe)
    lateral = render_mul(relx, rx, safe) + render_mul(rely, ry, safe)
    return (depth, lateral,
            render_mul(span_x, fwx, safe), render_mul(span_x, rx, safe),
            render_mul(span_y, fwy, safe), render_mul(span_y, ry, safe))


def decomposed_extrema(parts):
    """Exact corner extrema via the monotonic-shift decomposition (new C)."""
    depth, lateral, ddx, ldx, ddy, ldy = parts
    return ((depth + min(ddx, 0) + min(ddy, 0)) >> FX_SHIFT,
            (depth + max(ddx, 0) + max(ddy, 0)) >> FX_SHIFT,
            (lateral + min(ldx, 0) + min(ldy, 0)) >> FX_SHIFT,
            (lateral + max(ldx, 0) + max(ldy, 0)) >> FX_SHIFT)


def cheap_reject_old(depths, laterals):
    """Per-assembled-corner half-plane reject (the old C loop)."""
    left = max(PROJ_X * l + LEFT_SCALE * d for d, l in zip(depths, laterals))
    right = min(PROJ_X * l - RIGHT_SCALE * d for d, l in zip(depths, laterals))
    return left <= 0 or right >= 0


def cheap_reject_new(parts):
    """Axis-decomposed shifted-domain half-plane reject with the +-2-per-corner
    floor slack (the new C). Must only reject when cheap_reject_old rejects."""
    depth, lateral, ddx, ldx, ddy, ldy = parts
    d0, l0 = depth >> FX_SHIFT, lateral >> FX_SHIFT
    sdx, slx = ddx >> FX_SHIFT, ldx >> FX_SHIFT
    sdy, sly = ddy >> FX_SHIFT, ldy >> FX_SHIFT
    left_dx = PROJ_X * slx + LEFT_SCALE * sdx
    left_dy = PROJ_X * sly + LEFT_SCALE * sdy
    max_left = (PROJ_X * l0 + LEFT_SCALE * d0 +
                2 * (PROJ_X + LEFT_SCALE) + max(left_dx, 0) + max(left_dy, 0))
    if max_left <= 0:
        return True
    right_dx = PROJ_X * slx - RIGHT_SCALE * sdx
    right_dy = PROJ_X * sly - RIGHT_SCALE * sdy
    min_right = (PROJ_X * l0 - RIGHT_SCALE * d0 -
                 2 * RIGHT_SCALE + min(right_dx, 0) + min(right_dy, 0))
    return min_right >= 0


def projected_range(depths, laterals):
    min_depth, max_depth = min(depths), max(depths)
    if max_depth < NEAR:
        return None
    if min_depth < NEAR:
        screens = []
        for i in range(4):
            j = (i + 1) & 3
            if depths[i] >= NEAR:
                screens.append(VIEW_CENTER + perspective_divide(
                    laterals[i] * PROJ_X, depths[i]))
            if (depths[i] < NEAR) != (depths[j] < NEAR):
                t = perspective_divide((NEAR - depths[i]) << FX_SHIFT,
                                       depths[j] - depths[i])
                lateral = laterals[i] + (((laterals[j] - laterals[i]) * t) >> FX_SHIFT)
                screens.append(VIEW_CENTER + perspective_divide(
                    lateral * PROJ_X, NEAR))
        if not screens:
            return None
    else:
        left_planes = [PROJ_X * lat + LEFT_SCALE * depth
                       for depth, lat in zip(depths, laterals)]
        right_planes = [PROJ_X * lat - RIGHT_SCALE * depth
                        for depth, lat in zip(depths, laterals)]
        if max(left_planes) <= 0 or min(right_planes) >= 0:
            return None
        screens = [VIEW_CENTER + perspective_divide(lat * PROJ_X, depth)
                   for depth, lat in zip(depths, laterals)]
    left, right = min(screens) - STRIDE, max(screens) + STRIDE
    if right < 0 or left >= VIEW_COLS:
        return None
    return max(left, 0), min(right, VIEW_COLS - 1)


def segment_span(seg, vertices, camera, basis, safe):
    v1, v2, nx, ny = seg[:4]
    px, py = camera
    a, b = vertices[v1], vertices[v2]
    if render_mul(px - a[0], nx, safe) + render_mul(py - a[1], ny, safe) <= 0:
        return None

    transformed = []
    for x, y in (a, b):
        relx, rely = x - px, y - py
        depth = (render_mul(relx, basis[0], safe) +
                 render_mul(rely, basis[1], safe)) >> FX_SHIFT
        lateral = (render_mul(relx, basis[2], safe) +
                   render_mul(rely, basis[3], safe)) >> FX_SHIFT
        transformed.append([depth, lateral])
    (depth_a, lat_a), (depth_b, lat_b) = transformed
    if depth_a < NEAR and depth_b < NEAR:
        return None
    if depth_a < NEAR:
        t = perspective_divide((NEAR - depth_a) << FX_SHIFT, depth_b - depth_a)
        lat_a += ((lat_b - lat_a) * t) >> FX_SHIFT
        depth_a = NEAR
    elif depth_b < NEAR:
        t = perspective_divide((NEAR - depth_b) << FX_SHIFT, depth_a - depth_b)
        lat_b += ((lat_a - lat_b) * t) >> FX_SHIFT
        depth_b = NEAR
    xa = VIEW_CENTER + perspective_divide(lat_a * PROJ_X, depth_a)
    xb = VIEW_CENTER + perspective_divide(lat_b * PROJ_X, depth_b)
    if xa == xb:
        return None
    left, right = sorted((xa, xb))
    x0, x1 = max(left, 0), min(right - 1, VIEW_COLS - 1)
    if x0 > x1:
        return None
    first = (x0 + STRIDE - 1) // STRIDE
    last = x1 // STRIDE
    return None if first > last else (first, last)


def constant(name):
    match = re.search(rf"const s16 {name} = (-?\d+);", GENERATED)
    assert match, name
    return int(match.group(1))


def main():
    vertices = [tuple(row) for row in rows("BspVertex", "bsp_vertices", 2)]
    segments = rows("BspSeg", "bsp_segs", 11)
    nodes = rows("BspNode", "bsp_nodes", 14)
    bounds = (constant("bsp_map_min_x"), constant("bsp_map_min_y"),
              constant("bsp_map_max_x"), constant("bsp_map_max_y"))
    assert bounds == (min(x for x, _ in vertices), min(y for _, y in vertices),
                      max(x for x, _ in vertices), max(y for _, y in vertices))

    cameras = [(1056, 3616),
               ((bounds[0] + bounds[2]) // 2, (bounds[1] + bounds[3]) // 2),
               (bounds[0] + 16, bounds[1] + 16),
               (bounds[2] - 16, bounds[3] - 16),
               (bounds[0] - 40000, bounds[1] - 40000)]
    assert all(native_safe(*camera, bounds) for camera in cameras[:-1])
    assert not native_safe(*cameras[-1], bounds)

    box_checks = 0
    segment_checks = 0
    for camera in cameras:
        safe = native_safe(*camera, bounds)
        for angle in range(256):
            fwy = fsin(angle)
            fwx = fsin(angle + ANGLE_90)
            basis = (fwx, fwy, -fwy, fwx)
            for node in nodes:
                for box in (node[4:8], node[8:12]):
                    reference = transform_box_reference(box, camera, basis)
                    fast = transform_box_fast(box, camera, basis, safe)
                    assert fast == reference, (camera, angle, box, fast, reference)
                    assert projected_range(*fast) == projected_range(*reference)
                    parts = box_parts(box, camera, basis, safe)
                    depths, laterals = reference
                    # The loop-free corner extrema must be exactly the extrema
                    # of the four assembled corners.
                    assert decomposed_extrema(parts) == (
                        min(depths), max(depths),
                        min(laterals), max(laterals)), (camera, angle, box)
                    # The slackened decomposed half-plane test may only reject
                    # boxes the exact per-corner test also rejected (the C's
                    # cheap-reject path only runs when all corners are in front
                    # of the near plane, so restrict the check to that case).
                    if min(depths) >= NEAR and cheap_reject_new(parts):
                        assert cheap_reject_old(depths, laterals), (
                            camera, angle, box)
                    box_checks += 1
            # Cover every seg for representative cardinal/intercardinal angles;
            # all 256 angles above already exercise every box and near-plane case.
            if (angle & 31) == 0:
                for seg in segments:
                    fast_span = segment_span(seg, vertices, camera, basis, safe)
                    reference_span = segment_span(seg, vertices, camera, basis, False)
                    assert fast_span == reference_span
                    segment_checks += 1

    assert "render_mul" in RENDERER
    assert "depth_dx_q8" in RENDERER and "lateral_dy_q8" in RENDERER
    assert "laterals[i] * depths" not in RENDERER
    # The shifted-domain cheap reject must carry its floor-slack margins.
    assert "2 * (RAY_PROJ_X + LEFT_REJECT_SCALE)" in RENDERER
    assert "2 * RIGHT_REJECT_SCALE" in RENDERER
    # A visible-subsector cull may only skip a sprite when its full horizontal
    # footprint cannot cross any partition on the way to its leaf. The runtime
    # uses the cheap L1 upper bound on the splitter norm; prove every accepted
    # sample is also outside the exact Euclidean-radius strip.
    for px, py in vertices + cameras[:-1]:
        for node in nodes:
            nx, ny, ndx, ndy = node[:4]
            cross = (px - nx) * ndy - (py - ny) * ndx
            for radius in (0, 1, 24, 96):
                if disk_stays_on_partition_side(cross, ndx, ndy, radius):
                    assert cross * cross > radius * radius * (ndx * ndx + ndy * ndy)
    assert "bsp_find_subsector_with_margin" in BSP_MAP
    assert "radius * (abs_node_dx + abs_node_dy)" in BSP_MAP
    print(f"ok    BSP native math: {box_checks} boxes, {segment_checks} seg spans")


if __name__ == "__main__":
    main()
