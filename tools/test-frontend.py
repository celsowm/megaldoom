#!/usr/bin/env python3
"""Deterministic contracts for the Doom title, menus, audio and VRAM use."""

from pathlib import Path
import re
from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
FRONTEND = (ROOT / "src/frontend.c").read_text()
MAIN = (ROOT / "src/main.c").read_text()
AUDIO = (ROOT / "src/game_audio.c").read_text()
RENDERER = (ROOT / "src/renderer.c").read_text()
BILLBOARD = (ROOT / "src/billboard.c").read_text()
ASSETS = ROOT / "res/frontend"


def unique_tiles(path: Path) -> int:
    image = Image.open(path).convert("P")
    tiles = set()
    for y in range(0, image.height, 8):
        for x in range(0, image.width, 8):
            tiles.add(bytes(image.crop((x, y, x + 8, y + 8)).get_flattened_data()))
    return len(tiles)


expected = {
    "title.png": (320, 224),
    "prompt.png": (96, 8),
    "main_menu.png": (320, 224),
    "logo.png": (128, 64),
    "options.png": (112, 24),
    "skull1.png": (24, 24),
    "skull2.png": (24, 24),
}
for selected in range(3):
    for frame in range(2):
        expected[f"main_{selected}_{frame}.png"] = (192, 176)
    expected[f"pause_{selected}.png"] = (320, 224)
for music in range(2):
    for sfx in range(2):
        for selected in range(3):
            expected[f"options_{music}_{sfx}_{selected}.png"] = (320, 224)
for selected in range(5):
    expected[f"skill_{selected}.png"] = (320, 224)
for selected in range(2):
    expected[f"confirm_{selected}.png"] = (320, 224)
palettes = []
for name, size in expected.items():
    image = Image.open(ASSETS / name)
    assert image.mode == "P", f"{name} must remain indexed"
    assert image.size == size, f"{name}: expected {size}, got {image.size}"
    pixels = list(image.get_flattened_data())
    assert max(pixels) < 64, f"{name} exceeds the four VDP palette lines"
    for y in range(0, image.height, 8):
        for x in range(0, image.width, 8):
            tile = image.crop((x, y, x + 8, y + 8))
            assert len({value >> 4 for value in tile.get_flattened_data()}) == 1, \
                f"{name} tile ({x // 8},{y // 8}) mixes VDP palettes"
    palettes.append(tuple(image.getpalette()[:192]))
assert len(set(palettes)) == 1, "frontend images must share all four palettes"

title_tiles = unique_tiles(ASSETS / "title.png")
prompt_tiles = unique_tiles(ASSETS / "prompt.png")
menu_tiles = max(unique_tiles(path) for path in ASSETS.glob("main_[0-2]_[0-1].png"))
assert 16 + title_tiles + prompt_tiles < 1440, "title and prompt exceed user VRAM"
assert 16 + unique_tiles(ASSETS / "main_menu.png") + max(
    unique_tiles(ASSETS / "skull1.png"), unique_tiles(ASSETS / "skull2.png")
) < 1440, "main menu exceeds user VRAM"

pair_base = 16 + (20 * 15 * 2)
pause_tiles = max(
    unique_tiles(path)
    for pattern in ("pause_*.png", "options_*_*_*.png", "skill_*.png", "confirm_*.png")
    for path in ASSETS.glob(pattern)
)
pause_end = pair_base + pause_tiles
assert pause_end < 1440, "pause overlay exceeds the reloadable VRAM region"

for token in (
    "frontend_run", "frontend_run_pause", "frontend_prompt", "FRONTEND_PAUSE_QUIT_TO_TITLE",
    "run_skill_menu", "DOOM_SKILL_HURT_ME_PLENTY", "(selected + 4) % 5",
    "game_audio_toggle_music", "game_audio_toggle_sfx", "wait_for_release", "MAIN_CURSOR_Y 12",
):
    assert token in FRONTEND
for token in (
    "M_DOOM", "M_NGAME", "M_OPTION", "M_QUITG", "M_OPTTTL", "M_SKILL", "M_JKILL",
    "M_ROUGH", "M_HURT", "M_ULTRA", "M_NMARE", "M_SKULL1", "M_SKULL2", "PRESS START",
    "centered_doom_text",
):
    assert token in (ROOT / "tools/generate-frontend-assets.py").read_text()
assert "(selected + 2) % 3" in FRONTEND and "(selected + 1) % 3" in FRONTEND
assert "system_joy & ~previous_system_joy" in MAIN
assert "frontend_run_pause(renderer_get_menu_tile_base())" in MAIN
assert "renderer_restore_after_menu();" in MAIN
assert "game_audio_play_music(test_music);" in MAIN
assert "XGM2_playPCM" not in MAIN
assert "if (s_sfx_enabled) XGM2_playPCM" in AUDIO
assert "if (!s_music_enabled) XGM2_pause" in AUDIO
assert "s_current_music = music" in AUDIO and "XGM2_stop();" in AUDIO
assert "game_audio_suspend_for_video" in FRONTEND and "game_audio_resume_after_video" in FRONTEND
assert "return PAIR_TILE_BASE;" in RENDERER
assert "init_pair_tiles();" in RENDERER and "init_hud_tiles();" in RENDERER
for token in ("DOOM_THING_SKILL_EASY", "DOOM_THING_SKILL_MEDIUM", "DOOM_THING_SKILL_HARD", "doom_skill_thing_mask"):
    assert token in BILLBOARD
assert re.search(r"frontend_run\(\);\s*game_audio_stop_music\(\);\s*renderer_init\(\);", MAIN)

print("ok    frontend: title/menu flow, shared palette, pause VRAM and audio gates")
