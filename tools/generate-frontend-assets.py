#!/usr/bin/env python3
"""Ensure tile-aligned, shared-palette Doom frontend images for SGDK."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "res" / "originaldoom" / "graphics"
OUTPUT = ROOT / "res" / "frontend"
MANIFEST_NAME = ".frontend-assets.json"
MANIFEST_VERSION = 1

PATCHES = (
    "TITLEPIC", "M_DOOM", "M_NGAME", "M_OPTION", "M_QUITG",
    "M_OPTTTL", "M_SKILL", "M_JKILL", "M_ROUGH", "M_HURT", "M_ULTRA", "M_NMARE",
    "M_SKULL1", "M_SKULL2",
)
PROMPT = "PRESS START"
DEATH_PROMPT = "PRESS FIRE"
DOOM_FONT_TEXT = (PROMPT, DEATH_PROMPT, "MUSIC ON", "MUSIC OFF", "SFX ON", "SFX OFF", "BACK")
GLYPHS = tuple(sorted({
    f"STCFN{ord(character):03d}"
    for text in DOOM_FONT_TEXT
    for character in text
    if character != " "
}))
SKILL_PATCHES = ("M_JKILL", "M_ROUGH", "M_HURT", "M_ULTRA", "M_NMARE")


def expected_outputs() -> tuple[str, ...]:
    names = [
        "title.png", "prompt.png", "death_prompt.png", "main_menu.png", "logo.png",
        "options.png", "skull1.png", "skull2.png",
    ]
    names.extend(f"main_{selected}_{frame}.png" for selected in range(3) for frame in range(2))
    names.extend(
        f"options_{music}_{sfx}_{selected}.png"
        for music in range(2) for sfx in range(2) for selected in range(3)
    )
    names.extend(f"skill_{selected}.png" for selected in range(5))
    names.extend(f"pause_{selected}.png" for selected in range(3))
    names.extend(f"confirm_{selected}.png" for selected in range(2))
    return tuple(names)


EXPECTED_OUTPUTS = expected_outputs()


def generator_hash() -> str:
    return hashlib.sha256(Path(__file__).read_bytes()).hexdigest()


def source_paths(source: Path) -> tuple[Path, ...]:
    return tuple(source / f"{name}.png" for name in (*PATCHES, *GLYPHS))


def source_hash(source: Path) -> str:
    digest = hashlib.sha256()
    for path in source_paths(source):
        digest.update(path.name.encode("utf-8"))
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


def read_manifest(output: Path) -> dict | None:
    try:
        value = json.loads((output / MANIFEST_NAME).read_text(encoding="utf-8"))
    except (OSError, ValueError, TypeError):
        return None
    return value if isinstance(value, dict) else None


def complete_outputs(output: Path) -> bool:
    return all((output / name).is_file() for name in EXPECTED_OUTPUTS)


def cache_status(source: Path, output: Path) -> tuple[bool, str]:
    manifest = read_manifest(output)
    if not complete_outputs(output):
        return False, "one or more generated PNGs are missing"
    if not manifest or manifest.get("version") != MANIFEST_VERSION:
        return False, "the frontend manifest is missing or obsolete"
    if manifest.get("outputs") != list(EXPECTED_OUTPUTS):
        return False, "the expected frontend output set changed"
    if manifest.get("generator_sha256") != generator_hash():
        return False, "the frontend generator changed"

    missing_sources = [path for path in source_paths(source) if not path.is_file()]
    if missing_sources:
        # Copyrighted source PNGs may intentionally be removed after generation.
        # A complete cache made by this exact generator remains usable.
        return True, "source PNGs unavailable; using the complete recorded cache"
    if manifest.get("source_sha256") != source_hash(source):
        return False, "a frontend source PNG changed"
    return True, "fingerprint and all generated PNGs match"


def load(name: str, source: Path = SOURCE) -> Image.Image:
    return Image.open(source / f"{name}.png").convert("RGBA")


def image_tiles(image: Image.Image) -> list[list[tuple[int, int, int]]]:
    rgba = image.convert("RGBA")
    result = []
    for ty in range(0, rgba.height, 8):
        for tx in range(0, rgba.width, 8):
            pixels = []
            for y in range(ty, min(ty + 8, rgba.height)):
                for x in range(tx, min(tx + 8, rgba.width)):
                    r, g, b, a = rgba.getpixel((x, y))
                    if a >= 128:
                        pixels.append((r, g, b))
            if pixels:
                result.append(pixels)
    return result


def quantize_pixels(pixels: list[tuple[int, int, int]]) -> list[tuple[int, int, int]]:
    width = 256
    height = (len(pixels) + width - 1) // width
    sample = Image.new("RGB", (width, height), pixels[-1])
    sample.putdata(pixels + [pixels[-1]] * (width * height - len(pixels)))
    quantized = sample.quantize(colors=15, method=Image.Quantize.MEDIANCUT)
    raw = quantized.getpalette()[:45]
    colors = [(raw[i], raw[i + 1], raw[i + 2]) for i in range(0, len(raw), 3)]
    return [(0, 0, 0), *(colors + [colors[-1]] * 15)[:15]]


def palette_error(pixels: list[tuple[int, int, int]], palette: list[tuple[int, int, int]]) -> int:
    cache = {}
    total = 0
    for rgb in pixels:
        if rgb not in cache:
            cache[rgb] = min(sum((rgb[c] - color[c]) ** 2 for c in range(3)) for color in palette[1:])
        total += cache[rgb]
    return total


def build_palette(images: dict[str, Image.Image]) -> list[tuple[int, int, int]]:
    """Build four deterministic 15-colour tile palettes for the VDP."""
    tiles = []
    for name in PATCHES:
        tiles.extend(image_tiles(images[name]))

    means = [tuple(sum(pixel[c] for pixel in tile) // len(tile) for c in range(3)) for tile in tiles]
    seeds = [min(range(len(tiles)), key=lambda i: sum(means[i]))]
    while len(seeds) < 4:
        seeds.append(max(
            (i for i in range(len(tiles)) if i not in seeds),
            key=lambda i: min(sum((means[i][c] - means[s][c]) ** 2 for c in range(3)) for s in seeds),
        ))
    assignments = [min(range(4), key=lambda p: sum((means[i][c] - means[seeds[p]][c]) ** 2 for c in range(3)))
                   for i in range(len(tiles))]

    palettes = []
    for _ in range(6):
        palettes = []
        for group in range(4):
            pixels = [pixel for i, tile in enumerate(tiles) if assignments[i] == group for pixel in tile]
            palettes.append(quantize_pixels(pixels))
        assignments = [min(range(4), key=lambda p: palette_error(tile, palettes[p])) for tile in tiles]
    return [color for palette in palettes for color in palette]


def nearest(rgb: tuple[int, int, int], palette: list[tuple[int, int, int]], base: int) -> int:
    return min(range(base + 1, base + 16),
               key=lambda i: sum((rgb[c] - palette[i][c]) ** 2 for c in range(3)))


def indexed(image: Image.Image, palette: list[tuple[int, int, int]], transparent: bool) -> Image.Image:
    rgba = image.convert("RGBA")
    out = Image.new("P", rgba.size, 0)
    flat_palette = [channel for color in palette for channel in color]
    out.putpalette(flat_palette + [0] * (768 - len(flat_palette)))
    source = rgba.load()
    target = out.load()
    for ty in range(0, rgba.height, 8):
        for tx in range(0, rgba.width, 8):
            opaque = []
            for y in range(ty, ty + 8):
                for x in range(tx, tx + 8):
                    r, g, b, a = source[x, y]
                    if a >= 128:
                        opaque.append((r, g, b))
            palette_index = 0 if not opaque else min(
                range(4), key=lambda p: palette_error(opaque, palette[p * 16:(p + 1) * 16]))
            base = palette_index * 16
            for y in range(ty, ty + 8):
                for x in range(tx, tx + 8):
                    r, g, b, a = source[x, y]
                    target[x, y] = base if transparent and a < 128 else nearest((r, g, b), palette, base)
    if transparent:
        out.info["transparency"] = 0
    return out


def transparent_canvas(width: int, height: int) -> Image.Image:
    return Image.new("RGBA", (width, height), (0, 0, 0, 0))


def centered_text(image: Image.Image, text: str, y: int) -> None:
    draw = ImageDraw.Draw(image)
    font = ImageFont.load_default()
    box = draw.textbbox((0, 0), text, font=font)
    draw.text(((image.width - (box[2] - box[0])) // 2, y), text,
              font=font, fill=(255, 255, 255, 255))


def centered_doom_text(image: Image.Image, text: str, y: int, source: Path = SOURCE) -> None:
    patch = doom_text(text, source)
    image.alpha_composite(patch, ((image.width - patch.width) // 2, y))


def doom_text(text: str, source: Path = SOURCE) -> Image.Image:
    glyphs: list[Image.Image | None] = []
    width = 0
    for character in text:
        glyph = None if character == " " else load(f"STCFN{ord(character):03d}", source)
        glyphs.append(glyph)
        width += 5 if glyph is None else glyph.width + 1
    out = transparent_canvas(width - 1, 8)
    x = 0
    for glyph in glyphs:
        if glyph is None:
            x += 5
        else:
            out.alpha_composite(glyph, (x, 0))
            x += glyph.width + 1
    return out


def panel_with_skull(images: dict[str, Image.Image], selected: int, frame: int) -> Image.Image:
    panel = Image.new("RGBA", (192, 176), (0, 0, 0, 255))
    skull = images[f"M_SKULL{frame + 1}"]
    panel.alpha_composite(images["M_DOOM"], ((192 - images["M_DOOM"].width) // 2, 0))
    panel.alpha_composite(skull, (6, 79 + selected * 32))
    return panel


def submenu_panel(images: dict[str, Image.Image], selected: int) -> Image.Image:
    panel = Image.new("RGBA", (192, 112), (0, 0, 0, 255))
    panel.alpha_composite(images["M_SKULL1"], (24, 39 + selected * 24))
    return panel


def skill_panel(images: dict[str, Image.Image], selected: int) -> Image.Image:
    panel = Image.new("RGBA", (320, 224), (0, 0, 0, 255))
    title = images["M_SKILL"]
    panel.alpha_composite(title, ((320 - title.width) // 2, 32))
    for index, name in enumerate(SKILL_PATCHES):
        patch = images[name]
        panel.alpha_composite(patch, ((320 - patch.width) // 2, 64 + index * 24))
    skull = images["M_SKULL1"]
    panel.alpha_composite(skull, (8, 60 + selected * 24))
    return panel


def screen_overlay(panel: Image.Image) -> Image.Image:
    screen = transparent_canvas(320, 224)
    screen.alpha_composite(panel, ((320 - panel.width) // 2, (224 - panel.height) // 2))
    return screen


def doom_text_mask(text: str, width: int, height: int, palette_index: int,
                    palette: list[tuple[int, int, int]], source: Path = SOURCE) -> Image.Image:
    """A paletted (not quantized) mask: 0 = transparent, palette_index = lit.

    Unlike every other frontend asset, this is drawn during GAMEPLAY over the
    live PAL0 world/HUD ramp (renderer.c load_game_palettes), not the frontend
    menu palette, so it must not be run through indexed()'s per-tile
    quantizer -- it carries only two literal index values. It still writes the
    same shared 4x16 palette table as every other frontend PNG so
    test-frontend.py's cross-image palette-equality check keeps holding.
    """
    text_image = doom_text(text, source)
    mask = Image.new("P", (width, height), 0)
    flat_palette = [channel for color in palette for channel in color]
    mask.putpalette(flat_palette + [0] * (768 - len(flat_palette)))
    mask.info["transparency"] = 0
    src = text_image.load()
    dst = mask.load()
    off_x = (width - text_image.width) // 2
    for y in range(text_image.height):
        for x in range(text_image.width):
            r, g, b, a = src[x, y]
            if a >= 128:
                dst[off_x + x, y] = palette_index
    return mask


def generate(source: Path, output: Path) -> None:
    images = {name: load(name, source) for name in PATCHES}
    palette = build_palette(images)
    output.mkdir(parents=True, exist_ok=True)

    title = Image.new("RGBA", (320, 224), (0, 0, 0, 255))
    title.alpha_composite(images["TITLEPIC"], (0, 12))
    prompt = Image.new("RGBA", (96, 8), (0, 0, 0, 255))
    prompt_text = doom_text(PROMPT, source)
    prompt.alpha_composite(prompt_text, ((prompt.width - prompt_text.width) // 2, 0))

    logo = transparent_canvas(128, 64)
    logo.alpha_composite(images["M_DOOM"], ((128 - images["M_DOOM"].width) // 2, 0))

    menu = Image.new("RGBA", (320, 224), (0, 0, 0, 255))
    menu.alpha_composite(images["M_DOOM"], ((320 - images["M_DOOM"].width) // 2, 24))
    for name, y in (("M_NGAME", 72), ("M_OPTION", 96), ("M_QUITG", 120)):
        patch = images[name]
        menu.alpha_composite(patch, ((320 - patch.width) // 2, 24 + y))

    options = transparent_canvas(112, 24)
    options.alpha_composite(images["M_OPTTTL"], ((112 - images["M_OPTTTL"].width) // 2, 0))

    skulls = []
    for name in ("M_SKULL1", "M_SKULL2"):
        skull = transparent_canvas(24, 24)
        patch = images[name]
        skull.alpha_composite(patch, ((24 - patch.width) // 2, (24 - patch.height) // 2))
        skulls.append(skull)

    assets = {
        "title.png": (title, False),
        "prompt.png": (prompt, False),
        "main_menu.png": (menu, False),
        "logo.png": (logo, True),
        "options.png": (options, True),
        "skull1.png": (skulls[0], True),
        "skull2.png": (skulls[1], True),
    }

    for selected in range(3):
        for frame in range(2):
            panel = panel_with_skull(images, selected, frame)
            for name, y in (("M_NGAME", 80), ("M_OPTION", 112), ("M_QUITG", 144)):
                patch = images[name]
                panel.alpha_composite(patch, ((192 - patch.width) // 2 + 8, y))
            assets[f"main_{selected}_{frame}.png"] = (panel, False)

    for music in range(2):
        for sfx in range(2):
            for selected in range(3):
                panel = submenu_panel(images, selected)
                patch = images["M_OPTTTL"]
                panel.alpha_composite(patch, ((192 - patch.width) // 2, 0))
                centered_doom_text(panel, f"MUSIC {'ON' if music else 'OFF'}", 40, source)
                centered_doom_text(panel, f"SFX {'ON' if sfx else 'OFF'}", 64, source)
                centered_doom_text(panel, "BACK", 88, source)
                assets[f"options_{music}_{sfx}_{selected}.png"] = (screen_overlay(panel), True)

    for selected in range(5):
        assets[f"skill_{selected}.png"] = (skill_panel(images, selected), False)

    for selected in range(3):
        panel = submenu_panel(images, selected)
        centered_text(panel, "MEGALDOOM", 8)
        centered_text(panel, "RESUME", 40)
        centered_text(panel, "OPTIONS", 64)
        centered_text(panel, "QUIT TO TITLE", 88)
        assets[f"pause_{selected}.png"] = (screen_overlay(panel), True)

    for selected in range(2):
        panel = Image.new("RGBA", (192, 112), (0, 0, 0, 255))
        skull = images["M_SKULL1"]
        panel.alpha_composite(skull, (24, 47 + selected * 24))
        centered_text(panel, "QUIT TO TITLE?", 20)
        centered_text(panel, "YES", 52)
        centered_text(panel, "NO", 76)
        assets[f"confirm_{selected}.png"] = (screen_overlay(panel), True)
    for filename, (image, transparent) in assets.items():
        indexed(image, palette, transparent).save(output / filename, optimize=False)

    # Gameplay-drawn mask: index 9 is the death-prompt red claimed in PAL0 by
    # renderer.c's load_game_palettes (indices 2-8 are already used by the HUD
    # tiles). Saved directly -- see doom_text_mask -- instead of through the
    # indexed()/quantize path every other frontend asset uses.
    doom_text_mask(DEATH_PROMPT, 96, 8, 9, palette, source).save(
        output / "death_prompt.png", optimize=False)


def publish(source: Path, output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    temp = Path(tempfile.mkdtemp(prefix=".frontend-assets-", dir=output.parent))
    backup = output.parent / f".frontend-assets-backup-{os.getpid()}"
    try:
        generate(source, temp)
        manifest = {
            "version": MANIFEST_VERSION,
            "generator_sha256": generator_hash(),
            "source_sha256": source_hash(source),
            "outputs": list(EXPECTED_OUTPUTS),
        }
        (temp / MANIFEST_NAME).write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        if not complete_outputs(temp):
            raise RuntimeError("frontend generator did not produce the complete output set")

        if backup.exists():
            shutil.rmtree(backup)
        if output.exists():
            output.rename(backup)
        try:
            temp.rename(output)
        except Exception:
            if backup.exists() and not output.exists():
                backup.rename(output)
            raise
        if backup.exists():
            shutil.rmtree(backup)
    finally:
        if temp.exists():
            shutil.rmtree(temp)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--force", action="store_true", help="regenerate even when the cache is current")
    parser.add_argument("--check", action="store_true", help="only validate the cache; never write files")
    parser.add_argument("--source-dir", type=Path, default=SOURCE, help=argparse.SUPPRESS)
    parser.add_argument("--output-dir", type=Path, default=OUTPUT, help=argparse.SUPPRESS)
    args = parser.parse_args()

    source = args.source_dir.resolve()
    output = args.output_dir.resolve()
    current, reason = cache_status(source, output)
    if current and not args.force:
        print(f"frontend assets: cached ({reason})")
        return 0
    if args.check:
        print(f"frontend assets: stale ({reason})", file=sys.stderr)
        return 1

    missing_sources = [path for path in source_paths(source) if not path.is_file()]
    if missing_sources:
        print(f"frontend assets: cannot regenerate ({reason})", file=sys.stderr)
        print(f"missing source: {missing_sources[0]}", file=sys.stderr)
        print(
            "Extract your local DOOM1.WAD with 'python tools/wad-extract.py', then rerun the build.",
            file=sys.stderr,
        )
        return 1

    publish(source, output)

    print("generated frontend assets with four shared 16-colour tile palettes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
