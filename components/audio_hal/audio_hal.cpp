#include "audio_hal.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_aec.h"
#include "esp_nsn_iface.h"
extern "C" {
#include "esp_nsn_models.h"
}
#include "esp_heap_caps.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "model_path.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

static const char *TAG = "AUDIO_HAL";
static i2s_chan_handle_t rx_handle = NULL;
static i2s_chan_handle_t tx_handle = NULL;

constexpr size_t AEC_FRAME_SAMPLES = 512;
constexpr size_t AEC_REF_RING_SAMPLES = 32768;

static aec_handle_t *aec_handle = NULL;
static int16_t *aec_ref_ring = nullptr;
static size_t aec_ref_read = 0;
static size_t aec_ref_write = 0;
static portMUX_TYPE aec_ref_mux = portMUX_INITIALIZER_UNLOCKED;
static bool aec_ready = false;

static srmodel_list_t *ns_models = nullptr;
static const esp_nsn_iface_t *ns_iface = nullptr;
static esp_nsn_data_t *ns_data = nullptr;
static bool ns_ready = false;

static inline size_t aec_ref_count_locked(void) { return aec_ref_write - aec_ref_read; }

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
            if (aec_ref_count_locked() > AEC_REF_RING_SAMPLES) aec_ref_read = aec_ref_write - AEC_REF_RING_SAMPLES;
        }
    }
    portEXIT_CRITICAL(&aec_ref_mux);
}

static void aec_ref_pop(int16_t *dest, size_t samples)
{
    if (!dest || samples == 0) return;
    if (!aec_ref_ring) { memset(dest, 0, samples * sizeof(int16_t)); return; }
    portENTER_CRITICAL(&aec_ref_mux);
    size_t available = aec_ref_count_locked();
    size_t take = available < samples ? available : samples;
    for (size_t i = 0; i < take; ++i) dest[i] = aec_ref_ring[(aec_ref_read + i) % AEC_REF_RING_SAMPLES];
    aec_ref_read += take;
    portEXIT_CRITICAL(&aec_ref_mux);
    if (take < samples) memset(dest + take, 0, (samples - take) * sizeof(int16_t));
}

static void aec_init(void)
{
    aec_ref_ring = static_cast<int16_t *>(heap_caps_calloc(AEC_REF_RING_SAMPLES, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!aec_ref_ring) { ESP_LOGE(TAG, "AEC reference ring PSRAM allocation gagal - MIC akan tetap berjalan tanpa AEC"); aec_ready = false; return; }
    aec_config_t config = {};
    config.mic_num = 1; config.ref_num = 1; config.out_num = 1;
    config.filter_length = 4; config.sample_rate = MIC_SAMPLE_RATE;
    config.caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    config.mode = AEC_MODE_FD_LOW_COST; config.nlp_level = AEC_NLP_LEVEL_NORMAL;
    aec_handle = aec_create_from_config(&config);
    if (!aec_handle) { ESP_LOGE(TAG, "ESP-SR AEC init gagal - MIC akan tetap berjalan tanpa AEC"); heap_caps_free(aec_ref_ring); aec_ref_ring = nullptr; aec_ready = false; return; }
    int frame = aec_get_chunksize(aec_handle);
    if (frame != (int)AEC_FRAME_SAMPLES) {
        ESP_LOGE(TAG, "ESP-SR AEC frame tidak cocok: %d, expected=%u", frame, (unsigned)AEC_FRAME_SAMPLES);
        aec_destroy(aec_handle); aec_handle = NULL; heap_caps_free(aec_ref_ring); aec_ref_ring = nullptr; aec_ready = false; return;
    }
    aec_ready = true;
    ESP_LOGI(TAG, "ESP-SR AEC READY: mode=%s frame=%d rate=%dHz filter=%d NLP=%s", aec_get_mode_string(config.mode), frame, config.sample_rate, config.filter_length, aec_get_nlp_string(config.nlp_level));
}

static void log_audio_heap(const char *stage)
{
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "HEAP[%s]: internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u", stage, (unsigned)internal_free, (unsigned)internal_largest, (unsigned)psram_free, (unsigned)psram_largest);
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
    rx_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED; rx_cfg.gpio_cfg.bclk = MIC_I2S_SCK; rx_cfg.gpio_cfg.ws = MIC_I2S_WS; rx_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED; rx_cfg.gpio_cfg.din = MIC_I2S_SD;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &rx_cfg));
    i2s_std_config_t tx_cfg = {};
    tx_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SPK_SAMPLE_RATE);
    tx_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
    tx_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO; tx_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    tx_cfg.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_32BIT; tx_cfg.slot_cfg.ws_pol = false; tx_cfg.slot_cfg.bit_shift = true;
    tx_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED; tx_cfg.gpio_cfg.bclk = SPK_I2S_BCLK; tx_cfg.gpio_cfg.ws = SPK_I2S_LRCK; tx_cfg.gpio_cfg.dout = SPK_I2S_DOUT; tx_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &tx_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle)); ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
    log_audio_heap("after_i2s_init"); aec_init(); log_audio_heap("after_aec_init");
    ESP_LOGI(TAG, "Audio siap. MIC=%d Hz 32-bit LEFT -> PCM16, SPEAKER=%d Hz 32-bit LEFT", MIC_SAMPLE_RATE, SPK_SAMPLE_RATE);
}

void audio_hal_ns_init(void)
{
    if (ns_ready) return;

    ns_models = esp_srmodel_init("model");
    if (!ns_models) {
        ESP_LOGE(TAG, "ESP-SR NSNet2 init gagal: model partition tidak tersedia");
        return;
    }

    char *model_name = esp_srmodel_filter(ns_models, ESP_NSNET_PREFIX, NULL);
    if (!model_name) {
        ESP_LOGE(TAG, "ESP-SR NSNet model tidak ditemukan di srmodels.bin");
        return;
    }

    ns_iface = esp_nsnet_handle_from_name(model_name);
    if (!ns_iface) {
        ESP_LOGE(TAG, "ESP-SR NSNet handle tidak ditemukan: %s", model_name);
        return;
    }

    ns_data = ns_iface->create(model_name);
    if (!ns_data) {
        ESP_LOGE(TAG, "ESP-SR NSNet create gagal: %s", model_name);
        ns_iface = nullptr;
        return;
    }

    int chunk = ns_iface->get_samp_chunksize(ns_data);
    if (chunk != (int)AEC_FRAME_SAMPLES) {
        ESP_LOGE(TAG, "ESP-SR NSNet frame tidak cocok: chunk=%d expected=%u", chunk, (unsigned)AEC_FRAME_SAMPLES);
        ns_iface->destroy(ns_data);
        ns_data = nullptr;
        ns_iface = nullptr;
        return;
    }

    ns_ready = true;
    log_audio_heap("after_nsnet2_init");
    ESP_LOGI(TAG, "ESP-SR NSNet2 READY: model=%s frame=%d rate=%dHz", model_name, chunk, MIC_SAMPLE_RATE);
}

size_t audio_read_mic(uint8_t *dest, size_t max_len)
{
    if (!rx_handle || !dest || max_len < sizeof(int16_t)) return 0;
    if (!ns_ready) audio_hal_ns_init();

    static int32_t raw[512];
    static int16_t mic_frame[AEC_FRAME_SAMPLES];
    static int16_t ref_frame[AEC_FRAME_SAMPLES];
    static int16_t clean_frame[AEC_FRAME_SAMPLES];
    static int16_t ns_frame[AEC_FRAME_SAMPLES];
    size_t max_samples = max_len / sizeof(int16_t); if (max_samples > 512) max_samples = 512;
    size_t bytes_read = 0;
    if (i2s_channel_read(rx_handle, raw, max_samples * sizeof(int32_t), &bytes_read, portMAX_DELAY) != ESP_OK) return 0;
    size_t samples = bytes_read / sizeof(int32_t); int16_t *pcm = reinterpret_cast<int16_t *>(dest);
    for (size_t i = 0; i < samples; ++i) pcm[i] = static_cast<int16_t>(raw[i] >> 16);

    if (aec_ready && samples == AEC_FRAME_SAMPLES) {
        memcpy(mic_frame, pcm, sizeof(mic_frame));
        aec_ref_pop(ref_frame, AEC_FRAME_SAMPLES);
        aec_process(aec_handle, mic_frame, ref_frame, clean_frame);
        memcpy(pcm, clean_frame, sizeof(clean_frame));
    }

    // ESP-SR NSNet2 runs after AEC so speaker reference handling is unchanged.
    // It processes the same 512-sample / 32 ms frame used by the AEC path.
    if (ns_ready && samples == AEC_FRAME_SAMPLES) {
        ns_iface->process(ns_data, pcm, ns_frame);
        memcpy(pcm, ns_frame, sizeof(ns_frame));
    }

    // Preserve the original mic/reference scale for AEC, then boost only the
    // processed signal sent to Gemini. This raises far-field speech without
    // changing echo-cancellation reference behavior.
    constexpr int MIC_OUTPUT_GAIN = 4; // +12 dB
    for (size_t i = 0; i < samples; ++i) {
        int32_t value = (int32_t)pcm[i] * MIC_OUTPUT_GAIN;
        if (value > INT16_MAX) value = INT16_MAX;
        if (value < INT16_MIN) value = INT16_MIN;
        pcm[i] = (int16_t)value;
    }
    return samples * sizeof(int16_t);
}

void audio_write_speaker(const uint8_t *src, size_t len)
{
    if (!tx_handle || !src || len < 2) return;
    len &= ~((size_t)1); static int32_t tx_buffer[1024]; static int16_t ref_pcm[512];
    const int16_t *pcm = reinterpret_cast<const int16_t *>(src); size_t total = len / sizeof(int16_t), offset = 0;
    constexpr size_t I2S_WRITE_SAMPLES = 240; constexpr uint32_t I2S_WRITE_TIMEOUT_MS = 50;
    while (offset < total) {
        const size_t old_offset = offset; size_t n = total - offset; if (n > I2S_WRITE_SAMPLES) n = I2S_WRITE_SAMPLES;
        for (size_t i = 0; i < n; ++i) tx_buffer[i] = static_cast<int32_t>(pcm[old_offset + i]) << 16;
        size_t written = 0; esp_err_t err = i2s_channel_write(tx_handle, tx_buffer, n * sizeof(int32_t), &written, I2S_WRITE_TIMEOUT_MS);
        size_t samples_written = written / sizeof(int32_t); if (samples_written > n) samples_written = n;
        if (aec_ready && samples_written > 0) { for (size_t i = 0; i < samples_written; ++i) ref_pcm[i] = pcm[old_offset + i]; aec_ref_push_24k(ref_pcm, samples_written); }
        offset += samples_written;
        if (err != ESP_OK || samples_written == 0) { ESP_LOGW(TAG, "I2S speaker write timeout/fail: err=%s written=%u/%u timeout=%ums", esp_err_to_name(err), (unsigned)written, (unsigned)(n * sizeof(int32_t)), (unsigned)I2S_WRITE_TIMEOUT_MS); vTaskDelay(1); return; }
    }
}

void audio_i2s_test_tone(void)
{
    static const int16_t sine_table[24] = {
        0, 2071, 4000, 5657, 6928, 7727, 8000, 7727, 6928, 5657, 4000, 2071,
        0, -2071, -4000, -5657, -6928, -7727, -8000, -6928, -5657, -4000, -2071
    };
    static int16_t tone[2400];

    if (!tx_handle) {
        return;
    }

    for (size_t i = 0; i < 2400; ++i) {
        tone[i] = sine_table[i % 24];
    }

    ESP_LOGI(TAG, "I2S TEST TONE: 1kHz PCM16 -> PCM32 I2S, 24kHz, 100ms");
    audio_write_speaker(reinterpret_cast<const uint8_t *>(tone), sizeof(tone));
}
