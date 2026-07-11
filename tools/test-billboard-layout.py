"""Regression check for floor-anchored generated world billboard assets."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "src" / "generated_billboard_assets.h"
PUBLIC_HEADER = ROOT / "src" / "billboard.h"
EXPECTED_TEXTURES = 17
WORLD_HEIGHT = 48
EXPECTED_NAMES = [
    "BONUS", "BLUE_KEY", "YELLOW_KEY", "RED_KEY", "STIMPACK", "MEDIKIT",
    "ARMOR_BONUS", "GREEN_ARMOR", "BLUE_ARMOR", "CLIP", "AMMO_BOX",
    "CANDLE", "CANDELABRA", "COLUMN", "ELEC", "BARREL", "TREE",
]


def balanced_initializer(text: str, marker: str) -> str:
    start = text.index(marker)
    opening = text.index("{", start)
    depth = 0
    for index in range(opening, len(text)):
        char = text[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return text[opening + 1:index]
    raise ValueError("unterminated world billboard initializer")


def main() -> int:
    text = HEADER.read_text(encoding="utf-8")
    count_match = re.search(r"#define FREEDOOM_BILLBOARD_WORLD_TEXTURE_COUNT\s+(\d+)", text)
    if not count_match or int(count_match.group(1)) != EXPECTED_TEXTURES:
        raise ValueError("unexpected world billboard texture count")
    catalog = "// " + ", ".join(EXPECTED_NAMES)
    if catalog not in text:
        raise ValueError("generated world billboard catalog order changed")

    public_text = PUBLIC_HEADER.read_text(encoding="utf-8")
    enum_match = re.search(r"typedef enum \{(.*?)\} BillboardVisualId;", public_text, re.DOTALL)
    if not enum_match:
        raise ValueError("could not find BillboardVisualId")
    enum_names = re.findall(r"BILLBOARD_VISUAL_([A-Z0-9_]+)\s*=", enum_match.group(1))
    if enum_names[:EXPECTED_TEXTURES] != EXPECTED_NAMES:
        raise ValueError("BillboardVisualId order does not match generated atlas")

    body = balanced_initializer(text, "FREEDOOM_BILLBOARD_WORLD_TEXTURES")
    rows = []
    for row in re.findall(r"\{([^{}]+)\}", body):
        values = [int(value) for value in re.findall(r"\d+", row)]
        if values:
            rows.append(values)

    expected_rows = EXPECTED_TEXTURES * WORLD_HEIGHT
    if len(rows) != expected_rows or any(len(row) != 24 for row in rows):
        raise ValueError("world billboard matrix dimensions changed unexpectedly")

    for texture in range(EXPECTED_TEXTURES):
        sprite_rows = rows[texture * WORLD_HEIGHT:(texture + 1) * WORLD_HEIGHT]
        if not any(sprite_rows[-1]):
            raise ValueError(f"world billboard {texture} is not anchored to the floor row")

    print(f"ok    {EXPECTED_TEXTURES} generated world billboards are floor-anchored")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(f"error {error}", file=sys.stderr)
        raise SystemExit(1)
