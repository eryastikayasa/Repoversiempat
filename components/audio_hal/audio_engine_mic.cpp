#include "audio_engine.h"
#include "audio_hal.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "AUDIO_ENGINE_MIC";

static constexpr size_t MIC_FRAME_BYTES = 320U; // 20 ms @ 16 kHz PCM16 mono
static constexpr size_t MIC_READ_BYTES = 4096U;
static constexpr uint32_t MIC_IDLE_TIMEOUT_MS = 60000U;
static constexpr int32_t MIC_ACTIVITY_THRESHOLD = 80;
static constexpr size_t MIC_ACTIVITY_MIN_SAMPLES = 8U;

/*
 * Transport is deliberately inverted: AudioEngine owns capture/framing,
 * while the transport supplies only this sink. No WebSocket header is needed
 * here and AudioEngine remains independent of the transport implementation.
 */
extern "C" void websocket_send_audio_data(const uint8_t *data, size_t len)
    __attribute__((weak));

static audio_engine_mic_frame_cb_t s_mic_listener = nullptr;
static void *s_mic_listener_ctx = nullptr;
static volatile bool s_capture_started = false;
static volatile bool s_input_session_active = false;
static int64_t s_last_activity_us = 0;
static TaskHandle_t s_capture_task = nullptr;

static bool frame_has_activity(const uint8_t *data, size_t len)
{
    if (!data || len < 2) return false;

    size_t active_samples = 0;
    for (size_t i = 0; i + 1 < len; i += 2) {
        const int16_t sample = (int16_t)((uint16_t)data[i] |
                                         ((uint16_t)data[i + 1] << 8));
        const int32_t magnitude = sample < 0 ? -(int32_t)sample : (int32_t)sample;
        if (magnitude >= MIC_ACTIVITY_THRESHOLD && ++active_samples >= MIC_ACTIVITY_MIN_SAMPLES)
            return true;
    }
    return false;
}

static void capture_task(void *arg)
{
    (void)arg;

    static uint8_t read_buffer[MIC_READ_BYTES];
    static uint8_t frame_buffer[MIC_FRAME_BYTES];
    size_t frame_pos = 0;

    ESP_LOGI(TAG, "Mic capture owner aktif: PCM16 16kHz, frame=%uB, idle=%ums",
             (unsigned)MIC_FRAME_BYTES, (unsigned)MIC_IDLE_TIMEOUT_MS);

    for (;;) {
        const size_t bytes = audio_read_mic(read_buffer, sizeof(read_buffer));
        if (bytes == 0) {
            vTaskDelay(1);
            continue;
        }

        size_t offset = 0;
        while (offset < bytes) {
            const size_t copy_len = (MIC_FRAME_BYTES - frame_pos < bytes - offset)
                                        ? (MIC_FRAME_BYTES - frame_pos)
                                        : (bytes - offset);
            memcpy(frame_buffer + frame_pos, read_buffer + offset, copy_len);
            frame_pos += copy_len;
            offset += copy_len;

            if (frame_pos != MIC_FRAME_BYTES) continue;
            frame_pos = 0;

            /* Wake-word/application listener receives the same AudioEngine-owned frame. */
            if (s_mic_listener)
                s_mic_listener(frame_buffer, MIC_FRAME_BYTES, s_mic_listener_ctx);

            if (!s_input_session_active) continue;

            if (frame_has_activity(frame_buffer, MIC_FRAME_BYTES))
                s_last_activity_us = esp_timer_get_time();

            const int64_t now_us = esp_timer_get_time();
            if (s_last_activity_us != 0 &&
                now_us - s_last_activity_us >= (int64_t)MIC_IDLE_TIMEOUT_MS * 1000LL) {
                ESP_LOGI(TAG, "Input idle %ums: AudioEngine mengakhiri sesi MIC",
                         (unsigned)MIC_IDLE_TIMEOUT_MS);
                s_input_session_active = false;
                continue;
            }

            if (websocket_send_audio_data)
                websocket_send_audio_data(frame_buffer, MIC_FRAME_BYTES);
        }
    }
}

bool audio_engine_set_mic_listener(audio_engine_mic_frame_cb_t cb, void *ctx)
{
    s_mic_listener = cb;
    s_mic_listener_ctx = ctx;
    return true;
}

bool audio_engine_start_capture(void)
{
    if (s_capture_started) return true;

    BaseType_t rc = xTaskCreatePinnedToCore(
        capture_task, "audio_capture", 4096, nullptr, 5, &s_capture_task, 0);
    if (rc != pdPASS) {
        s_capture_task = nullptr;
        ESP_LOGE(TAG, "Gagal membuat AudioEngine capture task");
        return false;
    }

    s_capture_started = true;
    return true;
}

void audio_engine_start_input_session(void)
{
    if (!s_capture_started) {
        ESP_LOGW(TAG, "start_input_session sebelum capture aktif");
        return;
    }
    s_last_activity_us = esp_timer_get_time();
    s_input_session_active = true;
    ESP_LOGI(TAG, "MIC session START: AudioEngine -> transport");
}

void audio_engine_stop_input_session(void)
{
    if (!s_input_session_active) return;
    s_input_session_active = false;
    ESP_LOGI(TAG, "MIC session STOP: AudioEngine");
}

bool audio_engine_input_session_active(void)
{
    return s_input_session_active;
}
