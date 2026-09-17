#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

void audio_hal_init(void);
esp_err_t audio_hal_start_capture(void);
esp_err_t audio_hal_stop_capture(void);
esp_err_t audio_hal_read_pcm(int16_t *buffer, size_t samples, size_t *samples_read);
esp_err_t audio_hal_start_playback(void);
esp_err_t audio_hal_stop_playback(void);
esp_err_t audio_hal_write_pcm(const int16_t *buffer, size_t samples, size_t *samples_written);

#ifdef __cplusplus
}
#endif
