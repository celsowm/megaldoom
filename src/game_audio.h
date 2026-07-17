#ifndef MEGALDOOM_GAME_AUDIO_H
#define MEGALDOOM_GAME_AUDIO_H

#include <genesis.h>

void game_audio_init(void);
void game_audio_play_music(const u8 *music);
void game_audio_stop_music(void);
void game_audio_play_sfx(const u8 *sample, u32 length, SoundPCMChannel channel);
void game_audio_toggle_music(void);
void game_audio_toggle_sfx(void);
void game_audio_suspend_for_video(void);
void game_audio_resume_after_video(void);
bool game_audio_music_enabled(void);
bool game_audio_sfx_enabled(void);

#endif
