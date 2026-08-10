#!/usr/bin/env python3
"""Deterministic contracts for the native full-width Doom status bar."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def numeric_define(source: str, name: str) -> int:
    match = re.search(rf"#define {name} (\d+)", source)
    assert match, name
    return int(match.group(1))


def field_pixels(right: int, digits: list[int], widths: list[int], percent: bool) -> set[int]:
    pixels: set[int] = set()
    if percent:
        pixels.update(range(right, right + widths[10]))
    cursor = right
    for digit in reversed(digits):
        cursor -= widths[digit]
        pixels.update(range(cursor, cursor + widths[digit]))
    return pixels


def main() -> int:
    assets = (ROOT / "src/renderer/generated_hud_assets.h").read_text()
    internal = (ROOT / "src/renderer/renderer_internal.h").read_text()
    hud = (ROOT / "src/renderer/renderer_hud.c").read_text()
    renderer = (ROOT / "src/renderer/renderer.c").read_text()
    generator = (ROOT / "tools/convert-freedoom-assets.ps1").read_text()

    assert numeric_define(assets, "FREEDOOM_HUD_PIXEL_W") == 320
    assert numeric_define(assets, "FREEDOOM_HUD_PIXEL_H") == 32
    assert numeric_define(assets, "FREEDOOM_HUD_TILE_W") == 40
    assert numeric_define(assets, "FREEDOOM_HUD_TILE_H") == 4
    assert numeric_define(assets, "FREEDOOM_HUD_TILE_COUNT") == 160
    assert "HUD_PANEL_X 0" in internal
    assert "HUD_PANEL_Y (SCREEN_TILE_H - HUD_PANEL_H)" in internal
    assert "HUD must be flush with the bottom screen edge" in internal

    face_w = numeric_define(assets, "FREEDOOM_FACE_SOURCE_W")
    face_pad = numeric_define(assets, "FREEDOOM_FACE_CONTENT_PAD_X")
    face_cell_w = numeric_define(assets, "FREEDOOM_FACE_TILE_W") * 8
    face_cell_x = ((40 - numeric_define(assets, "FREEDOOM_FACE_TILE_W")) // 2) * 8
    face_x = face_cell_x + face_pad
    assert face_w == 24 and face_pad == 4 and face_cell_w == 32
    assert 2 * face_x + face_w == 320
    assert "Visible Doom face content must be exactly screen-centred" in internal
    assert "Face frame is not pixel-centred" in generator

    widths_match = re.search(
        r"FREEDOOM_HUD_DIGIT_WIDTHS\[FREEDOOM_HUD_DIGIT_COUNT\] = \{\s*([^}]+)",
        assets,
    )
    assert widths_match
    widths = [int(value.strip()) for value in widths_match.group(1).split(",")]
    assert len(widths) == 11 and max(widths) <= 16

    ammo = field_pixels(44, [9, 9, 9], widths, False)
    health = field_pixels(90, [1, 0, 0], widths, True)
    frags = field_pixels(138, [9, 9], widths, False)
    armor = field_pixels(221, [2, 0, 0], widths, True)
    assert min(ammo) >= 0 and max(ammo) < 48
    assert min(health) >= 48 and max(health) < 104
    assert min(frags) >= 104 and max(frags) < 144
    assert min(armor) >= 176 and max(armor) < 240
    face_cell = set(range(144, 176))
    assert frags.isdisjoint(face_cell) and armor.isdisjoint(face_cell)

    # Recomposing from a cleared canvas must remove pixels belonging only to a
    # previous wider value (for example 100 -> 09).
    wide = field_pixels(90, [1, 0, 0], widths, True)
    short = field_pixels(90, [0, 9], widths, True)
    assert wide - short
    assert "clear_number_scratch(tile_count);" in hud
    assert hud.index("clear_number_scratch(tile_count);") < hud.index(
        "if (field->percent && !blank)")
    assert "FREEDOOM_HUD_DIGIT_PERCENT" in hud

    # Melee weapons have no ammo pool, so the ammo field composes empty rather
    # than showing a zero. The blank state needs its own dirty-cache key or a
    # fist -> pistol switch back to the same count would not repaint.
    assert "const u8 count = blank ? 0" in hud
    assert "const u16 ammo_key = state->ammo_visible ? state->ammo : 0xFFFE;" in hud
    assert "draw_hud_number_ex(&HUD_AMMO_FIELD, state->ammo, !state->ammo_visible);" in hud
    assert "s_last_ammo = ammo_key;" in hud
    # 0xFFFE must not collide with a real count: the field clamps at 999.
    assert "const u16 limit = (max_digits == 3) ? 999 : 99;" in hud
    assert "VDP_setTileMapXY(BG_A" in hud
    assert "VDP_drawText" not in hud and "VDP_setTextPalette" not in hud
    assert "FREEDOOM_HUD_DIGIT_PALETTE" in renderer
    assert "HUD number tiles overlap the SGDK font VRAM region" in internal

    print("ok    native 320x32 HUD: flush edges, exact face centre, proportional Doom digits")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
