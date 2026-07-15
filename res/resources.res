# XGM2 music: rescomp runs xgm2tool to convert the VGM into an XGM2 blob
# embedded in the ROM. The generated resources.h exposes `extern const u8 test_music[]`.
XGM2 test_music "music/d_e1m1.vgm"

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
