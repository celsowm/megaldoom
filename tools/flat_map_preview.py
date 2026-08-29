#!/usr/bin/env python3
"""Render the flattened E1M1 alternatives before replacing generated files.

This is deliberately an offline verifier, not a second game renderer.  It uses
the shipped fixed-point basis, 160x120 projection, near clipping, stride 2,
perspective-correct wall U, 64x128 wall sampling, converted textures, PAL3 and
the four-level shade LUT.  The three profiles are rendered side by side:

* the currently checked-in generated map;
* a clean conversion of the current WAD with recipes disabled;
* the current WAD with a structurally resolved material transfer.

The renderer also retains the winning source linedef and depth for every sampled
column, which lets tests prove the recipe changes material only: it cannot add
occlusion, collision or LOS geometry.
"""

import argparse
from dataclasses import dataclass
from pathlib import Path
import re

from PIL import Image, ImageDraw, ImageFont

import doom_map
from flat_map_recipes import PREVIEW_POSES
from wad_reader import WadFile
import world_assets


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_WAD = ROOT / "DOOM1.WAD"
DEFAULT_MAP_SOURCE = ROOT / "src" / "bsp" / "generated_e1m1_map.c"
DEFAULT_ASSET_SOURCE = ROOT / "src" / "bsp" / "generated_assets.h"
DEFAULT_OUTPUT = ROOT / "out" / "flat-map-preview"

VIEW_W = 160
VIEW_H = 120
VIEW_CENTER_X = 80
PROJ_X = 80
PROJ_Y = 80
WALL_HEIGHT = 128
STRIDE = 2
NEAR = 16
FX_SHIFT = 8
FX_ONE = 1 << FX_SHIFT
INV_SCALE = 1 << 14
CEILING_INDEX = world_assets.GLOBAL_CEILING_INDEX
FLOOR_INDEX = world_assets.GLOBAL_FLOOR_INDEX


@dataclass
class PreviewSeg:
    v1: int
    v2: int
    nx: int
    ny: int
    texture_name: str
    tex_u: int
    tex_v: int
    source_linedef: int | None = None
    curated_material: bool = False


@dataclass
class PreviewProfile:
    name: str
    vertices: list
    segs: list


@dataclass
class RenderResult:
    image: Image.Image
    source_linedefs: list
    material_columns: set
    depths: list


def cdiv(numerator, denominator):
    """C/68000 signed division: truncate toward zero."""
    if denominator == 0:
        raise ZeroDivisionError
    quotient = abs(numerator) // abs(denominator)
    return -quotient if (numerator < 0) != (denominator < 0) else quotient


def sin_quarter(angle):
    a = angle & 63
    x = (a * FX_ONE) // 64
    x2 = (x * x) >> FX_SHIFT
    x3 = (x2 * x) >> FX_SHIFT
    x5 = (x3 * x2) >> FX_SHIFT
    # Bit-identical to src/fixed_math.c, including the intentional 1.1839 gain.
    return ((479 * x) - (196 * x3) + (24 * x5)) >> FX_SHIFT


def fsin(angle):
    quadrant = (angle & 255) // 64
    local = angle & 63
    if quadrant == 0:
        return sin_quarter(local)
    if quadrant == 1:
        return sin_quarter(63 - local)
    if quadrant == 2:
        return -sin_quarter(local)
    return -sin_quarter(63 - local)


def fcos(angle):
    return fsin((angle + 64) & 255)


def _declaration(text, typename, symbol):
    match = re.search(
        rf"const\s+{typename}\s+{symbol}\[\d+\]\s*=\s*\{{(.*?)\n\}};",
        text, re.S)
    if not match:
        raise ValueError("Missing generated declaration %s" % symbol)
    return match.group(1)


def _rows(text, typename, symbol, count):
    result = []
    for row in re.findall(r"^\s*\{(.+?)\},?\s*$",
                          _declaration(text, typename, symbol), re.M):
        values = [int(value) for value in re.findall(r"-?\d+", row)]
        if len(values) != count:
            raise ValueError("Malformed %s row: %s" % (symbol, values))
        result.append(values)
    return result


def load_versioned_profile(map_source=DEFAULT_MAP_SOURCE,
                           asset_source=DEFAULT_ASSET_SOURCE):
    """Parse the checked-in C artifact without regenerating it."""
    map_text = Path(map_source).read_text(encoding="utf-8")
    asset_text = Path(asset_source).read_text(encoding="utf-8")
    texture_names = {}
    for macro, value in re.findall(
            r"^#define\s+MEGALDOOM_TEX_([A-Z0-9_]+)\s+(\d+)\s*$",
            asset_text, re.M):
        texture_names[int(value)] = (world_assets.FALLBACK_TEXTURE
                                     if macro == "FALLBACK" else macro)
    vertices = [tuple(row) for row in _rows(
        map_text, "BspVertex", "bsp_vertices", 2)]
    segs = []
    for row in _rows(map_text, "BspSeg", "bsp_segs", 11):
        v1, v2, nx, ny, tex_u, tex_v, texture_id = row[:7]
        segs.append(PreviewSeg(
            v1, v2, nx, ny, texture_names[texture_id], tex_u, tex_v))
    return PreviewProfile("versioned artifact", vertices, segs)


def profile_from_map(name, map_data):
    segs = []
    for source in map_data.out_segs:
        texture_name = source["texture_name"]
        with Image.open(world_assets.texture_path(texture_name)) as image:
            texture_height = image.height
        tex_v = source["tex_v_offset"] % texture_height
        segs.append(PreviewSeg(
            source["v1"], source["v2"], source["nx"], source["ny"],
            texture_name, source["tex_u_offset"], tex_v,
            source["source_linedef"], source["curated_material"]))
    return PreviewProfile(name, map_data.vertices, segs)


def build_texture_bank(profiles, palette, use_wall_bake_recipe=True):
    names = sorted({seg.texture_name for profile in profiles for seg in profile.segs})
    bank = {}
    scales = {}
    for name in names:
        path = world_assets.texture_path(name)
        with Image.open(path) as image:
            sampled_width, _ = world_assets.sampled_texture_dimensions(
                name, image.width, image.height)
        bank[name] = world_assets.convert_texture(
            path, palette, use_wall_bake_recipe=use_wall_bake_recipe)
        scales[name] = world_assets.texture_u_scale_q12(sampled_width)
    return bank, scales


def _clip_endpoint(depth_near, lateral_near, u_near,
                   depth_far, lateral_far, u_far):
    t = cdiv((NEAR - depth_near) << FX_SHIFT, depth_far - depth_near)
    lateral_near += ((lateral_far - lateral_near) * t) >> FX_SHIFT
    u_near += ((u_far - u_near) * t) >> FX_SHIFT
    return NEAR, lateral_near, u_near


def _shade_level(depth, ny):
    return min(3, (depth >> 9) + (1 if ny != 0 else 0))


def render_profile(profile, pose, textures, u_scales, palette, shade_lut):
    """Render one wall-only frame and preserve per-column provenance."""
    _, px, py, angle = pose
    fwx, fwy = fcos(angle), fsin(angle)
    rx, ry = -fwy, fwx
    columns = [None] * (VIEW_W // STRIDE)

    for seg_id, seg in enumerate(profile.segs):
        ax, ay = profile.vertices[seg.v1]
        bx, by = profile.vertices[seg.v2]
        if (px - ax) * seg.nx + (py - ay) * seg.ny <= 0:
            continue

        rel_ax, rel_ay = ax - px, ay - py
        rel_bx, rel_by = bx - px, by - py
        depth_a = (rel_ax * fwx + rel_ay * fwy) >> FX_SHIFT
        lat_a = (rel_ax * rx + rel_ay * ry) >> FX_SHIFT
        depth_b = (rel_bx * fwx + rel_by * fwy) >> FX_SHIFT
        lat_b = (rel_bx * rx + rel_by * ry) >> FX_SHIFT
        u_a = seg.tex_u
        u_b = seg.tex_u + abs(bx - ax) + abs(by - ay)

        if depth_a < NEAR and depth_b < NEAR:
            continue
        if depth_a < NEAR:
            depth_a, lat_a, u_a = _clip_endpoint(
                depth_a, lat_a, u_a, depth_b, lat_b, u_b)
        elif depth_b < NEAR:
            depth_b, lat_b, u_b = _clip_endpoint(
                depth_b, lat_b, u_b, depth_a, lat_a, u_a)

        xa = VIEW_CENTER_X + cdiv(lat_a * PROJ_X, depth_a)
        xb = VIEW_CENTER_X + cdiv(lat_b * PROJ_X, depth_b)
        if xa == xb:
            continue
        if xa < xb:
            x_left, x_right = xa, xb
            depth_left, depth_right = depth_a, depth_b
            u_left, u_right = u_a, u_b
        else:
            x_left, x_right = xb, xa
            depth_left, depth_right = depth_b, depth_a
            u_left, u_right = u_b, u_a

        x0 = max(0, x_left)
        x1 = min(VIEW_W - 1, x_right - 1)
        if x0 > x1:
            continue
        first_sample = (x0 + STRIDE - 1) // STRIDE
        last_sample = x1 // STRIDE
        span = x_right - x_left
        invz_left = INV_SCALE // depth_left
        invz_right = INV_SCALE // depth_right
        uz_left = u_left * invz_left
        uz_right = u_right * invz_right
        inv_span = (FX_ONE << FX_SHIFT) // span

        for sample in range(first_sample, last_sample + 1):
            x = sample * STRIDE
            sfix = 0 if span == 1 else (((x - x_left) * inv_span) >> FX_SHIFT)
            invz = invz_left + (((invz_right - invz_left) * sfix) >> FX_SHIFT)
            if invz <= 0:
                continue
            depth = INV_SCALE // invz
            uz = uz_left + (((uz_right - uz_left) * sfix) >> FX_SHIFT)
            u = cdiv(uz, invz)
            scaled_u = (u * u_scales[seg.texture_name]) >> 12
            prior = columns[sample]
            if prior is None or depth < prior[0]:
                projected_height = max(1,
                    (PROJ_Y * WALL_HEIGHT * invz) >> 14)
                height = min(VIEW_H, projected_height)
                columns[sample] = (
                    depth, height, projected_height,
                    scaled_u & (world_assets.WALL_TEX_WIDTH - 1),
                    seg, seg_id)

    pixels = [[CEILING_INDEX if y < VIEW_H // 2 else FLOOR_INDEX
               for _ in range(VIEW_W)] for y in range(VIEW_H)]
    sources = [None] * (VIEW_W // STRIDE)
    depths = [0x7FFF] * (VIEW_W // STRIDE)
    material_columns = set()
    for sample, column in enumerate(columns):
        if column is None:
            continue
        depth, height, projected_height, tex_x, seg, _ = column
        top = (VIEW_H - height) // 2
        bottom = top + height
        clip_offset = (projected_height - height) // 2
        level = _shade_level(depth, seg.ny)
        texture = textures[seg.texture_name]
        with Image.open(world_assets.texture_path(seg.texture_name)) as image:
            _, source_height = world_assets.sampled_texture_dimensions(
                seg.texture_name, image.width, image.height)
        v_scale = world_assets.texture_v_scale_q12(source_height)
        for y in range(top, bottom):
            canonical_y = ((y - top + clip_offset) *
                           world_assets.WALL_TEX_HEIGHT) // projected_height
            tex_y = ((canonical_y * v_scale) >> 12) + seg.tex_v
            if tex_y >= source_height:
                tex_y -= source_height
            color = shade_lut[level][texture[tex_y][tex_x] & 0x0F]
            x = sample * STRIDE
            pixels[y][x] = color
            pixels[y][x + 1] = color
        sources[sample] = seg.source_linedef
        depths[sample] = depth
        if seg.curated_material:
            material_columns.add(sample)

    image = Image.new("RGB", (VIEW_W, VIEW_H))
    image.putdata([palette[index] for row in pixels for index in row])
    return RenderResult(image, sources, material_columns, depths)


def assert_candidate_is_geometry_neutral(clean, candidate, pose_name):
    """Enforce zero geometry/occlusion drift before generation."""
    if clean.source_linedefs != candidate.source_linedefs:
        raise AssertionError("%s recipe changed winning wall geometry" % pose_name)
    if clean.depths != candidate.depths:
        raise AssertionError("%s recipe changed wall depth/occlusion" % pose_name)
    if (not candidate.material_columns and
            pose_name in ("spawn", "approach", "entry", "beside")):
        raise AssertionError("%s preview does not show transferred material" % pose_name)
    clean_pixels = clean.image.load()
    candidate_pixels = candidate.image.load()
    for sample in range(VIEW_W // STRIDE):
        if sample in candidate.material_columns:
            continue
        x = sample * STRIDE
        for y in range(VIEW_H):
            if (clean_pixels[x, y] != candidate_pixels[x, y] or
                    clean_pixels[x + 1, y] != candidate_pixels[x + 1, y]):
                raise AssertionError(
                    "%s candidate changed non-transfer sample %d at y=%d" %
                    (pose_name, sample, y))


def _labeled_frame(result, label, scale=3):
    frame = result.image.resize((VIEW_W * scale, VIEW_H * scale),
                                Image.Resampling.NEAREST)
    label_h = 24
    output = Image.new("RGB", (frame.width, frame.height + label_h), "black")
    output.paste(frame, (0, label_h))
    draw = ImageDraw.Draw(output)
    draw.text((8, 5), label, fill="white", font=ImageFont.load_default())
    return output


def make_comparison(results, pose_name):
    panels = [_labeled_frame(result, label) for label, result in results]
    gap = 3
    output = Image.new("RGB", (sum(panel.width for panel in panels) + gap * 2,
                               panels[0].height), (160, 0, 24))
    x = 0
    for panel in panels:
        output.paste(panel, (x, 0))
        x += panel.width + gap
    draw = ImageDraw.Draw(output)
    draw.text((8, output.height - 16), pose_name, fill=(255, 255, 0),
              font=ImageFont.load_default())
    return output


def make_plan_view(clean_profile, candidate_profile, poses):
    """Focused first-room plan: erased source orange, destinations green."""
    bounds = (384, 2820, 1504, 3776)
    width, height = 720, 620
    margin = 36
    image = Image.new("RGB", (width, height), (18, 18, 18))
    draw = ImageDraw.Draw(image)
    x0, y0, x1, y1 = bounds

    def point(x, y):
        sx = margin + int((x - x0) * (width - 2 * margin) / (x1 - x0))
        sy = margin + int((y - y0) * (height - 2 * margin) / (y1 - y0))
        return sx, sy

    clean_lines = {seg.source_linedef for seg in clean_profile.segs}
    candidate_by_line = {}
    for seg in candidate_profile.segs:
        candidate_by_line.setdefault(seg.source_linedef, seg)
    source_vertices = candidate_profile.vertices
    # Draw every source linedef so erased height geometry remains auditable.
    # Solid flattened lines are white; open height-only lines are dark grey.
    map_data = getattr(candidate_profile, "map_data", None)
    if map_data is not None:
        for line_id, linedef in enumerate(map_data.linedefs):
            a = source_vertices[linedef["v1"]]
            b = source_vertices[linedef["v2"]]
            if not (x0 <= a[0] <= x1 and y0 <= a[1] <= y1 and
                    x0 <= b[0] <= x1 and y0 <= b[1] <= y1):
                continue
            color = (116, 116, 116) if line_id not in clean_lines else (218, 218, 218)
            candidate_seg = candidate_by_line.get(line_id)
            if line_id == 50:
                color = (255, 170, 48)
            elif candidate_seg is not None and candidate_seg.curated_material:
                color = (64, 230, 128)
            width_px = 5 if (line_id == 50 or
                             (candidate_seg is not None and
                              candidate_seg.curated_material)) else 2
            draw.line((*point(*a), *point(*b)), fill=color, width=width_px)
            if line_id in (37, 40, 41, 42, 48, 49, 50, 52, 53):
                mx = (a[0] + b[0]) // 2
                my = (a[1] + b[1]) // 2
                draw.text(point(mx, my), "L%d" % line_id,
                          fill=(255, 210, 64), font=ImageFont.load_default())

    for name, x, y, angle in poses:
        p = point(x, y)
        draw.ellipse((p[0] - 5, p[1] - 5, p[0] + 5, p[1] + 5),
                     fill=(64, 220, 255))
        end = point(x + cdiv(fcos(angle) * 72, FX_ONE),
                    y + cdiv(fsin(angle) * 72, FX_ONE))
        draw.line((*p, *end), fill=(64, 220, 255), width=2)
        draw.text((p[0] + 7, p[1] - 8), name, fill=(64, 220, 255),
                  font=ImageFont.load_default())
    draw.text((margin, 10),
              "E1M1: orange=erased upper band, green=solid COMPUTE2 destinations",
              fill="white", font=ImageFont.load_default())
    return image


def build_preview(wad_path=DEFAULT_WAD, mapn="E1M1",
                  output_dir=DEFAULT_OUTPUT,
                  versioned_map=DEFAULT_MAP_SOURCE,
                  versioned_assets=DEFAULT_ASSET_SOURCE):
    wad = WadFile(str(wad_path))
    clean_data = doom_map.load_map(wad, mapn, apply_recipes=False)
    candidate_data = doom_map.load_map(wad, mapn, apply_recipes=True)
    versioned = load_versioned_profile(versioned_map, versioned_assets)
    clean = profile_from_map("current WAD / no recipe", clean_data)
    candidate = profile_from_map("current WAD / perimeter material transfer",
                                 candidate_data)
    # Keep parsed source geometry available to the plan renderer without making
    # runtime-oriented PreviewProfile depend on all of doom_map.MapData.
    candidate.map_data = candidate_data
    profiles = (versioned, clean, candidate)

    palette = list(world_assets.FROZEN_WORLD_PALETTE)
    textures, u_scales = build_texture_bank(profiles, palette)
    shade_lut = world_assets.build_shade_lut(
        palette, 4, (CEILING_INDEX, FLOOR_INDEX))
    poses = PREVIEW_POSES[mapn]
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    output_paths = []
    for pose in poses:
        rendered = [render_profile(profile, pose, textures, u_scales,
                                   palette, shade_lut)
                    for profile in profiles]
        assert_candidate_is_geometry_neutral(rendered[1], rendered[2], pose[0])
        comparison = make_comparison(list(zip(
            (profile.name for profile in profiles), rendered)), pose[0])
        output_path = output_dir / (pose[0] + ".png")
        comparison.save(output_path)
        output_paths.append(output_path)

    plan = make_plan_view(clean, candidate, poses)
    plan_path = output_dir / "first-room-plan.png"
    plan.save(plan_path)
    output_paths.append(plan_path)
    return clean_data, candidate_data, output_paths


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--wad", type=Path, default=DEFAULT_WAD)
    parser.add_argument("--map", default="E1M1")
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--versioned-map", type=Path, default=DEFAULT_MAP_SOURCE)
    parser.add_argument("--versioned-assets", type=Path, default=DEFAULT_ASSET_SOURCE)
    args = parser.parse_args()
    clean, candidate, paths = build_preview(
        args.wad, args.map.upper(), args.out_dir,
        args.versioned_map, args.versioned_assets)
    print("Preview certified before generation")
    print("  source   : SHA-256 %s" % candidate.wad_sha256)
    print("  baseline : %d SEGs" % len(clean.out_segs))
    print("  candidate: %d SEGs (%d retextured, +0)" %
          (len(candidate.out_segs), candidate.curated_material_segs))
    for path in paths:
        print("  image    : %s" % path)


if __name__ == "__main__":
    main()
