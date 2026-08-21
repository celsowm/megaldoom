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
SPRITE_SOURCE = ROOT / "res" / "originaldoom" / "sprites"
BOOT_SOURCE = ROOT / "res" / "boot"
SEGA_FONT = BOOT_SOURCE / "SEGA.TTF"
MANIFEST_NAME = ".frontend-assets.json"
MANIFEST_VERSION = 2

PATCHES = (
    "TITLEPIC", "M_DOOM", "M_NGAME", "M_OPTION", "M_QUITG",
    "M_OPTTTL", "M_SKILL", "M_JKILL", "M_ROUGH", "M_HURT", "M_ULTRA", "M_NMARE",
    "M_SKULL1", "M_SKULL2",
)
PROMPT = "PRESS START"
DEATH_PROMPT = "PRESS FIRE"
DOOM_FONT_TEXT = (
    PROMPT, DEATH_PROMPT, "MUSIC ON", "MUSIC OFF", "SFX ON", "SFX OFF", "BACK",
    "MEGALDOOM", "UNOFFICIAL FAN PORT", "INSPIRED BY DOOM AND ID TECH",
    "NOT AFFILIATED WITH OR ENDORSED BY", "SEGA, ID SOFTWARE, OR ZENIMAX.",
    "DOOM IS A TRADEMARK OF ID SOFTWARE.", "BUILT WITH SGDK", "SGDK",
    "BUILT FOR THE 16-BIT ERA", "SOFTWARE DEVELOPMENT KIT", "FOLLOW THE PROJECT",
    "GITHUB.COM/CELSOWM/MEGALDOOM", "X.COM/PROFCELSOFONTES", "THANKS FOR PLAYING",
)
GLYPHS = tuple(sorted({
    f"STCFN{ord(character):03d}"
    for text in DOOM_FONT_TEXT
    for character in text
    if character != " "
}))
SKILL_PATCHES = ("M_JKILL", "M_ROUGH", "M_HURT", "M_ULTRA", "M_NMARE")
BOOT_INPUTS = (
    SPRITE_SOURCE / "HEADA1.png",
    BOOT_SOURCE / "sgdk-sparkle-source.png",
    SEGA_FONT,
)


def expected_outputs() -> tuple[str, ...]:
    names = [
        "title.png", "prompt.png", "death_prompt.png", "main_menu.png", "logo.png",
        "options.png", "skull1.png", "skull2.png",
        "boot_disclaimer.png", "boot_sgdk.png", "boot_social.png", "cacodemon.png",
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
    return (*tuple(source / f"{name}.png" for name in (*PATCHES, *GLYPHS)), *BOOT_INPUTS)


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


def indexed_fixed_palette(image: Image.Image, palette: list[tuple[int, int, int]],
                          palette_index: int, transparent: bool) -> Image.Image:
    """Index an image into one VDP palette line (needed by SPRITE resources)."""
    rgba = image.convert("RGBA")
    out = Image.new("P", rgba.size, 0)
    flat_palette = [channel for color in palette for channel in color]
    out.putpalette(flat_palette + [0] * (768 - len(flat_palette)))
    source = rgba.load()
    target = out.load()
    base = palette_index * 16
    for y in range(rgba.height):
        for x in range(rgba.width):
            r, g, b, a = source[x, y]
            target[x, y] = base if transparent and a < 128 else nearest((r, g, b), palette, base)
    if transparent:
        out.info["transparency"] = base
    return out


def build_cacodemon_palette(image: Image.Image) -> list[tuple[int, int, int]]:
    """Make a dedicated Genesis-friendly palette for the Cacodemon sprite.

    The frontend palette is tuned for Doom's title/menu patches and has almost
    no room for the sprite's red, grey and green ramps.  Quantize the opaque
    source pixels independently, then snap each channel to the Mega Drive's
    3-bit DAC levels so the sprite keeps its original hue and contrast on PAL1.
    """
    rgba = image.convert("RGBA")
    opaque = [(r, g, b) for r, g, b, a in rgba.getdata() if a >= 128]
    sample = Image.new("RGB", (len(opaque), 1))
    sample.putdata(opaque)
    # Keep a few entries free for the small but distinctive green horns and
    # brown/olive highlights, which a red-dominant median cut would otherwise
    # discard entirely.
    quantized = sample.quantize(colors=12, method=Image.Quantize.MEDIANCUT)
    raw = quantized.getpalette()

    def genesis_level(channel: int) -> int:
        return (round(channel * 7 / 255) * 255) // 7

    colors: list[tuple[int, int, int]] = []
    for index in range(12):
        color = tuple(genesis_level(channel) for channel in raw[index * 3:index * 3 + 3])
        if color not in colors:
            colors.append(color)
    # A pathological source or quantizer still gets a complete 16-entry
    # palette, with useful ramps instead of uninitialized palette entries.
    for color in ((36, 72, 0), (72, 109, 36), (109, 145, 36),
                  (109, 72, 36), (0, 0, 0), (109, 0, 0),
                  (182, 0, 0), (255, 0, 0), (72, 72, 72),
                  (182, 182, 182), (255, 255, 255)):
        if color not in colors:
            colors.append(color)
    return [(0, 0, 0), *colors[:15]]


def transparent_canvas(width: int, height: int) -> Image.Image:
    return Image.new("RGBA", (width, height), (0, 0, 0, 0))


def centered_boot_text(image: Image.Image, text: str, y: int,
                       color: tuple[int, int, int, int] = (255, 255, 255, 255)) -> None:
    # Use the extracted Doom glyphs for every boot label, matching the
    # existing title and menu. The color argument remains for compatibility
    # with the earlier prototype callers.
    centered_doom_text(image, text, y)


def centered_text(image: Image.Image, text: str, y: int) -> None:
    draw = ImageDraw.Draw(image)
    font = ImageFont.load_default()
    box = draw.textbbox((0, 0), text, font=font)
    draw.text(((image.width - (box[2] - box[0])) // 2, y), text,
              font=font, fill=(255, 255, 255, 255))


def centered_doom_text(image: Image.Image, text: str, y: int, source: Path = SOURCE) -> None:
    patch = doom_text(text, source)
    image.alpha_composite(patch, ((image.width - patch.width) // 2, y))


def centered_doom_text_at(image: Image.Image, text: str, x: int, y: int,
                          source: Path = SOURCE) -> None:
    image.alpha_composite(doom_text(text, source), (x, y))


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


def boot_background() -> Image.Image:
    image = Image.new("RGBA", (320, 224), (4, 2, 2, 255))
    draw = ImageDraw.Draw(image)
    draw.rectangle((12, 12, 307, 211), outline=(64, 8, 4, 255), width=2)
    draw.rectangle((16, 16, 303, 207), outline=(24, 8, 4, 255), width=1)
    for x, y in ((12, 12), (303, 12), (12, 207), (303, 207)):
        draw.rectangle((x, y, x + 4, y + 4), fill=(176, 16, 8, 255))
    return image


def draw_sgdk_logo(image: Image.Image, source: Path = SOURCE) -> None:
    """Render the SGDK wordmark with the supplied SEGA-style display font."""
    font = ImageFont.truetype(str(SEGA_FONT), 72)
    draw = ImageDraw.Draw(image)
    bbox = draw.textbbox((0, 0), "SGDK", font=font, stroke_width=2)
    width = bbox[2] - bbox[0]
    x = (image.width - width) // 2
    y = 54 - bbox[1]
    # Dark navy extrusion, white keyline, then the bright blue face. The
    # three passes keep the logo legible against the black/red boot frame.
    draw.text((x + 3, y + 5), "SGDK", font=font, fill=(4, 8, 32, 255),
              stroke_width=3, stroke_fill=(4, 8, 32, 255))
    draw.text((x, y), "SGDK", font=font, fill=(24, 112, 232, 255),
              stroke_width=2, stroke_fill=(224, 248, 255, 255))
    draw.text((x, y - 1), "SGDK", font=font, fill=(24, 112, 232, 255),
              stroke_width=1, stroke_fill=(8, 48, 144, 255))


def cropped_sparkles() -> Image.Image:
    sparkle = Image.open(BOOT_SOURCE / "sgdk-sparkle-source.png").convert("RGBA")
    bbox = sparkle.getchannel("A").getbbox()
    if bbox is None:
        raise RuntimeError("SGDK sparkle source has no visible pixels")
    sparkle = sparkle.crop(bbox)
    sparkle.thumbnail((104, 52), Image.Resampling.NEAREST)
    pixels = sparkle.load()
    for y in range(sparkle.height):
        for x in range(sparkle.width):
            r, g, b, a = pixels[x, y]
            if a:
                heat = max(r, g, b)
                pixels[x, y] = (min(255, heat), min(224, heat // 3), min(160, heat // 6), a)
    return sparkle


def make_boot_disclaimer() -> Image.Image:
    image = boot_background()
    centered_boot_text(image, "MEGALDOOM", 34)
    centered_boot_text(image, "UNOFFICIAL FAN PORT", 68)
    lines = (
        "INSPIRED BY DOOM AND ID TECH",
        "NOT AFFILIATED WITH OR ENDORSED BY",
        "SEGA, ID SOFTWARE, OR ZENIMAX.",
        "DOOM IS A TRADEMARK OF ID SOFTWARE.",
        "BUILT WITH SGDK.",
    )
    for index, line in enumerate(lines):
        centered_boot_text(image, line, 94 + index * 16)
    return image


def make_boot_sgdk() -> Image.Image:
    image = boot_background()
    draw = ImageDraw.Draw(image)
    draw.rectangle((24, 40, 295, 135), fill=(12, 4, 4, 255), outline=(96, 12, 4, 255))
    draw_sgdk_logo(image)
    sparkles = cropped_sparkles()
    image.alpha_composite(sparkles, (25, 13))
    centered_boot_text(image, "BUILT FOR THE 16-BIT ERA", 160)
    centered_boot_text(image, "SOFTWARE DEVELOPMENT KIT", 178)
    return image


def draw_github_icon(draw: ImageDraw.ImageDraw, x: int, y: int) -> None:
    """Draw a compact pixel Octocat mark (the official GitHub silhouette)."""
    colour = (224, 248, 255, 255)
    dark = (4, 2, 2, 255)
    pixels = (
        "....##....##....",
        "...####..####...",
        "..############..",
        ".##############.",
        ".##############.",
        "################",
        "####.########.##",
        "################",
        ".##############.",
        "..############..",
        "...##########...",
        "....########....",
        "....########....",
        "...##..##.......",
        "..##...##.......",
        ".##....##.......",
    )
    for row, line in enumerate(pixels):
        for column, value in enumerate(line):
            if value == "#":
                draw.point((x + column, y + row), fill=colour)
    # Two one-pixel cutouts keep the silhouette readable at 1x scale.
    draw.point((x + 5, y + 6), fill=dark)
    draw.point((x + 10, y + 6), fill=dark)


def draw_x_icon(draw: ImageDraw.ImageDraw, x: int, y: int) -> None:
    """Draw the official X mark's two broad interlocking diagonals."""
    colour = (224, 248, 255, 255)
    draw.polygon((
        (x + 2, y + 1), (x + 5, y + 1), (x + 8, y + 5),
        (x + 11, y + 1), (x + 14, y + 1), (x + 10, y + 7),
        (x + 14, y + 14), (x + 11, y + 14), (x + 8, y + 10),
        (x + 5, y + 14), (x + 2, y + 14), (x + 6, y + 7),
    ), fill=colour)


def make_boot_social() -> Image.Image:
    image = boot_background()
    draw = ImageDraw.Draw(image)
    centered_boot_text(image, "FOLLOW THE PROJECT", 44)
    draw_github_icon(draw, 42, 97)
    draw_x_icon(draw, 42, 137)
    centered_doom_text_at(image, "GITHUB.COM/CELSOWM/MEGALDOOM", 72, 100)
    centered_doom_text_at(image, "X.COM/PROFCELSOFONTES", 72, 140)
    centered_boot_text(image, "THANKS FOR PLAYING", 187)
    return image


def make_cacodemon() -> Image.Image:
    source = Image.open(SPRITE_SOURCE / "HEADA1.png").convert("RGBA")
    canvas = transparent_canvas(64, 72)
    canvas.alpha_composite(source, ((64 - source.width) // 2, 72 - source.height))
    return canvas


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
    boot_disclaimer = make_boot_disclaimer()
    boot_sgdk = make_boot_sgdk()
    boot_social = make_boot_social()
    cacodemon = make_cacodemon()
    cacodemon_palette = build_cacodemon_palette(cacodemon)
    # Keep the original Doom title/menu palette selection stable; boot cards
    # are quantized into that same four-line palette afterward.
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
        "boot_disclaimer.png": (boot_disclaimer, False),
        "boot_sgdk.png": (boot_sgdk, False),
        "boot_social.png": (boot_social, False),
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

    # This is a sprite-engine resource. Keep index 0 transparent, but give it
    # its own quantized palette so Doom's red/grey/green ramps are not forced
    # through the title/menu palette.
    indexed_fixed_palette(cacodemon, cacodemon_palette, 0, True).save(
        output / "cacodemon.png", optimize=False)

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
