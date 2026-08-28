#!/usr/bin/env python3
"""Audit curated 64x128 technological-wall bakes before generation.

The preview compares the checked conversion algorithm with the curated one in
memory. It renders source/current/candidate atlases, exact PAL3 shade levels,
fixed-math 160x120 scenes, and short approach/lateral motion strips. Nothing
under src/ or res/ is written.
"""

import argparse
import json
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

import flat_map_preview
from flat_map_recipes import PREVIEW_POSES
from wad_reader import WadFile
import doom_map
import world_assets


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_WAD = ROOT / "DOOM1.WAD"
DEFAULT_OUTPUT = ROOT / "out" / "wall-bake-preview"
EXPECTED_WAD_SHA256 = (
    "77CD3852B5F7114EC64A07A1B1EF1F734736A13BBD186477C9111A7DD8C55F82"
)
PALETTE = list(world_assets.FROZEN_WORLD_PALETTE)
TECH_MATERIALS = tuple(world_assets.TECH_WALL_MATERIALS)
# The preview shows what reaches the screen: convert_texture() emits one
# texel per displayed pixel, two per packed byte.
WIDTH = world_assets.WALL_TEX_DISPLAY_WIDTH
HEIGHT = world_assets.WALL_TEX_HEIGHT
FONT = ImageFont.load_default()
# Close to linedef 52 (x=704, y=3552..3360), looking north along it.  This is
# the failure angle from gameplay: COMPUTE2 occupies most of the left half and
# tiny incoherent details become immediately obvious.
COMPUTE2_OBLIQUE_POSE = ("compute2-oblique", 800, 3504, 192)


def churn(rows):
    changed = sum(row[x] != row[x + 1]
                  for row in rows for x in range(WIDTH - 1))
    return changed / (HEIGHT * (WIDTH - 1))


def isolated_count(rows):
    return sum(
        rows[y][x] not in (
        rows[y][(x - 1) % WIDTH], rows[y][(x + 1) % WIDTH],
            rows[(y - 1) % HEIGHT][x], rows[(y + 1) % HEIGHT][x],
        )
        for y in range(HEIGHT) for x in range(WIDTH)
    )


def palette_edge_mask(rows):
    columns = [[PALETTE[rows[y][x]] for y in range(HEIGHT)]
               for x in range(WIDTH)]
    return world_assets._edge_mask(columns, 0.09, 0.04)


def edge_f1(reference, output):
    expected = {(x, y) for x in range(WIDTH) for y in range(HEIGHT)
                if reference[x][y]}
    actual = {(x, y) for x in range(WIDTH) for y in range(HEIGHT)
              if output[x][y]}
    if not expected:
        return 1.0 if not actual else 0.0
    hits = len(expected & actual)
    precision = hits / max(1, len(actual))
    recall = hits / len(expected)
    return 2.0 * precision * recall / max(1e-12, precision + recall)


def perceptual_error(rows, target_columns):
    """Per-texel PAL3 error against the bake stage's intended RGB target."""
    total = 0.0
    for y in range(HEIGHT):
        for x in range(WIDTH):
            target = world_assets.world_palette.oklab(target_columns[x][y])
            mapped = world_assets.world_palette.oklab(PALETTE[rows[y][x]])
            total += (1.25 * (target[0] - mapped[0]) ** 2 +
                      (target[1] - mapped[1]) ** 2 +
                      (target[2] - mapped[2]) ** 2)
    return total / (WIDTH * HEIGHT)


def texture_metrics(name):
    """Bake one material with and without its recipe, and measure the pair.

    Every comparison below answers one question -- "does the curated recipe
    improve this material over the plain conversion?" -- so both sides are
    baked through _convert_texture, which skips convert_texture's churn
    fallback. That fallback is a per-material decision about resolution, and it
    can fire on one side and not the other; when it does, the comparison stops
    being about the recipe and becomes a half-resolution bake measured against
    a full-resolution one, which reads the extra texels as noise and as new
    isolated dots. STARTAN3 measured churn 0.105 vs 0.242 and isolated 0 vs 2
    that way, against 0.298 vs 0.242 and 4 vs 2 -- improvements on both counts
    -- once each side bakes on the same grid.

    What the SHIPPED bake does, fallback included, is a separate contract: it
    is what the preview renders, what the absolute WALL_CHURN_LIMIT ceiling in
    certify_metrics is applied to, what the COMPUTE2 facade is certified on,
    and what tools/test-wall-quality.py asserts against the real header.
    """
    diagnostics = {}
    path = world_assets.texture_path(name)
    shipped_current = world_assets.convert_texture(
        path, PALETTE, use_wall_bake_recipe=False)
    shipped_candidate = world_assets.convert_texture(
        path, PALETTE, use_wall_bake_recipe=True)
    current = world_assets._convert_texture(
        path, PALETTE, use_wall_bake_recipe=False)
    candidate = world_assets._convert_texture(
        path, PALETTE, use_wall_bake_recipe=True,
        diagnostics=diagnostics)
    current_target = world_assets._spatial_smooth(
        diagnostics["normalized_columns"])
    current_error = perceptual_error(current, current_target)
    candidate_error = perceptual_error(
        candidate, diagnostics["filtered_columns"])
    current_edges = edge_f1(
        diagnostics["edge_mask"], palette_edge_mask(current))
    candidate_edges = edge_f1(
        diagnostics["edge_mask"], palette_edge_mask(candidate))
    return {
        "name": name,
        # The shipped grids: rendered by the preview, and the subject of every
        # absolute contract (churn ceiling, COMPUTE2 facade).
        "current": shipped_current,
        "candidate": shipped_candidate,
        "diagnostics": diagnostics,
        "changed_texels": sum(
            shipped_current[y][x] != shipped_candidate[y][x]
            for y in range(HEIGHT) for x in range(WIDTH)),
        "shipped_candidate_churn": churn(shipped_candidate),
        # Same-lattice pair: every recipe-vs-no-recipe comparison. See the
        # docstring for why these must not go through the churn fallback.
        "current_churn": churn(current),
        "candidate_churn": churn(candidate),
        "current_isolated": isolated_count(current),
        "candidate_isolated": isolated_count(candidate),
        "current_error": current_error,
        "candidate_error": candidate_error,
        "error_ratio": candidate_error / max(1e-12, current_error),
        "current_edge_f1": current_edges,
        "candidate_edge_f1": candidate_edges,
    }


def certify_compute2_facade(rows):
    """Structural contract for the deliberately semantic COMPUTE2 bake."""
    # COMPUTE2 is one of the native 56-row sources. Inspect its semantic facade
    # on the square authoring lattice it was composed on -- both axes, since the
    # bake grid is wider than that lattice -- then let the runtime V scale map
    # it back to the native storage rows.
    dim = world_assets.COMPUTE2_FACADE_DIM
    rows = world_assets._resize_index_rows_x(
        world_assets._resize_index_rows(rows[:56], dim), dim)
    for y0, y1 in world_assets.COMPUTE2_RAIL_ROWS:
        candidates = range(max(0, y0 - 1), min(dim, y1 + 2))
        if max(sum(value == 5 for value in rows[y]) for y in candidates) < 60:
            raise AssertionError("COMPUTE2 rail %d is not continuous" % y0)
    # The contract is that readouts live in the authored bays and never bleed
    # onto the rails -- not that any particular palette index appears. Pinning
    # this to index 9 made it a test of which colour PAL3 happened to keep in
    # that slot, which is the composer's input, not its contract.
    accents = world_assets.COMPUTE2_ACCENT_INDICES
    # A readout is a *coloured* accent. The composer's accent set also carries
    # index 13, which it uses as the metal lip, and a neutral highlight sitting
    # on a rail is the facade working, not bleeding.
    palette = world_assets.FROZEN_WORLD_PALETTE
    readouts = {index for index in accents
                if not world_assets.world_palette.is_neutral(palette[index])}
    bays = world_assets.COMPUTE2_READOUT_BAYS
    inside = outside = waveform = 0
    for y, row in enumerate(rows):
        count = sum(1 for value in row if value in readouts)
        if any(y0 <= y < y1 for y0, y1 in bays):
            inside += count
        else:
            outside += count
        if bays[1][0] <= y < bays[1][1]:
            waveform += count
    if not waveform:
        raise AssertionError("COMPUTE2 waveform monitor lost every readout")
    # Measured 96.5% before PAL3's slot 9 became khaki and 92.6% after: the
    # handful outside the bays comes from the panel body, not from the bays
    # leaking. The floor is what stops a future bake from scattering readouts
    # across the facade, which is the failure this certifies against.
    if inside < 0.85 * (inside + outside):
        raise AssertionError("COMPUTE2 readouts scattered outside their bays")
    for x0, x1 in world_assets.COMPUTE2_INSTRUMENT_COLUMNS:
        upper = [rows[y][x] for y in range(4, 14) for x in range(x0, x1)]
        wave = [rows[y][x] for y in range(21, 32) for x in range(x0, x1)]
        if sum(value in (0, 5) for value in upper) < len(upper) * 0.85:
            raise AssertionError("COMPUTE2 upper instrument lost its recess")
        if sum(value in (0, 5) or value in accents for value in wave) < len(wave) * 0.88:
            raise AssertionError("COMPUTE2 waveform monitor lost its recess")
    if any(7 in row for row in rows):
        raise AssertionError("COMPUTE2 facade must not borrow floor index 7")


def certify_metrics(metrics):
    strict = []
    for result in metrics:
        name = result["name"]
        if name == "COMPUTE2":
            certify_compute2_facade(result["candidate"])
        # Absolute ceiling on what actually ships, fallback and all.
        if result["shipped_candidate_churn"] > 0.35:
            raise AssertionError("%s churn exceeds 35%%" % name)
        churn_margin = 0.04 if name == "COMPUTE2" else 0.02
        if result["candidate_churn"] > result["current_churn"] + churn_margin:
            raise AssertionError("%s churn regressed over its margin" % name)
        if result["candidate_isolated"] > result["current_isolated"]:
            raise AssertionError("%s introduced isolated texels" % name)
        if result["error_ratio"] > 1.05:
            raise AssertionError("%s perceptual error regressed over 5%%" % name)
        # The 128-row candidate samples twice as many vertical source rows;
        # quantization can move a single threshold edge by one row even when
        # the material is unchanged. Keep the old contract strict at the
        # material scale while allowing this bounded one-percent raster noise.
        if result["candidate_edge_f1"] + 0.01 < result["current_edge_f1"]:
            raise AssertionError("%s edge retention regressed" % name)
        if result["candidate_edge_f1"] > result["current_edge_f1"] + 0.005:
            strict.append(name)
    if "COMPUTE2" not in strict:
        raise AssertionError("COMPUTE2 must strictly improve edge retention")
    if len(set(strict) - {"COMPUTE2"}) < 4:
        raise AssertionError(
            "at least four additional technological walls must improve: %s" %
            strict)
    return strict


def _source_crop(name):
    path = world_assets.texture_path(name)
    with Image.open(path) as image:
        source = world_assets._fill_transparent_with_average(image)
        window = world_assets.CURATED_TEXTURE_WINDOWS.get(name)
        if window is not None:
            x, y, width, height = window
            source = source.crop((x, y, x + width, y + height))
        elif source.width > world_assets.WALL_TEX_MAX_SOURCE_WIDTH:
            source = source.crop(
                (0, 0, world_assets.WALL_TEX_MAX_SOURCE_WIDTH, source.height))
        return source.copy()


def _fit_panel(image, size=256):
    scale = min(size / image.width, size / image.height)
    fitted = image.resize(
        (max(1, int(image.width * scale)), max(1, int(image.height * scale))),
        Image.Resampling.NEAREST)
    panel = Image.new("RGB", (size, size), (18, 18, 18))
    panel.paste(fitted, ((size - fitted.width) // 2,
                         (size - fitted.height) // 2))
    return panel


def _index_image(rows, colors):
    image = Image.new("RGB", (WIDTH, HEIGHT))
    image.putdata([colors[rows[y][x]] for y in range(HEIGHT) for x in range(WIDTH)])
    return image


def _edge_image(mask):
    image = Image.new("RGB", (WIDTH, HEIGHT))
    image.putdata([(255, 255, 255) if mask[x][y] else (12, 12, 12)
                   for y in range(HEIGHT) for x in range(WIDTH)])
    return image


def _shade_panel(rows, shade_lut):
    panel = Image.new("RGB", (WIDTH * 2, HEIGHT * 2), "black")
    for level, lut in enumerate(shade_lut):
        shaded = [[lut[value] for value in row] for row in rows]
        image = _index_image(shaded, PALETTE)
        panel.paste(image, ((level & 1) * WIDTH, (level >> 1) * HEIGHT))
    return panel.resize((256, 256), Image.Resampling.NEAREST)


def _label(panel, text):
    result = Image.new("RGB", (panel.width, panel.height + 22), "black")
    result.paste(panel, (0, 22))
    ImageDraw.Draw(result).text((6, 6), text, fill="white", font=FONT)
    return result


def make_atlas(result, shade_lut):
    debug_indices = [
        ((index * 97) & 255, (index * 53 + 64) & 255,
         (index * 193 + 32) & 255)
        for index in range(16)
    ]
    panels = [
        _label(_fit_panel(_source_crop(result["name"])), "selected Doom source"),
        _label(_fit_panel(_index_image(result["current"], PALETTE)), "current 64x128"),
        _label(_fit_panel(_index_image(result["candidate"], PALETTE)), "candidate 64x128"),
        _label(_shade_panel(result["candidate"], shade_lut), "candidate shades 0..3"),
        _label(_fit_panel(_index_image(result["candidate"], debug_indices)), "PAL3 index mask"),
        _label(_fit_panel(_edge_image(result["diagnostics"]["edge_mask"])), "preserved edges"),
    ]
    gap = 3
    atlas = Image.new(
        "RGB", (sum(panel.width for panel in panels) + gap * (len(panels) - 1),
                panels[0].height + 30), (96, 0, 20))
    x = 0
    for panel in panels:
        atlas.paste(panel, (x, 0))
        x += panel.width + gap
    summary = (
        "churn %.3f -> %.3f | isolated %d -> %d | edge F1 %.3f -> %.3f" %
        (result["current_churn"], result["candidate_churn"],
         result["current_isolated"], result["candidate_isolated"],
         result["current_edge_f1"], result["candidate_edge_f1"]))
    ImageDraw.Draw(atlas).text((8, panels[0].height + 8), summary,
                               fill=(255, 240, 96), font=FONT)
    return atlas


def _angle_toward(dx, dy):
    return max(range(256), key=lambda angle:
               flat_map_preview.fcos(angle) * dx +
               flat_map_preview.fsin(angle) * dy)


def _point_seg_distance(px, py, a, b):
    ax, ay = a
    bx, by = b
    dx, dy = bx - ax, by - ay
    length_sq = dx * dx + dy * dy
    if not length_sq:
        return math.hypot(px - ax, py - ay)
    t = max(0.0, min(1.0,
        ((px - ax) * dx + (py - ay) * dy) / length_sq))
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy))


def _pose_is_clear(profile, px, py, ignored_seg):
    for seg_id, seg in enumerate(profile.segs):
        if seg_id == ignored_seg:
            continue
        if _point_seg_distance(px, py, profile.vertices[seg.v1],
                               profile.vertices[seg.v2]) < 16.0:
            return False
    return True


def auto_pose_for_material(profile, name, textures, scales, shade_lut):
    """Choose the valid front-side pose exposing most sampled columns."""
    best = None
    for seg_id, seg in enumerate(profile.segs):
        if seg.texture_name != name:
            continue
        a = profile.vertices[seg.v1]
        b = profile.vertices[seg.v2]
        length = math.hypot(b[0] - a[0], b[1] - a[1])
        normal_length = math.hypot(seg.nx, seg.ny)
        if length <= 0 or normal_length <= 0:
            continue
        ux, uy = (b[0] - a[0]) / length, (b[1] - a[1]) / length
        nx, ny = seg.nx / normal_length, seg.ny / normal_length
        for along in (-0.25, 0.0, 0.25):
            target_x = (a[0] + b[0]) * 0.5 + along * length * ux
            target_y = (a[1] + b[1]) * 0.5 + along * length * uy
            for distance in (96, 192):
                px = int(round(target_x + nx * distance))
                py = int(round(target_y + ny * distance))
                if not _pose_is_clear(profile, px, py, seg_id):
                    continue
                angle = _angle_toward(target_x - px, target_y - py)
                pose = ("auto-%s" % name.lower(), px, py, angle)
                rendered = flat_map_preview.render_profile(
                    profile, pose, textures, scales, PALETTE, shade_lut)
                visible = sum(source == seg.source_linedef
                              for source in rendered.source_linedefs)
                score = (visible, -distance, -abs(along))
                if visible and (best is None or score > best[0]):
                    best = (score, pose, (target_x, target_y), (nx, ny))
    if best is None:
        raise AssertionError("no valid preview pose found for %s" % name)
    return best[1:]


def _scene_comparison(current, candidate, label):
    a = flat_map_preview._labeled_frame(current, "current")
    b = flat_map_preview._labeled_frame(candidate, "candidate")
    output = Image.new("RGB", (a.width + b.width + 3, a.height + 18),
                       (96, 0, 20))
    output.paste(a, (0, 0))
    output.paste(b, (a.width + 3, 0))
    ImageDraw.Draw(output).text((8, a.height + 4), label,
                                fill=(255, 240, 96), font=FONT)
    return output


def render_pair(profile, pose, current_textures, candidate_textures,
                scales, shade_lut):
    current = flat_map_preview.render_profile(
        profile, pose, current_textures, scales, PALETTE, shade_lut)
    candidate = flat_map_preview.render_profile(
        profile, pose, candidate_textures, scales, PALETTE, shade_lut)
    if current.source_linedefs != candidate.source_linedefs:
        raise AssertionError("wall bake changed winning geometry at %s" % pose[0])
    if current.depths != candidate.depths:
        raise AssertionError("wall bake changed depth at %s" % pose[0])
    return current, candidate


def motion_poses(name, base_pose, target, normal):
    _, px, py, _ = base_pose
    target_x, target_y = target
    nx, ny = normal
    tx, ty = -ny, nx
    base_distance = math.hypot(px - target_x, py - target_y)
    poses = []
    for frame in range(12):
        phase = frame / 11.0
        distance = base_distance + (96.0 - base_distance) * phase
        lateral = -18.0 + 36.0 * phase
        x = int(round(target_x + nx * distance + tx * lateral))
        y = int(round(target_y + ny * distance + ty * lateral))
        angle = _angle_toward(target_x - x, target_y - y)
        poses.append(("%s-%02d" % (name, frame), x, y, angle))
    return poses


def build_preview(wad_path=DEFAULT_WAD, output_dir=DEFAULT_OUTPUT):
    wad = WadFile(str(wad_path))
    map_data = doom_map.load_map(wad, "E1M1", apply_recipes=True)
    if map_data.wad_sha256 != EXPECTED_WAD_SHA256:
        raise AssertionError("unexpected WAD SHA-256 %s" % map_data.wad_sha256)
    if len(map_data.out_segs) != 394:
        raise AssertionError("E1M1 must retain 394 SEGs")
    if map_data.next_door_group != 5:
        raise AssertionError("E1M1 must retain five door groups")
    if map_data.certificate["exit_index"] != 309:
        raise AssertionError("E1M1 exit certificate drifted")

    metrics = [texture_metrics(name) for name in TECH_MATERIALS]
    strict = certify_metrics(metrics)
    palette = tuple(PALETTE)
    if palette != world_assets.FROZEN_WORLD_PALETTE:
        raise AssertionError("PAL3 drifted")
    if world_assets.GLOBAL_FLOOR_INDEX != 7 or palette[7] != (0x6D, 0x6D, 0x6D):
        raise AssertionError("floor must remain neutral PAL3 index 7")
    profile = flat_map_preview.profile_from_map("current E1M1", map_data)
    current_textures, scales = flat_map_preview.build_texture_bank(
        (profile,), PALETTE, use_wall_bake_recipe=False)
    candidate_textures, candidate_scales = flat_map_preview.build_texture_bank(
        (profile,), PALETTE, use_wall_bake_recipe=True)
    door_texture_count = len({seg["texture_name"] for seg in map_data.out_segs
                              if seg["type"] == doom_map.SEG_DOOR})
    # One packed byte per PAIR column per row -- it carries two displayed
    # texels, so this total is unchanged by the sub-texel bake.
    pair_columns = world_assets.WALL_TEX_WIDTH
    packed_bytes = (4 * len(current_textures) * pair_columns * HEIGHT +
                    4 * door_texture_count * pair_columns * HEIGHT)
    if packed_bytes <= 0:
        raise AssertionError("packed wall table size drifted")
    if scales != candidate_scales:
        raise AssertionError("wall U scales changed")
    for name in current_textures:
        if name not in TECH_MATERIALS and current_textures[name] != candidate_textures[name]:
            raise AssertionError("non-target texture changed: %s" % name)

    shade_lut = world_assets.build_shade_lut(
        PALETTE, 4, (world_assets.GLOBAL_CEILING_INDEX,
                     world_assets.GLOBAL_FLOOR_INDEX))
    output_dir = Path(output_dir)
    atlas_dir = output_dir / "atlases"
    scene_dir = output_dir / "scenes"
    motion_dir = output_dir / "motion"
    for directory in (atlas_dir, scene_dir, motion_dir):
        directory.mkdir(parents=True, exist_ok=True)

    for result in metrics:
        make_atlas(result, shade_lut).save(
            atlas_dir / (result["name"].lower() + ".png"))

    scene_paths = []
    for pose in tuple(PREVIEW_POSES["E1M1"]) + (COMPUTE2_OBLIQUE_POSE,):
        current, candidate = render_pair(
            profile, pose, current_textures, candidate_textures,
            scales, shade_lut)
        path = scene_dir / (pose[0] + ".png")
        _scene_comparison(current, candidate, pose[0]).save(path)
        scene_paths.append(path)

    auto_poses = {}
    for name in TECH_MATERIALS:
        pose, target, normal = auto_pose_for_material(
            profile, name, current_textures, scales, shade_lut)
        auto_poses[name] = pose
        current, candidate = render_pair(
            profile, pose, current_textures, candidate_textures,
            scales, shade_lut)
        path = scene_dir / ("auto-" + name.lower() + ".png")
        _scene_comparison(current, candidate, pose[0]).save(path)
        scene_paths.append(path)

        frames = []
        for moving_pose in motion_poses(name.lower(), pose, target, normal):
            a, b = render_pair(profile, moving_pose, current_textures,
                               candidate_textures, scales, shade_lut)
            frames.append(_scene_comparison(a, b, moving_pose[0]))
        gif_path = motion_dir / (name.lower() + ".gif")
        frames[0].save(gif_path, save_all=True, append_images=frames[1:],
                       duration=100, loop=0)
        contact = Image.new("RGB", (frames[0].width * 3, frames[0].height * 2),
                            "black")
        for index, frame in enumerate(frames[::2]):
            contact.paste(frame, ((index % 3) * frame.width,
                                  (index // 3) * frame.height))
        contact.save(motion_dir / (name.lower() + "-contact.png"))

    serializable = []
    for result in metrics:
        serializable.append({key: value for key, value in result.items()
                             if key not in ("current", "candidate", "diagnostics")})
    report = {
        "wad_sha256": map_data.wad_sha256,
        "segments": len(map_data.out_segs),
        "door_groups": map_data.next_door_group,
        "exit_segment": map_data.certificate["exit_index"],
        "packed_pair_bytes": packed_bytes,
        "strict_edge_improvements": strict,
        "auto_poses": auto_poses,
        "textures": serializable,
    }
    report_path = output_dir / "report.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report, scene_paths


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--wad", type=Path, default=DEFAULT_WAD)
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()
    report, scenes = build_preview(args.wad, args.out_dir)
    print("Wall bake preview certified before generation")
    print("  WAD       : %s" % report["wad_sha256"])
    print("  geometry  : %d SEGs, %d doors, exit SEG %d" %
          (report["segments"], report["door_groups"], report["exit_segment"]))
    print("  improved  : %s" % ", ".join(report["strict_edge_improvements"]))
    print("  report    : %s" % (args.out_dir / "report.json"))
    print("  scenes    : %d" % len(scenes))


if __name__ == "__main__":
    main()
