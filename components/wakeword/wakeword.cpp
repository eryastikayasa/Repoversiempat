#include "wakeword.h"

#include "esp_log.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"

#include <string.h>

namespace {

static const char *TAG = "WAKEWORD";
static constexpr char MODEL_NAME[] = "wn9_hiesp";
static constexpr size_t BUFFER_SAMPLES = 1024;

static srmodel_list_t *s_models = nullptr;
static const esp_wn_iface_t *s_iface = nullptr;
static model_iface_data_t *s_model = nullptr;
static int s_chunk_samples = 0;
static int s_sample_rate = 0;
static int s_channels = 0;
static int16_t s_buffer[BUFFER_SAMPLES];
static size_t s_buffer_samples = 0;

static void reset_state()
{
    s_buffer_samples = 0;
}

} // namespace

extern "C" bool wakeword_init(void)
{
    if (s_model && s_iface) {
        return true;
    }

    ESP_LOGI(TAG, "Initializing ESP-SR WakeNet9: %s", MODEL_NAME);

    s_models = esp_srmodel_init("model");
    if (!s_models) {
        ESP_LOGE(TAG, "ESP-SR model partition 'model' tidak tersedia");
        return false;
    }

    if (esp_srmodel_exists(s_models, (char *)MODEL_NAME) < 0) {
        ESP_LOGE(TAG, "WakeNet model tidak ditemukan: %s", MODEL_NAME);
        esp_srmodel_deinit(s_models);
        s_models = nullptr;
        return false;
    }

    s_iface = esp_wn_handle_from_name(MODEL_NAME);
    if (!s_iface) {
        ESP_LOGE(TAG, "WakeNet interface tidak ditemukan: %s", MODEL_NAME);
        esp_srmodel_deinit(s_models);
        s_models = nullptr;
        return false;
    }

    s_model = s_iface->create(MODEL_NAME, DET_MODE_90);
    if (!s_model) {
        ESP_LOGE(TAG, "Gagal membuat WakeNet model: %s", MODEL_NAME);
        s_iface = nullptr;
        esp_srmodel_deinit(s_models);
        s_models = nullptr;
        return false;
    }

    s_chunk_samples = s_iface->get_samp_chunksize(s_model);
    s_sample_rate = s_iface->get_samp_rate(s_model);
    s_channels = s_iface->get_channel_num(s_model);
    reset_state();

    ESP_LOGI(TAG, "WakeNet ready: model=%s rate=%dHz chunk=%d channels=%d",
             MODEL_NAME, s_sample_rate, s_chunk_samples, s_channels);

    if (s_sample_rate != 16000 || s_channels != 1 || s_chunk_samples <= 0 ||
        s_chunk_samples > static_cast<int>(BUFFER_SAMPLES)) {
        ESP_LOGE(TAG, "WakeNet audio configuration tidak kompatibel");
        wakeword_deinit();
        return false;
    }

    ESP_LOGI(TAG, "Wake word aktif: HI, ESP");
    return true;
}

extern "C" bool wakeword_process_pcm16(const int16_t *samples, size_t sample_count)
{
    if (!samples || sample_count == 0 || !s_iface || !s_model || s_chunk_samples <= 0) {
        return false;
    }

    bool detected = false;

    while (sample_count > 0) {
        const size_t free_samples = BUFFER_SAMPLES - s_buffer_samples;
        const size_t copy_samples = sample_count < free_samples ? sample_count : free_samples;

        memcpy(s_buffer + s_buffer_samples, samples, copy_samples * sizeof(int16_t));
        s_buffer_samples += copy_samples;
        samples += copy_samples;
        sample_count -= copy_samples;

        while (s_buffer_samples >= static_cast<size_t>(s_chunk_samples)) {
            const int result = s_iface->detect(s_model, s_buffer);
            if (result > 0) {
                ESP_LOGW(TAG, "WAKE WORD TERDETEKSI: HI, ESP (id=%d)", result);
                detected = true;
                reset_state();
                return true;
            }

            const size_t remaining = s_buffer_samples - static_cast<size_t>(s_chunk_samples);
            if (remaining > 0) {
                memmove(s_buffer,
                        s_buffer + s_chunk_samples,
                        remaining * sizeof(int16_t));
            }
            s_buffer_samples = remaining;
        }
    }

    return detected;
}

extern "C" int wakeword_get_chunk_samples(void)
{
    return s_chunk_samples;
}

extern "C" int wakeword_get_sample_rate(void)
{
    return s_sample_rate;
}

extern "C" void wakeword_deinit(void)
{
    reset_state();

    if (s_model && s_iface) {
        s_iface->destroy(s_model);
    }

    s_model = nullptr;
    s_iface = nullptr;
    s_chunk_samples = 0;
    s_sample_rate = 0;
    s_channels = 0;

    if (s_models) {
        esp_srmodel_deinit(s_models);
        s_models = nullptr;
    }
}
