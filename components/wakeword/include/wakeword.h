#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize ESP-SR WakeNet9 using the model partition named "model".
 * The current Repo4-compatible model is wn9_hiesp ("HI, ESP").
 */
bool wakeword_init(void);

/**
 * Feed contiguous mono PCM16 samples at 16 kHz to WakeNet.
 * Returns true only when the wake word is detected.
 */
bool wakeword_process_pcm16(const int16_t *samples, size_t sample_count);

/** Return the WakeNet input chunk size in samples. */
int wakeword_get_chunk_samples(void);

/** Return the WakeNet sample rate expected by the loaded model. */
int wakeword_get_sample_rate(void);

/** Release WakeNet resources. */
void wakeword_deinit(void);

#ifdef __cplusplus
}
#endif
