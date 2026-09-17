#include "audio_engine.h"

#include "audio_hal.h"
#include "wakeword.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <string.h>

namespace {

static const char *TAG = "AUDIO_ENGINE";
static constexpr size_t PCM_BLOCK_SAMPLES = 512;
static constexpr uint32_t WAKEWORD_TASK_DELAY_MS = 1;
static constexpr uint32_t WAKEWORD_TASK_STACK = 8192;

static constexpr size_t MIC_FRAME_SAMPLES = 320;
static constexpr size_t MIC_FRAME_BYTES = MIC_FRAME_SAMPLES * sizeof(int16_t);
static constexpr size_t MIC_QUEUE_DEPTH = 8;
static constexpr uint32_t CONVERSATION_TASK_STACK = 4096;

static int16_t s_pcm_buffer[PCM_BLOCK_SAMPLES];
static TaskHandle_t s_wakeword_task = nullptr;
static bool s_initialized = false;
static volatile bool s_wakeword_running = false;
static volatile bool s_wakeword_detected = false;

static TaskHandle_t s_conversation_task = nullptr;
static volatile bool s_conversation_running = false;
static StaticQueue_t s_mic_queue_storage;
static uint8_t s_mic_queue_buffer[MIC_QUEUE_DEPTH][MIC_FRAME_BYTES];
static QueueHandle_t s_mic_queue = nullptr;

static volatile bool s_playback_running = false;

static void wakeword_task(void *)
{
    while (s_wakeword_running) {
        if (audio_engine_process_wakeword()) {
            ESP_LOGI(TAG, "WakeWord event diterima AudioEngine");
        }
        vTaskDelay(pdMS_TO_TICKS(WAKEWORD_TASK_DELAY_MS));
    }

    s_wakeword_task = nullptr;
    vTaskDelete(nullptr);
}

static void conversation_task(void *)
{
    static int16_t read_buffer[PCM_BLOCK_SAMPLES];
    static uint8_t frame_buffer[MIC_FRAME_BYTES];
    size_t frame_pos = 0;

    ESP_LOGI(TAG,
             "Conversation MIC owner START: PCM16 mono 16kHz, frame=%uB (20ms), queue=%u, stack=%u",
             (unsigned)MIC_FRAME_BYTES,
             (unsigned)MIC_QUEUE_DEPTH,
             (unsigned)CONVERSATION_TASK_STACK);

    while (s_conversation_running) {
        size_t samples_read = 0;
        const esp_err_t err = audio_hal_read_pcm(
            read_buffer,
            PCM_BLOCK_SAMPLES,
            &samples_read);

        if (err != ESP_OK) {
            if (s_conversation_running) {
                ESP_LOGE(TAG, "Conversation MIC read gagal: %s", esp_err_to_name(err));
                vTaskDelay(pdMS_TO_TICKS(1));
            }
            continue;
        }

        if (samples_read == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        size_t offset = 0;
        const uint8_t *raw = reinterpret_cast<const uint8_t *>(read_buffer);
        const size_t bytes_read = samples_read * sizeof(int16_t);

        while (offset < bytes_read && s_conversation_running) {
            const size_t remaining_frame = MIC_FRAME_BYTES - frame_pos;
            const size_t remaining_input = bytes_read - offset;
            const size_t copy_len = remaining_frame < remaining_input
                                      ? remaining_frame
                                      : remaining_input;

            memcpy(frame_buffer + frame_pos, raw + offset, copy_len);
            frame_pos += copy_len;
            offset += copy_len;

            if (frame_pos != MIC_FRAME_BYTES) continue;

            frame_pos = 0;
            if (xQueueSend(s_mic_queue, frame_buffer, 0) != pdTRUE) {
                static uint32_t drops = 0;
                ++drops;
                if ((drops & 0x3FU) == 1U) {
                    ESP_LOGW(TAG,
                             "Conversation MIC queue penuh; frame drop total=%u",
                             (unsigned)drops);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }

    s_conversation_task = nullptr;
    ESP_LOGI(TAG, "Conversation MIC owner STOP");
    vTaskDelete(nullptr);
}

static bool stop_wakeword_and_wait(void)
{
    if (!s_wakeword_running && s_wakeword_task == nullptr) return true;

    s_wakeword_running = false;
    (void)audio_hal_stop_capture();

    for (uint32_t i = 0; i < 100 && s_wakeword_task != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (s_wakeword_task != nullptr) {
        ESP_LOGE(TAG, "WakeWord task belum berhenti; MIC ownership tetap dikunci");
        return false;
    }

    return true;
}

} // namespace

extern "C" bool audio_engine_init(void)
{
    if (s_initialized) return true;

    ESP_LOGI(TAG, "Initializing AudioEngine");
    audio_hal_init();

    if (!wakeword_init()) {
        ESP_LOGE(TAG, "WakeWord initialization failed");
        return false;
    }

    if (wakeword_get_sample_rate() != 16000) {
        ESP_LOGE(TAG, "WakeWord sample rate mismatch: %dHz",
                 wakeword_get_sample_rate());
        wakeword_deinit();
        return false;
    }

    s_mic_queue = xQueueCreateStatic(
        MIC_QUEUE_DEPTH,
        MIC_FRAME_BYTES,
        &s_mic_queue_buffer[0][0],
        &s_mic_queue_storage);
    if (!s_mic_queue) {
        ESP_LOGE(TAG, "Gagal membuat conversation MIC queue");
        wakeword_deinit();
        return false;
    }

    s_wakeword_detected = false;
    s_wakeword_running = false;
    s_wakeword_task = nullptr;
    s_conversation_running = false;
    s_conversation_task = nullptr;
    s_playback_running = false;
    s_initialized = true;

    ESP_LOGI(TAG, "AudioEngine ready: MIC -> Audio HAL -> WakeNet");
    return true;
}

extern "C" bool audio_engine_start_wakeword(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "AudioEngine belum diinisialisasi");
        return false;
    }
    if (s_wakeword_running) return true;
    if (s_conversation_running) {
        ESP_LOGE(TAG, "Tidak bisa start WakeWord saat conversation MIC aktif");
        return false;
    }

    s_wakeword_detected = false;
    const esp_err_t err = audio_hal_start_capture();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Gagal start MIC capture: %s", esp_err_to_name(err));
        return false;
    }

    s_wakeword_running = true;
    const BaseType_t result = xTaskCreate(
        wakeword_task, "wakeword_task", WAKEWORD_TASK_STACK,
        nullptr, 6, &s_wakeword_task);

    if (result != pdPASS) {
        s_wakeword_running = false;
        audio_hal_stop_capture();
        ESP_LOGE(TAG, "Gagal membuat WakeWord task");
        return false;
    }

    ESP_LOGI(TAG, "WakeWord capture START");
    return true;
}

extern "C" void audio_engine_stop_wakeword(void)
{
    if (!s_wakeword_running && s_wakeword_task == nullptr) return;
    s_wakeword_running = false;
    (void)audio_hal_stop_capture();
    ESP_LOGI(TAG, "WakeWord capture STOP");
}

extern "C" bool audio_engine_process_wakeword(void)
{
    if (!s_initialized || !s_wakeword_running) return false;

    size_t samples_read = 0;
    const esp_err_t err = audio_hal_read_pcm(
        s_pcm_buffer, PCM_BLOCK_SAMPLES, &samples_read);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Audio HAL read gagal: %s", esp_err_to_name(err));
        return false;
    }
    if (samples_read == 0) return false;

    if (wakeword_process_pcm16(s_pcm_buffer, samples_read)) {
        s_wakeword_detected = true;
        return true;
    }
    return false;
}

extern "C" bool audio_engine_wakeword_detected(void)
{
    return s_wakeword_detected;
}

extern "C" void audio_engine_clear_wakeword(void)
{
    s_wakeword_detected = false;
}

extern "C" void audio_engine_stop(void)
{
    audio_engine_stop_wakeword();
    audio_engine_stop_conversation();
    audio_engine_stop_playback();
}

extern "C" bool audio_engine_start_conversation(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "AudioEngine belum diinisialisasi");
        return false;
    }
    if (s_conversation_running) return true;

    if (!stop_wakeword_and_wait()) return false;

    if (audio_hal_start_capture() != ESP_OK) {
        ESP_LOGE(TAG, "Gagal start MIC capture untuk conversation");
        return false;
    }

    xQueueReset(s_mic_queue);
    s_conversation_running = true;

    const BaseType_t result = xTaskCreate(
        conversation_task,
        "audio_conversation",
        CONVERSATION_TASK_STACK,
        nullptr,
        5,
        &s_conversation_task);

    if (result != pdPASS) {
        s_conversation_running = false;
        audio_hal_stop_capture();
        s_conversation_task = nullptr;
        ESP_LOGE(TAG, "Gagal membuat conversation MIC task");
        return false;
    }

    ESP_LOGI(TAG, "Conversation audio START: AudioEngine owns MIC");
    return true;
}

extern "C" void audio_engine_stop_conversation(void)
{
    if (!s_conversation_running && s_conversation_task == nullptr) return;

    s_conversation_running = false;
    (void)audio_hal_stop_capture();

    for (uint32_t i = 0; i < 100 && s_conversation_task != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    xQueueReset(s_mic_queue);
    ESP_LOGI(TAG, "Conversation audio STOP: MIC released");
}

extern "C" bool audio_engine_conversation_active(void)
{
    return s_conversation_running;
}

extern "C" bool audio_engine_read_mic_frame(
    int16_t *buffer,
    size_t samples,
    uint32_t timeout_ms)
{
    if (!buffer || samples != MIC_FRAME_SAMPLES || !s_mic_queue) return false;

    TickType_t wait_ticks = 0;
    if (timeout_ms == UINT32_MAX) {
        wait_ticks = portMAX_DELAY;
    } else {
        wait_ticks = pdMS_TO_TICKS(timeout_ms);
    }

    return xQueueReceive(s_mic_queue, buffer, wait_ticks) == pdTRUE;
}

extern "C" size_t audio_engine_mic_frame_samples(void)
{
    return MIC_FRAME_SAMPLES;
}

extern "C" bool audio_engine_start_playback(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "AudioEngine belum diinisialisasi");
        return false;
    }
    if (s_playback_running) return true;

    const esp_err_t err = audio_hal_start_playback();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Gagal start speaker playback: %s", esp_err_to_name(err));
        return false;
    }

    s_playback_running = true;
    ESP_LOGI(TAG, "Speaker playback START: direct Audio HAL writer");
    return true;
}

extern "C" void audio_engine_stop_playback(void)
{
    if (!s_playback_running) return;

    s_playback_running = false;
    const esp_err_t err = audio_hal_stop_playback();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Speaker playback STOP gagal: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "Speaker playback STOP: AudioEngine released SPK");
}

extern "C" bool audio_engine_playback_active(void)
{
    return s_playback_running;
}

extern "C" bool audio_engine_write_speaker_pcm(
    const int16_t *buffer,
    size_t samples,
    uint32_t timeout_ms)
{
    if (!buffer || samples == 0 || !s_playback_running) return false;

    size_t offset = 0;
    while (offset < samples && s_playback_running) {
        const size_t chunk = (samples - offset) > PCM_BLOCK_SAMPLES
                           ? PCM_BLOCK_SAMPLES
                           : (samples - offset);

        size_t written = 0;
        const esp_err_t err = audio_hal_write_pcm(
            buffer + offset,
            chunk,
            &written);

        if (err != ESP_OK) {
            ESP_LOGW(TAG,
                     "Speaker I2S write gagal: offset=%u chunk=%u written=%u err=%s",
                     (unsigned)offset,
                     (unsigned)chunk,
                     (unsigned)written,
                     esp_err_to_name(err));
            return false;
        }

        if (written == 0) {
            ESP_LOGW(TAG, "Speaker I2S write no-progress: offset=%u chunk=%u",
                     (unsigned)offset,
                     (unsigned)chunk);
            return false;
        }

        offset += written;
    }

    if (offset != samples) {
        ESP_LOGW(TAG, "Speaker PCM interrupted: written=%u/%u",
                 (unsigned)offset, (unsigned)samples);
        return false;
    }

    (void)timeout_ms;
    return true;
}
