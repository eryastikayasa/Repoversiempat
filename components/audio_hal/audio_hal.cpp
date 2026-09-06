#include "audio_hal.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_aec.h"
#include "esp_heap_caps.h"
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
// are actually written to the I2S speaker.
constexpr size_t AEC_FRAME_SAMPLES = 512;
constexpr size_t AEC_REF_RING_SAMPLES = 32768;

static aec_handle_t *aec_handle = NULL;
// Keep the AEC reference ring in PSRAM so it does not consume scarce
// internal DRAM needed by MbedTLS during the Gemini TLS handshake.
static int16_t *aec_ref_ring = nullptr;
static size_t aec_ref_read = 0;
static size_t aec_ref_write = 0;
static portMUX_TYPE aec_ref_mux = portMUX_INITIALIZER_UNLOCKED;
static bool aec_ready = false;

static inline size_t aec_ref_count_locked(void)
{
    return aec_ref_write - aec_ref_read;
}

static void aec_ref_push_24k(const int16_t *pcm, size_t samples)
{
    if (!pcm || samples == 0 || !aec_ref_ring) return;

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

    if (!aec_ref_ring) {
        memset(dest, 0, samples * sizeof(int16_t));
        return;
    }

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
    aec_ref_ring = static_cast<int16_t *>(heap_caps_calloc(
        AEC_REF_RING_SAMPLES,
        sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!aec_ref_ring) {
        ESP_LOGE(TAG, "AEC reference ring PSRAM allocation gagal - MIC akan tetap berjalan tanpa AEC");
        aec_ready = false;
        return;
    }

    aec_config_t config = {};
    config.mic_num = 1;
    config.ref_num = 1;
    config.out_num = 1;
    config.filter_length = 4;
    config.sample_rate = MIC_SAMPLE_RATE;
    config.caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    config.mode = AEC_MODE_FD_LOW_COST;
    config.nlp_level = AEC_NLP_LEVEL_NORMAL;

    aec_handle = aec_create_from_config(&config);
    if (!aec_handle) {
        ESP_LOGE(TAG, "ESP-SR AEC init gagal - MIC akan tetap berjalan tanpa AEC");
        heap_caps_free(aec_ref_ring);
        aec_ref_ring = nullptr;
        aec_ready = false;
        return;
    }

    int frame = aec_get_chunksize(aec_handle);
    if (frame != (int)AEC_FRAME_SAMPLES) {
        ESP_LOGE(TAG, "ESP-SR AEC frame tidak cocok: %d, expected=%u", frame, (unsigned)AEC_FRAME_SAMPLES);
        aec_destroy(aec_handle);
        aec_handle = NULL;
        heap_caps_free(aec_ref_ring);
        aec_ref_ring = nullptr;
        aec_ready = false;
        return;
    }

    aec_ready = true;
    ESP_LOGI(TAG, "ESP-SR AEC READY: mode=%s frame=%d rate=%dHz filter=%d NLP=%s",
             aec_get_mode_string(config.mode), frame, config.sample_rate,
             config.filter_length, aec_get_nlp_string(config.nlp_level));
}

static void log_audio_heap(const char *stage)
{
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "HEAP[%s]: internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u",
             stage, (unsigned)internal_free, (unsigned)internal_largest,
             (unsigned)psram_free, (unsigned)psram_largest);
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

    log_audio_heap("after_i2s_init");
    aec_init();
    log_audio_heap("after_aec_init");
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

    // I2S write is blocking until the supplied buffer has been transmitted.
    // Keeping this chunk at 240 samples limits the post-write AEC reference
    // lag to about 10 ms at 24 kHz, matching Espressif's documented AEC
    // recording/reference delay target of roughly 0-10 ms.
    constexpr size_t I2S_WRITE_SAMPLES = 240;
    constexpr uint32_t I2S_WRITE_TIMEOUT_MS = 50;

    while (offset < total) {
        const size_t old_offset = offset;
        size_t n = total - offset;
        if (n > I2S_WRITE_SAMPLES) n = I2S_WRITE_SAMPLES;
        for (size_t i = 0; i < n; ++i) tx_buffer[i] = static_cast<int32_t>(pcm[old_offset + i]) << 16;

        size_t written = 0;
        esp_err_t err = i2s_channel_write(tx_handle, tx_buffer, n * sizeof(int32_t), &written, I2S_WRITE_TIMEOUT_MS);
        size_t samples_written = written / sizeof(int32_t);
        if (samples_written > n) samples_written = n;

        // AEC reference must contain only samples that were actually accepted by I2S.
        // This also avoids the old offset-n underflow when I2S performs a partial write.
        if (aec_ready && samples_written > 0) {
            for (size_t i = 0; i < samples_written; ++i) {
                ref_pcm[i] = pcm[old_offset + i];
            }
            aec_ref_push_24k(ref_pcm, samples_written);
        }

        offset += samples_written;

        if (err != ESP_OK || samples_written == 0) {
            ESP_LOGW(TAG, "I2S speaker write timeout/fail: err=%s written=%u/%u timeout=%ums",
                     esp_err_to_name(err), (unsigned)written,
                     (unsigned)(n * sizeof(int32_t)), (unsigned)I2S_WRITE_TIMEOUT_MS);
            vTaskDelay(1);
            return;
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
