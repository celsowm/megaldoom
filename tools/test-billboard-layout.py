"""Regression check for floor-anchored generated world billboard assets."""

from pathlib import Path
import json
import re
import sys


ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "src" / "billboard" / "generated_billboard_assets.h"
GEOMETRY_HEADER = ROOT / "src" / "billboard" / "generated_billboard_geometry.h"
OFFSETS = ROOT / "res" / "originaldoom" / "sprites" / "_offsets.json"
PUBLIC_HEADER = ROOT / "src" / "billboard" / "billboard.h"
EXPECTED_TEXTURES = 22
WORLD_HEIGHT = 48
# BillboardVisualId values ARE indices into this atlas, so the enum in
# billboard.h and $BillboardWorldSpecs in tools/convert-freedoom-assets.ps1 must
# agree with this order. The collectibles come first: $billboardPickupCount in
# the generator gives column-post tables to the front of the list.
EXPECTED_NAMES = [
    "BONUS", "BLUE_KEY", "YELLOW_KEY", "RED_KEY", "STIMPACK", "MEDIKIT",
    "ARMOR_BONUS", "GREEN_ARMOR", "BLUE_ARMOR", "CLIP", "AMMO_BOX",
    "SHELLS", "SHELL_BOX", "SHOTGUN_PICKUP", "CHAINGUN_PICKUP", "CHAINSAW_PICKUP",
    "CANDLE", "CANDELABRA", "COLUMN", "ELEC", "BARREL", "TREE",
]
EXPECTED_SPRITES = [
    "BON1A0", "BKEYA0", "YKEYA0", "RKEYA0", "STIMA0", "MEDIA0",
    "BON2A0", "ARM1A0", "ARM2A0", "CLIPA0", "AMMOA0",
    "SHELA0", "SBOXA0", "SHOTA0", "MGUNA0", "CSAWA0",
    "CANDA0", "CBRAA0", "COLUA0", "ELECA0", "BAR1A0", "TREDA0",
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
    geometry_text = GEOMETRY_HEADER.read_text(encoding="utf-8")
    offsets = json.loads(OFFSETS.read_text(encoding="utf-8"))
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

    geometry_body = balanced_initializer(
        geometry_text, "FREEDOOM_BILLBOARD_WORLD_GEOMETRY")
    geometry_rows = [
        [int(value) for value in re.findall(r"-?\d+", row)]
        for row in re.findall(r"\{([^{}]+)\}", geometry_body)
    ]
    if len(geometry_rows) != EXPECTED_TEXTURES or any(
            len(row) != 8 for row in geometry_rows):
        raise ValueError("generated billboard geometry dimensions changed")

    for index, sprite_name in enumerate(EXPECTED_SPRITES):
        source = offsets[sprite_name]
        width, height, left, top, atlas_x, atlas_y, atlas_w, atlas_h = geometry_rows[index]
        expected_source = [source["width"], source["height"],
                           source["leftOffset"], source["topOffset"]]
        if [width, height, left, top] != expected_source:
            raise ValueError(f"{sprite_name} WAD geometry metadata drifted")

        scale = min(24 / width, 48 / height)
        expected_w = max(1, round(width * scale))
        expected_h = max(1, round(height * scale))
        expected_x = round((24 - expected_w) / 2)
        expected_y = 48 - expected_h
        if [atlas_x, atlas_y, atlas_w, atlas_h] != [
                expected_x, expected_y, expected_w, expected_h]:
            raise ValueError(f"{sprite_name} atlas crop metadata drifted")

    expected_examples = {
        "BKEYA0": (14, 16),
        "CLIPA0": (9, 11),
        "BAR1A0": (23, 32),
    }
    for name, dimensions in expected_examples.items():
        source = offsets[name]
        if (source["width"], source["height"]) != dimensions:
            raise ValueError(f"{name} no longer has the expected Doom dimensions")

    print(f"ok    {EXPECTED_TEXTURES} world billboards preserve WAD geometry and atlas crops")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(f"error {error}", file=sys.stderr)
        raise SystemExit(1)
