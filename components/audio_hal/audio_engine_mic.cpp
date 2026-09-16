#include "audio_engine.h"
#include "audio_hal.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <string.h>

static const char *TAG = "AUDIO_ENGINE_MIC";

static constexpr size_t MIC_FRAME_BYTES = 320U; // 20 ms @ 16 kHz PCM16 mono
static constexpr size_t MIC_TX_BATCH_BYTES = 1600U; // 100 ms, matching Repo3 WS transport cadence
static constexpr size_t MIC_READ_BYTES = 4096U;
static constexpr uint32_t MIC_IDLE_TIMEOUT_MS = 60000U;
static constexpr uint32_t MIC_VAD_HANGOVER_MS = 500U;
static constexpr int32_t MIC_ACTIVITY_THRESHOLD = 80;
static constexpr size_t MIC_ACTIVITY_MIN_SAMPLES = 8U;
static constexpr size_t MIC_TX_QUEUE_DEPTH = 16U;

static audio_engine_mic_frame_cb_t s_mic_listener = nullptr;
static void *s_mic_listener_ctx = nullptr;
static audio_engine_mic_sink_cb_t s_mic_sink = nullptr;
static void *s_mic_sink_ctx = nullptr;
static volatile bool s_capture_started = false;
static volatile bool s_input_session_active = false;
static int64_t s_last_activity_us = 0;
static TaskHandle_t s_capture_task = nullptr;
static TaskHandle_t s_sink_task = nullptr;

static StaticQueue_t s_tx_queue_struct;
static uint8_t s_tx_queue_storage[MIC_TX_QUEUE_DEPTH][MIC_TX_BATCH_BYTES];
static QueueHandle_t s_tx_queue = nullptr;
static uint32_t s_tx_queue_drops = 0;

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

static void flush_mic_tx_queue(void)
{
    if (s_tx_queue)
        (void)xQueueReset(s_tx_queue);
}

static void sink_task(void *arg)
{
    (void)arg;
    uint8_t batch[MIC_TX_BATCH_BYTES];
    ESP_LOGI(TAG, "Mic transport worker aktif; capture/WakeNet tidak mengerjakan TX");

    for (;;) {
        if (xQueueReceive(s_tx_queue, batch, portMAX_DELAY) != pdTRUE)
            continue;

        audio_engine_mic_sink_cb_t sink = s_mic_sink;
        void *sink_ctx = s_mic_sink_ctx;

        /* Race-safe second gate: a batch can already be queued when Gemini
         * starts speaking. Never transmit stale microphone audio in that turn. */
        if (!sink || !s_input_session_active || audio_engine_turn_active())
            continue;

        sink(batch, MIC_TX_BATCH_BYTES, sink_ctx);
    }
}

static void capture_task(void *arg)
{
    (void)arg;

    static uint8_t read_buffer[MIC_READ_BYTES];
    static uint8_t frame_buffer[MIC_FRAME_BYTES];
    static uint8_t tx_batch[MIC_TX_BATCH_BYTES];
    size_t frame_pos = 0;
    size_t tx_batch_pos = 0;
    bool tx_batch_has_activity = false;
    bool tx_speech_active = false;

    ESP_LOGI(TAG, "Mic capture owner aktif: PCM16 16kHz, frame=%uB, TX batch=%uB, read=%uB, idle=%ums, stack=8192",
             (unsigned)MIC_FRAME_BYTES, (unsigned)MIC_TX_BATCH_BYTES,
             (unsigned)MIC_READ_BYTES, (unsigned)MIC_IDLE_TIMEOUT_MS);

    for (;;) {
        const size_t bytes = audio_read_mic(read_buffer, sizeof(read_buffer));
        if (bytes == 0) {
            vTaskDelay(1);
            continue;
        }

        /* WakeNet receives the same contiguous PCM read produced by Audio HAL. */
        if (s_mic_listener)
            s_mic_listener(read_buffer, bytes, s_mic_listener_ctx);

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

            if (!s_input_session_active) {
                tx_batch_pos = 0;
                tx_batch_has_activity = false;
                tx_speech_active = false;
                vTaskDelay(1);
                continue;
            }

            /* Keep the existing capture-side model-turn gate. */
            if (audio_engine_turn_active()) {
                tx_batch_pos = 0;
                tx_batch_has_activity = false;
                tx_speech_active = false;
                vTaskDelay(1);
                continue;
            }

            const bool active = frame_has_activity(frame_buffer, MIC_FRAME_BYTES);
            const int64_t now_us = esp_timer_get_time();
            if (active) {
                s_last_activity_us = now_us;
                tx_speech_active = true;
                tx_batch_has_activity = true;
            }

            if (s_last_activity_us != 0 &&
                now_us - s_last_activity_us >= (int64_t)MIC_IDLE_TIMEOUT_MS * 1000LL) {
                ESP_LOGI(TAG, "Input idle %ums: AudioEngine mengakhiri sesi MIC",
                         (unsigned)MIC_IDLE_TIMEOUT_MS);
                s_input_session_active = false;
                tx_batch_pos = 0;
                tx_batch_has_activity = false;
                tx_speech_active = false;
                flush_mic_tx_queue();
                vTaskDelay(1);
                continue;
            }

            /* Preserve a short trailing silence window so Gemini's server VAD
             * can observe end-of-speech. Repo4 no longer floods the TX queue
             * with silence before speech or long after speech has ended. */
            if (tx_speech_active &&
                s_last_activity_us != 0 &&
                now_us - s_last_activity_us >= (int64_t)MIC_VAD_HANGOVER_MS * 1000LL) {
                tx_speech_active = false;
            }

            if (tx_speech_active || tx_batch_has_activity) {
                memcpy(tx_batch + tx_batch_pos, frame_buffer, MIC_FRAME_BYTES);
                tx_batch_pos += MIC_FRAME_BYTES;
            }

            if (tx_batch_pos == MIC_TX_BATCH_BYTES) {
                if (s_tx_queue && s_mic_sink && tx_batch_has_activity &&
                    xQueueSend(s_tx_queue, tx_batch, 0) != pdTRUE) {
                    ++s_tx_queue_drops;
                    if ((s_tx_queue_drops & 0x3FU) == 1U)
                        ESP_LOGW(TAG, "MIC transport queue penuh; batch drop total=%u",
                                 (unsigned)s_tx_queue_drops);
                }
                tx_batch_pos = 0;
                tx_batch_has_activity = false;
            }

            vTaskDelay(1);
        }
    }
}

bool audio_engine_set_mic_listener(audio_engine_mic_frame_cb_t cb, void *ctx)
{
    s_mic_listener = cb;
    s_mic_listener_ctx = ctx;
    return true;
}

bool audio_engine_set_mic_sink(audio_engine_mic_sink_cb_t cb, void *ctx)
{
    s_mic_sink = cb;
    s_mic_sink_ctx = ctx;
    return true;
}

bool audio_engine_start_capture(void)
{
    if (s_capture_started) return true;

    s_tx_queue = xQueueCreateStatic(
        MIC_TX_QUEUE_DEPTH,
        MIC_TX_BATCH_BYTES,
        &s_tx_queue_storage[0][0],
        &s_tx_queue_struct);
    if (!s_tx_queue) {
        ESP_LOGE(TAG, "Gagal membuat MIC transport queue");
        return false;
    }

    BaseType_t sink_rc = xTaskCreatePinnedToCore(
        sink_task, "mic_tx", 4096, nullptr, 4, &s_sink_task, 0);
    if (sink_rc != pdPASS) {
        s_sink_task = nullptr;
        s_tx_queue = nullptr;
        ESP_LOGE(TAG, "Gagal membuat MIC transport worker");
        return false;
    }

    BaseType_t rc = xTaskCreatePinnedToCore(
        capture_task, "audio_capture", 8192, nullptr, 5, &s_capture_task, 1);
    if (rc != pdPASS) {
        s_capture_task = nullptr;
        s_sink_task = nullptr;
        s_tx_queue = nullptr;
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

    flush_mic_tx_queue();
    s_last_activity_us = esp_timer_get_time();
    s_input_session_active = true;
    ESP_LOGI(TAG, "MIC session START: AudioEngine -> transport");
}

void audio_engine_stop_input_session(void)
{
    if (!s_input_session_active) {
        flush_mic_tx_queue();
        return;
    }

    s_input_session_active = false;
    flush_mic_tx_queue();
    ESP_LOGI(TAG, "MIC session STOP: AudioEngine");
}

bool audio_engine_input_session_active(void)
{
    return s_input_session_active;
}
