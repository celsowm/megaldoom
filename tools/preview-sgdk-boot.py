#!/usr/bin/env python3
"""Render exact-paletted stills for the SGDK/Cacodemon boot card.

The runtime owns PAL0 with the background wordmark and PAL1 with the sprite.
This preview reads the generated indexed PNGs, not the source art, so it
catches a palette-line collision before a ROM is built.
"""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
CARD_X = 128
CARD_Y = 50
SHIMMER_NORMAL = (
    ((0, 73, 182), (0, 146, 255), (109, 219, 255), (255, 255, 255)),
    ((0, 109, 219), (0, 182, 255), (182, 255, 255), (255, 255, 255)),
    ((0, 73, 182), (0, 182, 255), (255, 255, 255), (182, 255, 255)),
)
SHIMMER_ATTACK = ((0, 146, 255), (0, 216, 255), (182, 255, 255), (255, 255, 255))


def recolour_shimmer(card: Image.Image, colours: tuple[tuple[int, int, int], ...]) -> Image.Image:
    result = card.copy()
    palette = result.getpalette()
    assert palette is not None
    for index, colour in enumerate(colours, start=12):
        palette[index * 3:index * 3 + 3] = colour
    result.putpalette(palette)
    return result.convert("RGBA")


def sprite_frame(sheet: Image.Image, index: int) -> Image.Image:
    frame = sheet.crop((index * 64, 0, (index + 1) * 64, 72))
    frame.putpalette(sheet.getpalette())
    frame.info["transparency"] = 0
    return frame.convert("RGBA")


def compose(card: Image.Image, sheet: Image.Image, frame: int, bob: int,
            shimmer: tuple[tuple[int, int, int], ...]) -> Image.Image:
    result = recolour_shimmer(card, shimmer)
    result.alpha_composite(sprite_frame(sheet, frame), (CARD_X, CARD_Y + bob))
    return result


def palette_mask(card: Image.Image) -> Image.Image:
    """Visualize the only permitted palette line: black/PAL0/shimmer."""
    result = Image.new("RGB", card.size, (0, 0, 0))
    source = card.load()
    target = result.load()
    for y in range(card.height):
        for x in range(card.width):
            index = source[x, y]
            if index == 0:
                continue
            target[x, y] = (32, 112, 208) if index < 12 else (224, 248, 255)
    return result


def check_card(card: Image.Image) -> None:
    assert card.mode == "P" and card.size == (320, 224)
    indices = set(card.get_flattened_data())
    assert indices <= set(range(16)), "boot card leaked into PAL1+"
    assert card.getpixel((0, 0)) == 0 and card.getpixel((319, 223)) == 0
    palette = card.getpalette()
    assert palette is not None
    for index in indices - {0}:
        red, green, blue = palette[index * 3:index * 3 + 3]
        assert blue >= red and blue >= green, "card contains a non-blue logo colour"


def check_sheet(sheet: Image.Image) -> None:
    assert sheet.mode == "P" and sheet.size == (384, 72)
    assert sheet.info.get("transparency") == 0
    assert set(sheet.get_flattened_data()) <= set(range(16)), "sprite leaked past PAL1"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets", type=Path, default=ROOT / "res" / "frontend")
    parser.add_argument("--out-dir", type=Path, default=ROOT / "out" / "sgdk-boot-preview")
    args = parser.parse_args()

    card = Image.open(args.assets / "boot_sgdk.png")
    sheet = Image.open(args.assets / "cacodemon.png")
    check_card(card)
    check_sheet(sheet)
    args.out_dir.mkdir(parents=True, exist_ok=True)

    stills = (
        ("idle-a", 0, 0, SHIMMER_NORMAL[0]),
        ("idle-b", 1, -2, SHIMMER_NORMAL[1]),
        ("attack-peak", 3, 0, SHIMMER_ATTACK),
    )
    rendered = []
    for name, frame, bob, shimmer in stills:
        image = compose(card, sheet, frame, bob, shimmer)
        image.save(args.out_dir / f"{name}.png")
        rendered.append(image)
    mask = palette_mask(card)
    mask.save(args.out_dir / "palette-mask.png")
    rendered.append(mask.convert("RGBA"))

    contact = Image.new("RGBA", (640, 448), (0, 0, 0, 255))
    for index, image in enumerate(rendered):
        contact.alpha_composite(image, ((index & 1) * 320, (index >> 1) * 224))
    contact.save(args.out_dir / "contact-sheet.png")
    print(f"SGDK boot preview: {args.out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
