#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool audio_engine_init(void);
bool audio_engine_start_wakeword(void);
void audio_engine_stop_wakeword(void);
bool audio_engine_process_wakeword(void);
bool audio_engine_wakeword_detected(void);
void audio_engine_clear_wakeword(void);
void audio_engine_stop(void);
bool audio_engine_start_conversation(void);
void audio_engine_stop_conversation(void);
bool audio_engine_conversation_active(void);
bool audio_engine_read_mic_frame(int16_t *buffer, size_t samples, uint32_t timeout_ms);
size_t audio_engine_mic_frame_samples(void);
bool audio_engine_start_playback(void);
void audio_engine_stop_playback(void);
bool audio_engine_playback_active(void);
bool audio_engine_write_speaker_pcm(const int16_t *buffer, size_t samples, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
