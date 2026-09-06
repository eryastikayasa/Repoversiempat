#include "audio_hal.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_aec.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

static const char *TAG = "AUDIO_HAL";
static i2s_chan_handle_t rx_handle = NULL;
static i2s_chan_handle_t tx_handle = NULL;

// ESP-SR AEC works at 16 kHz. The speaker path is 24 kHz, so a small
// real-time 24k -> 16k reference queue is maintained from the samples that
// are actually written to the I2S speaker. This gives AEC the far-end signal
// at the acoustic output boundary instead of the still-buffered Gemini PCM.
constexpr size_t AEC_FRAME_SAMPLES = 512;       // 32 ms @ 16 kHz
constexpr size_t AEC_REF_RING_SAMPLES = 32768;  // ~2.0 s @ 16 kHz

static aec_handle_t *aec_handle = NULL;
static int16_t aec_ref_ring[AEC_REF_RING_SAMPLES];
static volatile size_t aec_ref_read = 0;
static volatile size_t aec_ref_write = 0;
static portMUX_TYPE aec_ref_mux = portMUX_INITIALIZER_UNLOCKED;
static bool aec_ready = false;

static inline size_t aec_ref_count_locked(void)
{
    return aec_ref_write - aec_ref_read;
}

static void aec_ref_push_24k(const int16_t *pcm, size_t samples)
{
    if (!pcm || samples == 0) return;

    // 24 kHz -> 16 kHz, exact 2/3 rate. A simple phase-locked sample
    // selection is sufficient for the AEC reference because the AEC filter
    // adapts to the acoustic path; it avoids adding another large resampler.
    static uint32_t phase = 0;
    constexpr uint32_t STEP = 2;
    constexpr uint32_t DEN = 3;

    portENTER_CRITICAL(&aec_ref_mux);
    for (size_t i = 0; i < samples; ++i) {
        phase += STEP;
        if (phase >= DEN) {
            phase -= DEN;
            size_t next = aec_ref_write % AEC_REF_RING_SAMPLES;
            aec_ref_ring[next] = pcm[i];
            ++aec_ref_write;
            if (aec_ref_count_locked() > AEC_REF_RING_SAMPLES) {
                aec_ref_read = aec_ref_write - AEC_REF_RING_SAMPLES;
            }
        }
    }
    portEXIT_CRITICAL(&aec_ref_mux);
}

static void aec_ref_pop(int16_t *dest, size_t samples)
{
    if (!dest || samples == 0) return;

    portENTER_CRITICAL(&aec_ref_mux);
    size_t available = aec_ref_count_locked();
    size_t take = available < samples ? available : samples;
    for (size_t i = 0; i < take; ++i) {
        dest[i] = aec_ref_ring[(aec_ref_read + i) % AEC_REF_RING_SAMPLES];
    }
    aec_ref_read += take;
    portEXIT_CRITICAL(&aec_ref_mux);

    if (take < samples) {
        memset(dest + take, 0, (samples - take) * sizeof(int16_t));
    }
}

static void aec_init(void)
{
    aec_config_t config = {};
    config.mic_num = 1;
    config.ref_num = 1;
    config.out_num = 1;
    config.filter_length = 4;
    config.sample_rate = MIC_SAMPLE_RATE;
    config.caps = MALLOC_CAP_PSRAM | MALLOC_CAP_8BIT;
    config.mode = AEC_MODE_FD_LOW_COST;
    config.nlp_level = AEC_NLP_LEVEL_NORMAL;

    aec_handle = aec_create_from_config(&config);
    if (!aec_handle) {
        ESP_LOGE(TAG, "ESP-SR AEC init gagal - MIC akan tetap berjalan tanpa AEC");
        aec_ready = false;
        return;
    }

    int frame = aec_get_chunksize(aec_handle);
    if (frame != (int)AEC_FRAME_SAMPLES) {
        ESP_LOGE(TAG, "ESP-SR AEC frame tidak cocok: %d, expected=%u", frame, (unsigned)AEC_FRAME_SAMPLES);
        aec_destroy(aec_handle);
        aec_handle = NULL;
        aec_ready = false;
        return;
    }

    aec_ready = true;
    ESP_LOGI(TAG, "ESP-SR AEC READY: mode=%s frame=%d rate=%dHz filter=%d NLP=%s",
             aec_get_mode_string(config.mode), frame, config.sample_rate,
             config.filter_length, aec_get_nlp_string(config.nlp_level));
}

void audio_hal_init(void)
{
    ESP_LOGI(TAG, "Menginisialisasi Audio I2S - proven v6.1.5 / Xiaozhi-compatible...");
    i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    tx_chan_cfg.dma_desc_num = 6; tx_chan_cfg.dma_frame_num = 240; tx_chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &tx_handle, nullptr));
    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    rx_chan_cfg.dma_desc_num = 6; rx_chan_cfg.dma_frame_num = 240; rx_chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&rx_chan_cfg, nullptr, &rx_handle));

    i2s_std_config_t rx_cfg = {};
    rx_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE);
    rx_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
    rx_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    rx_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED; rx_cfg.gpio_cfg.bclk = MIC_I2S_SCK; rx_cfg.gpio_cfg.ws = MIC_I2S_WS;
    rx_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED; rx_cfg.gpio_cfg.din = MIC_I2S_SD;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &rx_cfg));

    // Keep the proven v6.1.5 MAX98357A format: 32-bit I2S slots, LEFT, Philips.
    // Gemini audio remains PCM16/24k; conversion happens only at the I2S boundary.
    i2s_std_config_t tx_cfg = {};
    tx_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SPK_SAMPLE_RATE);
    tx_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
    tx_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
    tx_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    tx_cfg.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_32BIT;
    tx_cfg.slot_cfg.ws_pol = false;
    tx_cfg.slot_cfg.bit_shift = true;
    tx_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED; tx_cfg.gpio_cfg.bclk = SPK_I2S_BCLK; tx_cfg.gpio_cfg.ws = SPK_I2S_LRCK;
    tx_cfg.gpio_cfg.dout = SPK_I2S_DOUT; tx_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &tx_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));

    aec_init();
    ESP_LOGI(TAG, "Audio siap. MIC=%d Hz 32-bit LEFT -> PCM16, SPEAKER=%d Hz 32-bit LEFT",
             MIC_SAMPLE_RATE, SPK_SAMPLE_RATE);
}

size_t audio_read_mic(uint8_t *dest, size_t max_len)
{
    if (!rx_handle || !dest || max_len < sizeof(int16_t)) return 0;
    static int32_t raw[512];
    static int16_t mic_frame[AEC_FRAME_SAMPLES];
    static int16_t ref_frame[AEC_FRAME_SAMPLES];
    static int16_t clean_frame[AEC_FRAME_SAMPLES];

    size_t max_samples = max_len / sizeof(int16_t); if (max_samples > 512) max_samples = 512;
    size_t bytes_read = 0;
    if (i2s_channel_read(rx_handle, raw, max_samples * sizeof(int32_t), &bytes_read, portMAX_DELAY) != ESP_OK) return 0;
    size_t samples = bytes_read / sizeof(int32_t);
    int16_t *pcm = reinterpret_cast<int16_t *>(dest);
    for (size_t i = 0; i < samples; ++i) pcm[i] = static_cast<int16_t>(raw[i] >> 16);

    // audio_task normally receives a full 512-sample frame. Only invoke AEC
    // when the complete frame is available; partial frames remain untouched.
    if (aec_ready && samples == AEC_FRAME_SAMPLES) {
        memcpy(mic_frame, pcm, sizeof(mic_frame));
        aec_ref_pop(ref_frame, AEC_FRAME_SAMPLES);
        aec_process(aec_handle, mic_frame, ref_frame, clean_frame);
        memcpy(pcm, clean_frame, sizeof(clean_frame));
    }

    return samples * sizeof(int16_t);
}

void audio_write_speaker(const uint8_t *src, size_t len)
{
    if (!tx_handle || !src || len < 2) return;
    len &= ~((size_t)1);
    static int32_t tx_buffer[1024];
    static int16_t ref_pcm[512];
    const int16_t *pcm = reinterpret_cast<const int16_t *>(src);
    size_t total = len / sizeof(int16_t), offset = 0;

    /*
     * v7.0.34: keep the bounded I2S write from v7.0.33, but also yield after
     * every successful DMA chunk. v7.0.33 only yielded when I2S stalled; a
     * stream of successful 512-sample writes could still keep audio_playback
     * runnable on CPU1 long enough to starve IDLE1 and trigger Task WDT.
     *
     * ESP-IDF 6.x uses a millisecond timeout for i2s_channel_write().
     * Do not pass portMAX_DELAY here: that value is an RTOS tick sentinel,
     * not an I2S timeout in milliseconds. Bound each DMA wait so a stalled
     * speaker path cannot monopolize CPU1 and starve IDLE1/WDT.
     *
     * 512 PCM samples = 21.3 ms of 24 kHz audio in the 32-bit I2S path.
     * The explicit scheduler yield below gives other CPU1 tasks a chance
     * between chunks without changing the I2S format or DMA configuration.
     */
    constexpr size_t I2S_WRITE_SAMPLES = 512;
    constexpr uint32_t I2S_WRITE_TIMEOUT_MS = 50;

    while (offset < total) {
        size_t n = total - offset; if (n > I2S_WRITE_SAMPLES) n = I2S_WRITE_SAMPLES;
        for (size_t i = 0; i < n; ++i) tx_buffer[i] = static_cast<int32_t>(pcm[offset + i]) << 16;

        size_t written = 0;
        esp_err_t err = i2s_channel_write(tx_handle, tx_buffer, n * sizeof(int32_t), &written, I2S_WRITE_TIMEOUT_MS);
        size_t samples_written = written / sizeof(int32_t);
        if (samples_written > n) samples_written = n;
        offset += samples_written;

        if (err != ESP_OK || samples_written == 0) {
            ESP_LOGW(TAG, "I2S speaker write timeout/fail: err=%s written=%u/%u timeout=%ums",
                     esp_err_to_name(err), (unsigned)written,
                     (unsigned)(n * sizeof(int32_t)), (unsigned)I2S_WRITE_TIMEOUT_MS);
            vTaskDelay(1);
            return;
        }

        // Feed the exact samples that just reached the I2S write boundary to
        // the AEC reference path. The AEC consumer runs from the MIC task.
        if (aec_ready) {
            for (size_t i = 0; i < n; ++i) ref_pcm[i] = pcm[offset - n + i];
            aec_ref_push_24k(ref_pcm, n);
        }

        vTaskDelay(1);
    }
}

void audio_i2s_test_tone(void)
{
    static const int16_t sine_table[24] = {0,2071,4000,5657,6928,7727,8000,7727,6928,5657,4000,2071,0,-2071,-4000,-5657,-6928,-7727,-8000,-6928,-5657,-4000,-2071};
    static int16_t tone[2400];
    if (!tx_handle) return;
    for (size_t i = 0; i < 2400; ++i) tone[i] = sine_table[i % 24];
    ESP_LOGI(TAG, "I2S TEST TONE: 1kHz PCM16 -> PCM32 I2S, 24kHz, 100ms");
    audio_write_speaker(reinterpret_cast<const uint8_t *>(tone), sizeof(tone));
}
