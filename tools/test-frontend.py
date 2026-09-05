#!/usr/bin/env python3
"""Deterministic contracts for the Doom title, menus, audio and VRAM use."""

from pathlib import Path
import re
from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
FRONTEND = (ROOT / "src/frontend.c").read_text()
MAIN = (ROOT / "src/main.c").read_text()
AUDIO = (ROOT / "src/game_audio.c").read_text()
RENDERER = (ROOT / "src/renderer/renderer.c").read_text()
BILLBOARD = (ROOT / "src/billboard/billboard.c").read_text()
RESOURCES = (ROOT / "res/resources.res").read_text()
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
    "boot_disclaimer.png": (320, 224),
    "boot_sega.png": (320, 224),
    "boot_social.png": (320, 224),
    "cacodemon.png": (288, 56),
    "cacodemon_projectile.png": (280, 48),
    "sega_s.png": (32, 48),
    "sega_e.png": (32, 48),
    "sega_g.png": (32, 48),
    "sega_a.png": (32, 48),
    "ending_mars.png": (320, 224),
    "ending_thanks.png": (320, 224),
    "death_prompt.png": (96, 8),
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
    if name not in ("cacodemon.png", "boot_sega.png", "cacodemon_projectile.png",
                    "sega_s.png", "sega_e.png", "sega_g.png", "sega_a.png",
                    "ending_mars.png"):
        palettes.append(tuple(image.getpalette()[:192]))
assert len(set(palettes)) == 1, "frontend images must share all four palettes"

boot_sega = Image.open(ASSETS / "boot_sega.png")
assert set(boot_sega.get_flattened_data()) <= set(range(16)), \
    "SEGA boot card must stay entirely in PAL0"
assert boot_sega.getpixel((0, 0)) == 0 and boot_sega.getpixel((319, 223)) == 0, \
    "SEGA boot background must be literal black"
assert set(boot_sega.get_flattened_data()) == {0}, \
    "SEGA boot card must leave the wordmark to moving sprites"
sega_palette = boot_sega.getpalette()
assert sega_palette is not None
assert any(sega_palette[index:index + 3] == [255, 255, 255]
           for index in range(36, 48, 3)), "SEGA shimmer lost its white highlight"
assert all(sega_palette[index + 2] >= sega_palette[index]
           and sega_palette[index + 2] >= sega_palette[index + 1]
           for index in range(3, 48, 3)), "SEGA boot card contains a non-blue palette entry"
assert not (ASSETS / "sega_letters.png").exists(), \
    "SEGA letters must not be retained as a sliced aggregate sheet"
letter_images = [Image.open(ASSETS / f"sega_{letter}.png") for letter in "sega"]
letter_start_match = re.search(
    r"s_sega_letter_start_x\[4\] = \{ ([0-9, ]+) \}", FRONTEND
)
assert letter_start_match, "SEGA letter start positions are missing"
letter_start_x = tuple(int(value.strip())
                       for value in letter_start_match.group(1).split(","))
assert len(letter_start_x) == 4
letter_masks = []
main_logo = []
has_native_one_pixel_detail = False
for index, letter_image in enumerate(letter_images):
    assert letter_image.info.get("transparency") == 0, \
        "SEGA letter tiles need transparent index zero"
    assert set(letter_image.get_flattened_data()) <= set(range(16)), \
        "SEGA letter sprites must remain on PAL0"
    assert letter_image.getpalette() == sega_palette, \
        "SEGA letter sprites must share the animated PAL0 palette"
    assert all(letter_image.getpixel((x, y)) == 0
               for x in (0, 31) for y in range(48)), \
        "each SEGA letter needs an independent transparent horizontal gutter"
    assert all(letter_image.getpixel((x, y)) == 0
               for y in (0, 47) for x in range(32)), \
        "each SEGA letter needs an independent transparent vertical gutter"
    for y in range(0, 48, 2):
        for x in range(0, 32, 2):
            block = {
                letter_image.getpixel((x + dx, y + dy))
                for dy in range(2) for dx in range(2)
            }
            if len(block) > 1:
                has_native_one_pixel_detail = True
    mask = tuple(value != 0 for value in letter_image.get_flattened_data())
    assert any(mask), "each SEGA letter sprite must contain a visible glyph"
    letter_masks.append(mask)
    main_logo.extend(
        (letter_start_x[index] + x, y)
        for y in range(48) for x in range(32)
        if letter_image.getpixel((x, y)) != 0
    )
assert len(set(letter_masks)) == 4, "S, E, G and A must be independently rendered glyphs"
assert has_native_one_pixel_detail, \
    "SEGA logo must retain native one-pixel detail instead of doubled 2x2 blocks"
logo_width = max(x for x, _ in main_logo) - min(x for x, _ in main_logo) + 1
logo_height = max(y for _, y in main_logo) - min(y for _, y in main_logo) + 1
assert 90 <= logo_width <= 105 and 30 <= logo_height <= 36, \
    f"SEGA wordmark drifted from classic size: {logo_width}x{logo_height}"

sprite_vram_match = re.search(r"#define BOOT_SEGA_SPRITE_VRAM_TILES (\d+)", FRONTEND)
assert sprite_vram_match, "SEGA boot sprite VRAM reservation is missing"
sprite_vram_tiles = int(sprite_vram_match.group(1))
required_sprite_tiles = (6 * 7) + (7 * 6) + (4 * 4 * 6)
assert required_sprite_tiles == 180
assert sprite_vram_tiles >= required_sprite_tiles, \
    f"SEGA boot sprites need {required_sprite_tiles} tiles, only {sprite_vram_tiles} reserved"

projectile = Image.open(ASSETS / "cacodemon_projectile.png")
assert projectile.info.get("transparency") == 0
projectile_colors = projectile.getpalette()[:48]
assert any(projectile_colors[index] > projectile_colors[index + 1]
           and projectile_colors[index] > projectile_colors[index + 2]
           for index in range(3, 48, 3)), "projectile palette lost its red ramp"
assert any(projectile_colors[index] > 180 and projectile_colors[index + 1] > 180
           and projectile_colors[index + 2] < 96
           for index in range(3, 48, 3)), "projectile palette lost its yellow ramp"

cacodemon = Image.open(ASSETS / "cacodemon.png")
assert cacodemon.info.get("transparency") == 0, "Cacodemon index 0 must be transparent"
cacodemon_colors = cacodemon.getpalette()[:48]
assert any(cacodemon_colors[index:index + 3][0] > cacodemon_colors[index:index + 3][1]
           for index in range(3, 48, 3)), "Cacodemon palette lost its red ramp"
assert any(cacodemon_colors[index + 1] > cacodemon_colors[index]
           and cacodemon_colors[index + 1] > cacodemon_colors[index + 2]
           for index in range(3, 48, 3)), "Cacodemon palette lost its green ramp"
assert any(cacodemon_colors[index + 2] > cacodemon_colors[index]
           and cacodemon_colors[index + 2] > cacodemon_colors[index + 1]
           for index in range(3, 48, 3)), "Cacodemon palette lost its blue attack ramp"
assert any(cacodemon_colors[index] == cacodemon_colors[index + 1] == cacodemon_colors[index + 2]
           and cacodemon_colors[index] > 0 for index in range(3, 48, 3)), \
    "Cacodemon palette lost its grey ramp"
for frame in range(6):
    frame_image = cacodemon.crop((frame * 48, 0, (frame + 1) * 48, 56))
    tiles = {
        bytes(frame_image.crop((x, y, x + 8, y + 8)).get_flattened_data())
        for y in range(0, 56, 8)
        for x in range(0, 48, 8)
    }
    assert len(tiles) <= 96, f"Cacodemon frame {frame} exceeds reserved sprite VRAM"

title_tiles = unique_tiles(ASSETS / "title.png")
prompt_tiles = unique_tiles(ASSETS / "prompt.png")
menu_tiles = max(unique_tiles(path) for path in ASSETS.glob("main_[0-2]_[0-1].png"))
assert 16 + title_tiles + prompt_tiles < 1440, "title and prompt exceed user VRAM"
boot_tiles = max(
    unique_tiles(ASSETS / name)
    for name in ("boot_disclaimer.png", "boot_sega.png", "boot_social.png")
)
assert 16 + boot_tiles < 1440, "boot card exceeds user VRAM"
ending_mars_tiles = unique_tiles(ASSETS / "ending_mars.png")
ending_thanks_tiles = unique_tiles(ASSETS / "ending_thanks.png")
assert 16 + ending_mars_tiles < 1440, "Mars intermission exceeds user VRAM"
assert 16 + ending_thanks_tiles < 1440, "thanks card exceeds user VRAM"
assert 800 <= ending_mars_tiles <= 1100, "WIMAP0 tile budget drifted unexpectedly"
intermission_stats_tiles = max(
    unique_tiles(ASSETS / "intermission_stats.png"),
    unique_tiles(ASSETS / "intermission_stats_e1m2.png"),
)
intermission_digits_tiles = unique_tiles(ASSETS / "intermission_digits.png")
intermission_map_tiles = sum(unique_tiles(ASSETS / name) for name in (
    "intermission_entering_e1m2.png", "intermission_splat.png",
    "intermission_pointer0.png", "intermission_pointer1.png",
))
assert 16 + ending_mars_tiles + intermission_stats_tiles + intermission_digits_tiles < 1440, \
    "intermission stats exceed user VRAM"
assert 16 + ending_mars_tiles + intermission_map_tiles < 1440, \
    "intermission map overlays exceed user VRAM"
ending_mars = Image.open(ASSETS / "ending_mars.png")
ending_mars_palette = ending_mars.getpalette()
assert ending_mars_palette is not None
for y in (*range(0, 12), *range(212, 224)):
    for x in range(320):
        index = ending_mars.getpixel((x, y))
        assert ending_mars_palette[index * 3:index * 3 + 3] == [0, 0, 0], \
            "WIMAP0 must retain a literal-black 12px letterbox"
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
    "MAIN_CURSOR_STEP 3",
    "frontend_load_death_prompt", "frontend_set_death_prompt", "DEATH_PROMPT_X", "DEATH_PROMPT_Y",
    "run_boot_sequence", "run_boot_card", "BOOT_CARD_FRAMES 180", "BOOT_SEGA_CARD_FRAMES 564",
    "BOOT_SEGA_VISIBLE_FRAMES", "BOOT_CACODEMON_X", "s_sega_letter_start_x",
    "BOOT_CACODEMON_ENTRY_X 320", "BOOT_CACODEMON_ENTRY_Y 8",
    "frontend_boot_disclaimer", "frontend_boot_sega", "frontend_boot_social", "frontend_cacodemon",
    "frontend_cacodemon_projectile", "s_sega_letter_defs", "sfx_cacodemon_fire",
    "frontend_sega_s", "frontend_sega_e", "frontend_sega_g", "frontend_sega_a",
    "sfx_cacodemon_impact", "sfx_cacodemon_laugh",
    "frontend_ending_mars", "frontend_ending_thanks", "frontend_run_intermission",
    "FrontendIntermissionStats", "intermission_percent", "INTERMISSION_INPUT",
    "BOOT_SEGA_SPRITE_VRAM_TILES 192", "SPR_initEx(BOOT_SEGA_SPRITE_VRAM_TILES)",
    "SPR_end()", "animate_sega_shimmer", "animate_sega_letters",
    "fade_sega_card_in", "PAL_setColors(12", "SPR_setFrame", "BOOT_CACODEMON_ATTACK_START",
    "SPR_setFrame(sega_letters[index], 0)",
    "BOOT_PROJECTILE_EXPLOSION_END 318", "BOOT_SEGA_LETTERS_FLIGHT_END 438",
    "BOOT_CACODEMON_LAUGH_END 540", "s_velocity_x", "s_velocity_y",
    "frontend_cacodemon.palette->data", "SYS_doVBlankProcess();",
):
    assert token in FRONTEND
assert "SPR_addSprite(&frontend_cacodemon, BOOT_CACODEMON_ENTRY_X,\n                                  BOOT_CACODEMON_ENTRY_Y," in FRONTEND
assert re.search(r"SPR_end\(\);\s*// SPR_end queues the cleared SAT.*?\s*"
                 r"SYS_doVBlankProcess\(\);", FRONTEND, re.S), \
    "boot sprites must commit their queued SAT clear before the VBlank-only menu"
assert "u16 caco_x = BOOT_CACODEMON_X;" in FRONTEND
assert "u16 caco_y = BOOT_CACODEMON_Y;" in FRONTEND
assert re.search(
    r"caco_x = \(u16\)\(BOOT_CACODEMON_ENTRY_X\s*-\s*"
    r"\(\(u32\)frame \* \(BOOT_CACODEMON_ENTRY_X - BOOT_CACODEMON_X\)\)",
    FRONTEND), "Cacodemon entry must interpolate from the off-screen x position"
assert re.search(
    r"caco_y = \(u16\)\(BOOT_CACODEMON_ENTRY_Y\s*\+\s*"
    r"\(\(u32\)frame \* \(BOOT_CACODEMON_Y - BOOT_CACODEMON_ENTRY_Y\)\)",
    FRONTEND), "Cacodemon entry must interpolate from the off-screen y position"
assert "SPR_addSprite(&frontend_cacodemon, BOOT_CACODEMON_X," not in FRONTEND
for token in (
    "M_DOOM", "M_NGAME", "M_OPTION", "M_QUITG", "M_OPTTTL", "M_SKILL", "M_JKILL",
    "M_ROUGH", "M_HURT", "M_ULTRA", "M_NMARE", "M_SKULL1", "M_SKULL2", "PRESS START",
    "centered_doom_text", "PRESS FIRE", "doom_text_mask",
    "HEADA1", "HEADB1", "HEADC1", "HEADD1", "HEADE1", "HEADF1",
    "make_boot_disclaimer", "make_boot_sega", "make_boot_social", "make_cacodemon",
    "make_cacodemon_projectile", "make_sega_letter", "indexed_fixed_palette", "build_cacodemon_palette",
    "build_projectile_palette", "SEGA_BOOT_PALETTE", "paletted_sega_canvas",
    "SEGA.TTF", "ImageFont.truetype",
    "WIMAP0", "make_ending_mars", "indexed_ending_mars", "make_ending_thanks",
    "EPISODE COMPLETE", "THE INVASION CONTINUES...", "VERSION 0.1",
):
    assert token in (ROOT / "tools/generate-frontend-assets.py").read_text()
assert "(selected + 2) % 3" in FRONTEND and "(selected + 1) % 3" in FRONTEND
assert "system_joy & ~previous_system_joy" in MAIN
assert "frontend_run_pause(renderer_get_menu_tile_base())" in MAIN
assert "renderer_restore_after_menu();" in MAIN
assert "game_audio_play_music((phase_index == 0) ? test_music : e1m2_music);" in MAIN
assert "game_audio_play_music(intermission_music);" in FRONTEND
assert "PAL_fadeOut(0, 63, BOOT_FADE_FRAMES, FALSE);\n    frontend_video_init();" in FRONTEND
assert "level_cleared = TRUE" not in MAIN
assert "demo_exit_pending" in MAIN and "frontend_run_intermission(&stats)" in MAIN
assert 'XGM2 intermission_music "music/d_inter.vgm"' in RESOURCES
assert 'XGM2 e1m2_music  "music/d_e1m2.vgm"' in RESOURCES
assert 'SPRITE frontend_sega_s        "frontend/sega_s.png" 4 6 FAST 0' in RESOURCES
assert 'SPRITE frontend_sega_e        "frontend/sega_e.png" 4 6 FAST 0' in RESOURCES
assert 'SPRITE frontend_sega_g        "frontend/sega_g.png" 4 6 FAST 0' in RESOURCES
assert 'SPRITE frontend_sega_a        "frontend/sega_a.png" 4 6 FAST 0' in RESOURCES
assert 'WAV sfx_cacodemon_laugh  "sound/dscacsit.wav" XGM2' in RESOURCES
assert "XGM2_playPCM" not in MAIN
assert "if (s_sfx_enabled) XGM2_playPCM" in AUDIO
assert "if (!s_music_enabled) XGM2_pause" in AUDIO
assert "s_current_music = music" in AUDIO and "XGM2_stop();" in AUDIO
assert "game_audio_suspend_for_video" in FRONTEND and "game_audio_resume_after_video" in FRONTEND
assert "return PAIR_TILE_BASE;" in RENDERER
assert "init_pair_tiles" not in RENDERER
assert "init_backdrop_tiles" not in RENDERER
assert "init_hud_tiles();" in RENDERER
for token in ("DOOM_THING_SKILL_EASY", "DOOM_THING_SKILL_MEDIUM", "DOOM_THING_SKILL_HARD", "doom_skill_thing_mask"):
    assert token in BILLBOARD
assert re.search(r"frontend_run\(\);\s*game_audio_stop_music\(\);\s*renderer_init\(\);", MAIN)
assert "game_audio_play_music(intro_music);\n    run_boot_card(&frontend_boot_disclaimer" in FRONTEND
assert "wait_for_release(BUTTON_START);" in FRONTEND

# Death state (Doom's PST_REBORN, reproduced in place -- see main.c): health
# locks at zero, the screen holds red, and the player is frozen until they
# press to respawn. PAL0 index 9 is reserved for the death prompt text; the
# HUD tiles must never grow into it, or the prompt would draw the wrong color.
for token in (
    "player_dead", "death_lockout", "DEATH_INPUT_LOCKOUT_VBLANKS",
    "frontend_load_death_prompt", "frontend_set_death_prompt",
    "DEBUG_CHECKPOINT_DEATH",
):
    assert token in MAIN
assert "PAL_setColor(9, RGB24_TO_VDPCOLOR(0xD80000));" in RENDERER

hud_assets = (ROOT / "src/renderer/generated_hud_assets.h").read_text()
hud_tiles_match = re.search(
    r"FREEDOOM_HUD_TILES\[FREEDOOM_HUD_TILE_COUNT\]\[8\]\s*=\s*\{(.*?)\n\};",
    hud_assets, re.S)
assert hud_tiles_match, "could not locate FREEDOOM_HUD_TILES in generated_hud_assets.h"
hud_nibbles = {
    int(digit, 16)
    for word in re.findall(r"0x([0-9A-Fa-f]{8})", hud_tiles_match.group(1))
    for digit in word
}
assert max(hud_nibbles) <= 8, (
    "FREEDOOM_HUD_TILES now uses PAL0 index 9+, which collides with the "
    "death-prompt red reserved in renderer.c's load_game_palettes"
)

print("ok    frontend: title/menu flow, palettes, pause VRAM and audio gates")
