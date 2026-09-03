#!/usr/bin/env python3
"""Render the full slow-motion SEGA/Cacodemon startup timeline.

This reads the indexed generated assets, so the GIF catches a PAL0 collision or
bad sprite-sheet packing before building a ROM.
"""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
LOGO_X = 96
LOGO_Y = 128
CACO_X = 136
CACO_Y = 60
ENTRY_END = 90
ATTACK_START = 180
ATTACK_END = 228
PROJECTILE_START = 228
PROJECTILE_IMPACT = 270
EXPLOSION_END = 318
LETTERS_FLIGHT_END = 438
LAUGH_END = 540
FACE = ((0, 18, 72), (0, 40, 125), (0, 65, 170), (0, 85, 205),
        (20, 112, 225), (72, 152, 238), (128, 196, 248))
IMPACT_FACE = ((255, 255, 255), (255, 255, 255), (182, 255, 255),
               (255, 255, 255), (182, 255, 255), (255, 255, 255),
               (182, 255, 255))
SHIMMER = (
    ((0, 40, 125), (0, 74, 180), (88, 168, 240), (255, 255, 255)),
    ((0, 57, 149), (0, 92, 200), (128, 196, 248), (255, 255, 255)),
    ((0, 40, 125), (0, 92, 200), (255, 255, 255), (128, 196, 248)),
    ((0, 18, 72), (0, 57, 149), (0, 92, 200), (88, 168, 240)),
)
ATTACK_SHIMMER = ((0, 74, 180), (0, 120, 216), (128, 196, 248), (255, 255, 255))
START_X = (109, 130, 152, 174)
VELOCITY_X = (-3, -2, 2, 3)
VELOCITY_Y = (0, -3, 2, 0)
BOB = (0, -1, -2, -2, -2, -1, 0, 1, 2, 2, 2, 1, 0, -1, -2, -1)


def sprite_frame(sheet: Image.Image, index: int, width: int, height: int) -> Image.Image:
    frame = sheet.crop((index * width, 0, (index + 1) * width, height))
    frame.putpalette(sheet.getpalette())
    frame.info["transparency"] = 0
    return frame.convert("RGBA")


def recolour_logo(image: Image.Image, frame: int) -> Image.Image:
    result = image.copy()
    palette = result.getpalette()
    assert palette is not None
    impact = PROJECTILE_IMPACT <= frame < EXPLOSION_END
    face = IMPACT_FACE if impact and ((frame >> 2) & 1) == 0 else FACE
    for index, colour in enumerate(face, start=2):
        palette[index * 3:index * 3 + 3] = colour
    shimmer = ATTACK_SHIMMER if ATTACK_START <= frame < PROJECTILE_IMPACT or impact else SHIMMER[(frame >> 3) & 3]
    for index, colour in enumerate(shimmer, start=12):
        palette[index * 3:index * 3 + 3] = colour
    result.putpalette(palette)
    return result.convert("RGBA")


def caco_frame(frame: int) -> int:
    if ATTACK_START <= frame < ATTACK_END:
        return 2 + (((frame - ATTACK_START) >> 3) & 3)
    if LETTERS_FLIGHT_END <= frame < LAUGH_END:
        return 2 + ((frame >> 2) & 3)
    return (frame >> 3) & 1


def caco_position(frame: int) -> tuple[int, int]:
    x, y = CACO_X, CACO_Y
    if frame < ENTRY_END:
        x = 320 - (frame * (320 - CACO_X)) // (ENTRY_END - 1)
        y = 8 + (frame * (CACO_Y - 8)) // (ENTRY_END - 1)
    bob = BOB[(frame >> 1) & 15]
    if frame >= LETTERS_FLIGHT_END:
        bob *= 2
    return x, y + bob


def letter_position(index: int, frame: int) -> tuple[int, int] | None:
    if frame >= LETTERS_FLIGHT_END:
        return None
    if frame < EXPLOSION_END:
        return START_X[index], LOGO_Y
    travel = frame - EXPLOSION_END
    x = START_X[index] + VELOCITY_X[index] * travel
    y = LOGO_Y + VELOCITY_Y[index] * travel + (travel * travel) // 160
    return (x, y) if -32 < x < 320 and -48 < y < 224 else None


def compose(card: Image.Image, caco: Image.Image, letters: tuple[Image.Image, ...],
            projectile: Image.Image, frame: int) -> Image.Image:
    result = recolour_logo(card, frame)
    for index in range(4):
        position = letter_position(index, frame)
        if position is not None:
            result.alpha_composite(recolour_logo(letters[index], frame), position)
    x, y = caco_position(frame)
    result.alpha_composite(sprite_frame(caco, caco_frame(frame), 48, 56), (x, y))
    if PROJECTILE_START <= frame < PROJECTILE_IMPACT:
        travel = frame - PROJECTILE_START
        x = 132 + (((travel >> 1) & 3) - 1) * 2
        y = 80 + (travel * 43) // (PROJECTILE_IMPACT - PROJECTILE_START - 1)
        result.alpha_composite(sprite_frame(projectile, (travel >> 3) & 1, 56, 48), (x, y))
    elif PROJECTILE_IMPACT <= frame < EXPLOSION_END:
        result.alpha_composite(sprite_frame(projectile, 2 + ((frame - PROJECTILE_IMPACT) >> 4), 56, 48),
                               (132, 123))
    return result


def check_assets(card: Image.Image, caco: Image.Image, letters: tuple[Image.Image, ...],
                 projectile: Image.Image) -> None:
    assert card.mode == "P" and card.size == (320, 224)
    assert set(card.get_flattened_data()) == {0}, "card must be black behind flying letters"
    assert caco.mode == "P" and caco.size == (288, 56) and caco.info.get("transparency") == 0
    assert len(letters) == 4
    for letter in letters:
        assert letter.mode == "P" and letter.size == (32, 48)
        assert letter.info.get("transparency") == 0
    assert projectile.mode == "P" and projectile.size == (280, 48) and projectile.info.get("transparency") == 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets", type=Path, default=ROOT / "res" / "frontend")
    parser.add_argument("--out-dir", type=Path, default=ROOT / "out" / "sega-boot-preview")
    args = parser.parse_args()

    card = Image.open(args.assets / "boot_sega.png")
    caco = Image.open(args.assets / "cacodemon.png")
    letters = tuple(Image.open(args.assets / f"sega_{letter}.png")
                    for letter in "sega")
    projectile = Image.open(args.assets / "cacodemon_projectile.png")
    check_assets(card, caco, letters, projectile)
    args.out_dir.mkdir(parents=True, exist_ok=True)

    keyframes = (("entry", 45), ("hover", 130), ("attack", 205), ("flight", 250),
                 ("blast", 292), ("letters-flying", 368), ("laugh", 486))
    rendered = []
    for name, frame in keyframes:
        image = compose(card, caco, letters, projectile, frame)
        image.save(args.out_dir / f"{name}.png")
        rendered.append(image)

    contact = Image.new("RGBA", (1280, 448), (0, 0, 0, 255))
    for index, image in enumerate(rendered):
        contact.alpha_composite(image, ((index % 4) * 320, (index // 4) * 224))
    contact.save(args.out_dir / "contact-sheet.png")

    timeline = [compose(card, caco, letters, projectile, frame) for frame in range(0, LAUGH_END, 4)]
    timeline[0].save(args.out_dir / "timeline.gif", save_all=True, append_images=timeline[1:],
                     duration=67, loop=0, disposal=2)
    print(f"SEGA boot preview: {args.out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
