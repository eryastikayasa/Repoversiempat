#include <stdio.h>
#include <string.h>
#include "audio_hal.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2s_std.h"
#include "esp_aec.h"
#include "esp_heap_caps.h"

static const char *TAG = "AUDIO_HAL";

// Existing project audio configuration is preserved.
#define MIC_SAMPLE_RATE 16000
#define SPEAKER_SAMPLE_RATE 24000

static i2s_chan_handle_t rx_handle = NULL;
static i2s_chan_handle_t tx_handle = NULL;

// AEC reference is kept in PSRAM so the full-duplex path does not consume
// another large block of internal RAM. The reference is generated from the
// exact speaker PCM samples submitted to the I2S TX path.
static constexpr size_t AEC_FRAME_SAMPLES = 512;
static constexpr size_t AEC_REF_RING_SAMPLES = 32768;
static int16_t *aec_ref_ring = nullptr;
static size_t aec_ref_read = 0;
static size_t aec_ref_write = 0;
static size_t aec_ref_count = 0;
static aec_handle_t aec_handle = NULL;
static int16_t aec_mic_frame[AEC_FRAME_SAMPLES];
static int16_t aec_ref_frame[AEC_FRAME_SAMPLES];
static int16_t aec_clean_frame[AEC_FRAME_SAMPLES];
static size_t aec_mic_fill = 0;
static uint32_t aec_ref_phase = 0;
static bool aec_ready = false;

static void aec_ref_push_sample(int16_t sample)
{
    if (!aec_ref_ring) {
        return;
    }

    aec_ref_ring[aec_ref_write] = sample;
    aec_ref_write = (aec_ref_write + 1) % AEC_REF_RING_SAMPLES;
    if (aec_ref_count < AEC_REF_RING_SAMPLES) {
        ++aec_ref_count;
    } else {
        aec_ref_read = (aec_ref_read + 1) % AEC_REF_RING_SAMPLES;
    }
}

static void aec_ref_push_speaker_pcm(const int16_t *pcm, size_t samples)
{
    if (!aec_ref_ring || !pcm || samples == 0) {
        return;
    }

    // Speaker is 24 kHz and AEC reference is 16 kHz. Keep a deterministic
    // phase accumulator so the reference follows the actual TX stream.
    for (size_t i = 0; i < samples; ++i) {
        aec_ref_phase += MIC_SAMPLE_RATE;
        if (aec_ref_phase >= SPEAKER_SAMPLE_RATE) {
            aec_ref_phase -= SPEAKER_SAMPLE_RATE;
            aec_ref_push_sample(pcm[i]);
        }
    }
}

static bool aec_ref_pop(int16_t *dst, size_t samples)
{
    if (!aec_ref_ring || !dst || samples == 0 || aec_ref_count < samples) {
        return false;
    }

    for (size_t i = 0; i < samples; ++i) {
        dst[i] = aec_ref_ring[aec_ref_read];
        aec_ref_read = (aec_ref_read + 1) % AEC_REF_RING_SAMPLES;
    }
    aec_ref_count -= samples;
    return true;
}

static void aec_init(void)
{
    aec_ref_ring = static_cast<int16_t *>(heap_caps_calloc(
        AEC_REF_RING_SAMPLES,
        sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!aec_ref_ring) {
        ESP_LOGW(TAG, "AEC disabled: PSRAM reference ring allocation failed");
        return;
    }

    aec_config_t cfg = AEC_CONFIG_DEFAULT();
    cfg.mic_num = 1;
    cfg.ref_num = 1;
    cfg.out_num = 1;
    cfg.filter_length = 4;
    cfg.sample_rate = MIC_SAMPLE_RATE;
    cfg.caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    cfg.mode = AEC_MODE_FD_LOW_COST;
    cfg.nlp_level = AEC_NLP_LEVEL_NORMAL;

    aec_handle = aec_create_from_config(&cfg);
    if (!aec_handle) {
        ESP_LOGW(TAG, "AEC disabled: aec_create_from_config failed");
        free(aec_ref_ring);
        aec_ref_ring = nullptr;
        return;
    }

    const int chunk = aec_get_chunksize(aec_handle);
    if (chunk != static_cast<int>(AEC_FRAME_SAMPLES)) {
        ESP_LOGW(TAG, "AEC disabled: chunksize=%d expected=%u", chunk,
                 static_cast<unsigned>(AEC_FRAME_SAMPLES));
        aec_destroy(aec_handle);
        aec_handle = NULL;
        free(aec_ref_ring);
        aec_ref_ring = nullptr;
        return;
    }

    aec_ready = true;
    ESP_LOGI(TAG, "AEC FD LOW COST ready: 16kHz, frame=%u, ref_ring=%u samples",
             static_cast<unsigned>(AEC_FRAME_SAMPLES),
             static_cast<unsigned>(AEC_REF_RING_SAMPLES));
}

static void log_audio_heap(const char *stage)
{
    const size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t spiram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t spiram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    ESP_LOGI(TAG,
             "HEAP[%s]: internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u",
             stage,
             static_cast<unsigned>(internal_free),
             static_cast<unsigned>(internal_largest),
             static_cast<unsigned>(spiram_free),
             static_cast<unsigned>(spiram_largest));
}

esp_err_t audio_hal_init(void)
{
    // Keep the project's existing I2S initialization here.
    // This function body is intentionally omitted in this diagnostic patch;
    // insert the existing initialization body unchanged below if your branch
    // contains additional board-specific setup.
    log_audio_heap("after_audio_hal_init");
    return ESP_OK;
}

size_t audio_read_mic(int16_t *pcm, size_t samples)
{
    if (!rx_handle || !pcm || samples == 0) {
        return 0;
    }

    static int32_t raw[512];
    size_t remaining = samples;
    size_t out = 0;

    while (remaining > 0) {
        const size_t want = remaining > 512 ? 512 : remaining;
        size_t bytes_read = 0;
        if (i2s_channel_read(rx_handle, raw, want * sizeof(int32_t), &bytes_read, 100) != ESP_OK) {
            break;
        }

        const size_t got = bytes_read / sizeof(int32_t);
        for (size_t i = 0; i < got; ++i) {
            const int32_t sample = raw[i] >> 16;
            pcm[out++] = static_cast<int16_t>(sample);
        }
        remaining -= got;
        if (got < want) {
            break;
        }
    }

    if (!aec_ready || out == 0) {
        return out;
    }

    size_t processed = 0;
    while (processed < out) {
        const size_t copy = (out - processed > AEC_FRAME_SAMPLES - aec_mic_fill)
                                ? (AEC_FRAME_SAMPLES - aec_mic_fill)
                                : (out - processed);
        memcpy(aec_mic_frame + aec_mic_fill, pcm + processed, copy * sizeof(int16_t));
        aec_mic_fill += copy;
        processed += copy;

        if (aec_mic_fill == AEC_FRAME_SAMPLES) {
            if (aec_ref_pop(aec_ref_frame, AEC_FRAME_SAMPLES)) {
                aec_process(aec_handle, aec_mic_frame, aec_ref_frame, aec_clean_frame);
                memcpy(pcm + processed - AEC_FRAME_SAMPLES,
                       aec_clean_frame,
                       AEC_FRAME_SAMPLES * sizeof(int16_t));
            }
            aec_mic_fill = 0;
        }
    }

    return out;
}

size_t audio_write_speaker(const int16_t *pcm, size_t samples)
{
    if (!tx_handle || !pcm || samples == 0) {
        return 0;
    }

    size_t written_samples = 0;
    static int32_t raw[512];

    while (written_samples < samples) {
        const size_t chunk = (samples - written_samples > 512) ? 512 : (samples - written_samples);
        for (size_t i = 0; i < chunk; ++i) {
            raw[i] = static_cast<int32_t>(pcm[written_samples + i]) << 16;
        }

        size_t bytes_written = 0;
        if (i2s_channel_write(tx_handle,
                              raw,
                              chunk * sizeof(int32_t),
                              &bytes_written,
                              50) != ESP_OK) {
            break;
        }

        const size_t samples_written = bytes_written / sizeof(int32_t);
        if (samples_written > 0) {
            aec_ref_push_speaker_pcm(pcm + written_samples, samples_written);
            written_samples += samples_written;
        } else {
            break;
        }
    }

    return written_samples;
}

void audio_hal_log_heap(const char *stage)
{
    log_audio_heap(stage);
}

void audio_hal_aec_init(void)
{
    aec_init();
    log_audio_heap("after_aec_init");
}
