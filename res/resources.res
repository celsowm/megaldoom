# XGM2 music: rescomp runs xgm2tool to convert the VGM into an XGM2 blob
# embedded in the ROM. The generated resources.h exposes `extern const u8 test_music[]`.
XGM2 test_music  "music/d_e1m1.vgm"
XGM2 e1m2_music  "music/d_e1m2.vgm"
XGM2 intro_music "music/d_intro.vgm"
XGM2 intermission_music "music/d_inter.vgm"

# Doom frontend. All images are generated from the extracted DOOM1.WAD patches
# with one shared PAL0 palette; transparent overlays use palette index zero.
# Keep the 32 KB title tileset unpacked in ROM: APLIB would require a temporary
# buffer larger than the game's remaining SGDK heap before gameplay starts.
IMAGE frontend_title          "frontend/title.png"     NONE ALL
IMAGE frontend_prompt         "frontend/prompt.png"    NONE ALL
IMAGE frontend_boot_disclaimer "frontend/boot_disclaimer.png" NONE ALL
IMAGE frontend_boot_sega      "frontend/boot_sega.png" NONE ALL
IMAGE frontend_boot_social    "frontend/boot_social.png" NONE ALL
IMAGE frontend_ending_mars    "frontend/ending_mars.png" NONE ALL
IMAGE frontend_ending_thanks  "frontend/ending_thanks.png" NONE ALL
IMAGE frontend_intermission_stats "frontend/intermission_stats.png" NONE ALL
IMAGE frontend_intermission_stats_e1m2 "frontend/intermission_stats_e1m2.png" NONE ALL
IMAGE frontend_intermission_entering_e1m2 "frontend/intermission_entering_e1m2.png" NONE ALL
IMAGE frontend_intermission_digits "frontend/intermission_digits.png" NONE ALL
IMAGE frontend_intermission_splat "frontend/intermission_splat.png" NONE ALL
IMAGE frontend_intermission_pointer0 "frontend/intermission_pointer0.png" NONE ALL
IMAGE frontend_intermission_pointer1 "frontend/intermission_pointer1.png" NONE ALL
SPRITE frontend_cacodemon     "frontend/cacodemon.png" 6 7 FAST 0
SPRITE frontend_cacodemon_projectile "frontend/cacodemon_projectile.png" 7 6 FAST 0
SPRITE frontend_sega_s        "frontend/sega_s.png" 4 6 FAST 0
SPRITE frontend_sega_e        "frontend/sega_e.png" 4 6 FAST 0
SPRITE frontend_sega_g        "frontend/sega_g.png" 4 6 FAST 0
SPRITE frontend_sega_a        "frontend/sega_a.png" 4 6 FAST 0
# Drawn during gameplay over the live PAL0 world/HUD ramp, not the frontend
# palette -- see doom_text_mask() in generate-frontend-assets.py. Index 9 is
# reserved for it in renderer.c's load_game_palettes.
IMAGE frontend_death_prompt   "frontend/death_prompt.png" NONE ALL
IMAGE frontend_main_menu      "frontend/main_menu.png" NONE ALL
IMAGE frontend_skull1         "frontend/skull1.png"    NONE ALL
IMAGE frontend_skull2         "frontend/skull2.png"    NONE ALL
IMAGE frontend_options_0_0_0  "frontend/options_0_0_0.png" NONE ALL
IMAGE frontend_options_0_0_1  "frontend/options_0_0_1.png" NONE ALL
IMAGE frontend_options_0_0_2  "frontend/options_0_0_2.png" NONE ALL
IMAGE frontend_options_0_1_0  "frontend/options_0_1_0.png" NONE ALL
IMAGE frontend_options_0_1_1  "frontend/options_0_1_1.png" NONE ALL
IMAGE frontend_options_0_1_2  "frontend/options_0_1_2.png" NONE ALL
IMAGE frontend_options_1_0_0  "frontend/options_1_0_0.png" NONE ALL
IMAGE frontend_options_1_0_1  "frontend/options_1_0_1.png" NONE ALL
IMAGE frontend_options_1_0_2  "frontend/options_1_0_2.png" NONE ALL
IMAGE frontend_options_1_1_0  "frontend/options_1_1_0.png" NONE ALL
IMAGE frontend_options_1_1_1  "frontend/options_1_1_1.png" NONE ALL
IMAGE frontend_options_1_1_2  "frontend/options_1_1_2.png" NONE ALL
IMAGE frontend_skill_0        "frontend/skill_0.png" NONE ALL
IMAGE frontend_skill_1        "frontend/skill_1.png" NONE ALL
IMAGE frontend_skill_2        "frontend/skill_2.png" NONE ALL
IMAGE frontend_skill_3        "frontend/skill_3.png" NONE ALL
IMAGE frontend_skill_4        "frontend/skill_4.png" NONE ALL
IMAGE frontend_pause_0        "frontend/pause_0.png" NONE ALL
IMAGE frontend_pause_1        "frontend/pause_1.png" NONE ALL
IMAGE frontend_pause_2        "frontend/pause_2.png" NONE ALL
IMAGE frontend_confirm_0      "frontend/confirm_0.png" NONE ALL
IMAGE frontend_confirm_1      "frontend/confirm_1.png" NONE ALL

# XGM2 PCM sound effects. Each WAV is converted at build time by rescomp into an
# 8-bit signed sample resampled to the XGM2 fixed 13300 Hz rate, 256-byte aligned.
# The generated resources.h exposes them as `extern const u8 <name>[]`; the length
# passed to XGM2_playPCM(..) is sizeof(<name>). SFX play on PCM channels 2/3
# (channel 1 is reserved for music) at default priority 6, below music's 7.
WAV sfx_pistol       "sound/dspistol.wav"  XGM2
WAV sfx_enemy_pain   "sound/dspopain.wav"  XGM2
WAV sfx_enemy_death  "sound/dspodth1.wav"  XGM2
WAV sfx_player_pain  "sound/dsplpain.wav"  XGM2
WAV sfx_player_death "sound/dspldeth.wav"  XGM2
WAV sfx_door         "sound/dsstnmov.wav"  XGM2
WAV sfx_pickup       "sound/dsitemup.wav"  XGM2
WAV sfx_barexp       "sound/dsbarexp.wav"  XGM2
WAV sfx_cacodemon_fire   "sound/dsfirsht.wav" XGM2
WAV sfx_cacodemon_impact "sound/dsfirxpl.wav" XGM2
WAV sfx_cacodemon_laugh  "sound/dscacsit.wav" XGM2
# Per-weapon fire sounds, referenced from the WEAPON_DEFS table in src/weapons.c.
# The chaingun deliberately reuses sfx_pistol, as Doom does.
WAV sfx_shotgun      "sound/dsshotgn.wav"  XGM2
WAV sfx_punch        "sound/dspunch.wav"   XGM2
WAV sfx_chainsaw     "sound/dssawful.wav"  XGM2
WAV sfx_weapon_up    "sound/dswpnup.wav"   XGM2
