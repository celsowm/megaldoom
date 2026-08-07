"""Validate generated collectible posts against the packed source textures."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "src" / "billboard" / "generated_billboard_assets.h"
# renderer_scene.c was split by SRP into several files; the packer/billboard-
# draw code these checks look for now lives across that set.
SCENE_SPLIT_FILES = [
    "renderer_scene.c", "renderer_pack.c", "renderer_doors.c",
    "renderer_billboard_draw.c", "renderer_frame_overlay.c",
    "renderer_upload.c", "renderer_sparse.c",
    "renderer_flats.c",
]


def initializer(source: str, marker: str) -> list[int]:
    marker_at = source.index(marker)
    start = source.index("{", marker_at)
    depth = 0
    for end in range(start, len(source)):
        char = source[end]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return [int(value, 0) for value in re.findall(
                    r"(?<![A-Za-z_])(?:0x[0-9A-Fa-f]+|\d+)",
                    source[start:end + 1])]
    raise AssertionError(f"unterminated initializer: {marker}")


def macro(source: str, name: str) -> int:
    match = re.search(rf"^#define\s+{re.escape(name)}\s+(\d+)\s*$",
                      source, re.MULTILINE)
    assert match, f"missing macro {name}"
    return int(match.group(1))


def main() -> None:
    header = HEADER.read_text(encoding="utf-8")
    scene = "\n".join((ROOT / "src" / "renderer" / name).read_text(encoding="utf-8")
                      for name in SCENE_SPLIT_FILES)
    width = macro(header, "FREEDOOM_BILLBOARD_WORLD_W")
    height = macro(header, "FREEDOOM_BILLBOARD_WORLD_H")
    texture_count = macro(header, "FREEDOOM_BILLBOARD_WORLD_TEXTURE_COUNT")
    pickup_count = macro(header, "FREEDOOM_BILLBOARD_PICKUP_TEXTURE_COUNT")
    post_count = macro(header, "FREEDOOM_BILLBOARD_PICKUP_POST_COUNT")

    textures = initializer(header, "FREEDOOM_BILLBOARD_WORLD_TEXTURES[")
    offsets = initializer(header, "FREEDOOM_BILLBOARD_PICKUP_POST_OFFSETS")
    posts = initializer(header, "FREEDOOM_BILLBOARD_PICKUP_POSTS\n")
    flags = initializer(header, "FREEDOOM_BILLBOARD_PICKUP_USE_POSTS")

    assert len(textures) == texture_count * height * width
    assert len(offsets) == pickup_count * (width + 1)
    assert len(posts) == post_count * 2
    assert len(flags) == pickup_count
    assert offsets[0] == 0 and offsets[-1] == post_count

    for visual in range(pickup_count):
        visual_offsets = offsets[visual * (width + 1):(visual + 1) * (width + 1)]
        assert visual_offsets == sorted(visual_offsets)
        opaque = 0
        opaque_x: list[int] = []
        opaque_y: list[int] = []
        for x in range(width):
            begin, end = visual_offsets[x:x + 2]
            for y in range(height):
                source_index = visual * width * height + y * width + x
                source_opaque = textures[source_index] != 0
                post_opaque = any(
                    posts[post * 2] <= y <
                    posts[post * 2] + posts[post * 2 + 1]
                    for post in range(begin, end))
                assert post_opaque == source_opaque, (
                    f"post mismatch visual={visual} x={x} y={y}")
                opaque += int(source_opaque)
                if source_opaque:
                    opaque_x.append(x)
                    opaque_y.append(y)
        crop_area = ((max(opaque_x) - min(opaque_x) + 1) *
                     (max(opaque_y) - min(opaque_y) + 1))
        expected_sparse = opaque * 100 < crop_area * 80
        assert bool(flags[visual]) == expected_sparse

    assert "pickup_post_contains" in scene
    assert "FREEDOOM_BILLBOARD_PICKUP_USE_POSTS[object->visual_id]" in scene
    print(f"ok pickup posts: {pickup_count} textures, {post_count} spans, exact masks")


if __name__ == "__main__":
    main()
