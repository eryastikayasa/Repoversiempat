#pragma once

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

// ================================
// INMP441 - MICROPHONE
// ================================
#define MIC_I2S_SCK   GPIO_NUM_5
#define MIC_I2S_WS    GPIO_NUM_4
#define MIC_I2S_SD    GPIO_NUM_6

// ================================
// MAX98357A - SPEAKER
// ================================
#define SPK_I2S_BCLK  GPIO_NUM_15
#define SPK_I2S_LRCK  GPIO_NUM_16
#define SPK_I2S_DOUT  GPIO_NUM_7

// Gemini Live API input audio
#define MIC_SAMPLE_RATE 16000

// Gemini Live API native audio output
#define SPK_SAMPLE_RATE 24000

#define AUDIO_BITS 16

void audio_hal_init(void);

// Initialize ESP-SR NSNet2 after WakeNet has loaded the shared model partition.
void audio_hal_ns_init(void);

void audio_i2s_test_tone(void);

size_t audio_read_mic(uint8_t *dest, size_t max_len);

size_t audio_write_speaker(const uint8_t *src, size_t len);

// Repo5 WakeWord compatibility adapter.
// Repo4 keeps the I2S RX channel enabled for the lifetime of Audio HAL;
// ownership is controlled by AudioEngine task lifecycle, not by toggling
// the I2S channel. These wrappers preserve the Repo5 engine interface
// without introducing a second MIC implementation.
static inline esp_err_t audio_hal_read_pcm(int16_t *dest, size_t max_samples, size_t *samples_read)
{
    if (!dest || !samples_read || max_samples == 0) return ESP_ERR_INVALID_ARG;
    const size_t bytes = audio_read_mic(reinterpret_cast<uint8_t *>(dest), max_samples * sizeof(int16_t));
    *samples_read = bytes / sizeof(int16_t);
    return (*samples_read > 0) ? ESP_OK : ESP_FAIL;
}

static inline esp_err_t audio_hal_start_capture(void)
{
    // I2S RX is enabled by audio_hal_init(); WakeWord/Conversation tasks own reads.
    return ESP_OK;
}

static inline esp_err_t audio_hal_stop_capture(void)
{
    // Capture remains enabled; the active owner task is stopped by AudioEngine.
    return ESP_OK;
}
