#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize AudioEngine and the WakeWord pipeline. */
bool audio_engine_init(void);

/** Start microphone capture for WakeWord processing. */
bool audio_engine_start_wakeword(void);

/** Stop microphone capture used by WakeWord processing. */
void audio_engine_stop_wakeword(void);

/**
 * Read one PCM16 block from Audio HAL and feed it to WakeNet.
 * Returns true when the wake word is detected.
 */
bool audio_engine_process_wakeword(void);

/** Return true when the latest processing detected the wake word. */
bool audio_engine_wakeword_detected(void);

/** Clear the current wake-word detection flag. */
void audio_engine_clear_wakeword(void);

/** Stop the current AudioEngine capture path. */
void audio_engine_stop(void);

/**
 * Start the conversation microphone path.
 *
 * AudioEngine becomes the microphone owner and prepares fixed 20 ms
 * PCM16 mono frames (320 samples @ 16 kHz). The transport layer may pull
 * those prepared frames with audio_engine_read_mic_frame().
 *
 * WakeWord capture must be stopped before starting this path.
 */
bool audio_engine_start_conversation(void);

/** Stop the conversation microphone path and discard queued input frames. */
void audio_engine_stop_conversation(void);

/** Return true while the conversation microphone path is running. */
bool audio_engine_conversation_active(void);

/**
 * Pull one prepared microphone frame from AudioEngine.
 * Returns true when a complete 20 ms PCM16 frame was read.
 * timeout_ms=0 is non-blocking; UINT32_MAX waits indefinitely.
 */
bool audio_engine_read_mic_frame(int16_t *buffer, size_t samples, uint32_t timeout_ms);

/** Number of PCM samples in one prepared conversation microphone frame. */
size_t audio_engine_mic_frame_samples(void);

/** Start the AudioEngine speaker playback path. */
bool audio_engine_start_playback(void);

/** Stop the AudioEngine speaker playback path. */
void audio_engine_stop_playback(void);

/** Return true while AudioEngine owns the speaker playback path. */
bool audio_engine_playback_active(void);

/**
 * Write PCM16 mono audio to the AudioEngine speaker path.
 * AudioEngine forwards the samples to Audio HAL using the fixed 24 kHz
 * speaker configuration. The caller must not access Audio HAL directly.
 */
bool audio_engine_write_speaker_pcm(const int16_t *buffer,
                                    size_t samples,
                                    uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
