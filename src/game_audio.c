#include "game_audio.h"

static bool s_music_enabled = TRUE;
static bool s_sfx_enabled = TRUE;
static const u8 *s_current_music = NULL;

void game_audio_init(void) {
    s_music_enabled = TRUE;
    s_sfx_enabled = TRUE;
    s_current_music = NULL;
    Z80_loadDriver(Z80_DRIVER_XGM2, TRUE);
}

void game_audio_play_music(const u8 *music) {
    s_current_music = music;
    XGM2_play(music);
    if (!s_music_enabled) XGM2_pause();
}

void game_audio_stop_music(void) {
    XGM2_stop();
    s_current_music = NULL;
}

void game_audio_play_sfx(const u8 *sample, u32 length, SoundPCMChannel channel) {
    if (s_sfx_enabled) XGM2_playPCM(sample, length, channel);
}

void game_audio_toggle_music(void) {
    s_music_enabled = !s_music_enabled;
    if (s_music_enabled && (s_current_music != NULL)) XGM2_play(s_current_music);
    else XGM2_pause();
}

void game_audio_toggle_sfx(void) {
    s_sfx_enabled = !s_sfx_enabled;
    if (!s_sfx_enabled) {
        XGM2_stopPCM(SOUND_PCM_CH2);
        XGM2_stopPCM(SOUND_PCM_CH3);
    }
}

void game_audio_suspend_for_video(void) {
    XGM2_stop();
}

void game_audio_resume_after_video(void) {
    if (s_music_enabled && (s_current_music != NULL)) XGM2_play(s_current_music);
}

bool game_audio_music_enabled(void) {
    return s_music_enabled;
}

bool game_audio_sfx_enabled(void) {
    return s_sfx_enabled;
}
