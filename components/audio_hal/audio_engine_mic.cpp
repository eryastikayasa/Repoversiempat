#include "audio_engine.h"
#include "audio_hal.h"
#include "wakeword.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <string.h>

static const char *TAG = "AUDIO_ENGINE_MIC";

static constexpr size_t WAKEWORD_READ_SAMPLES = 512U;
static constexpr uint32_t WAKEWORD_TASK_STACK = 8192U;
static constexpr UBaseType_t WAKEWORD_TASK_PRIORITY = 6U;

static constexpr size_t MIC_FRAME_BYTES = 320U; // 20 ms @ 16 kHz PCM16 mono
static constexpr size_t MIC_TX_BATCH_BYTES = 1600U; // 100 ms
static constexpr size_t MIC_READ_BYTES = 4096U; // 1024 PCM16 samples
static constexpr uint32_t MIC_IDLE_TIMEOUT_MS = 60000U;
static constexpr uint32_t MIC_VAD_HANGOVER_MS = 500U;
static constexpr int32_t MIC_ACTIVITY_THRESHOLD = 80;
static constexpr size_t MIC_ACTIVITY_MIN_SAMPLES = 8U;
static constexpr size_t MIC_TX_QUEUE_DEPTH = 16U;
static constexpr uint32_t CONVERSATION_TASK_STACK = 8192U;
static constexpr UBaseType_t CONVERSATION_TASK_PRIORITY = 5U;

static audio_engine_mic_sink_cb_t s_mic_sink = nullptr;
static void *s_mic_sink_ctx = nullptr;
static volatile bool s_capture_started = false;
static volatile bool s_input_session_active = false;
static volatile bool s_wakeword_running = false;
static volatile bool s_wakeword_detected = false;
static int64_t s_last_activity_us = 0;
static TaskHandle_t s_wakeword_task = nullptr;
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
    if (s_tx_queue) (void)xQueueReset(s_tx_queue);
}

static void wakeword_task(void *arg)
{
    (void)arg;
    static int16_t pcm[WAKEWORD_READ_SAMPLES];

    ESP_LOGI(TAG,
             "WakeWord task START: model=wn9_hiesp rate=16000Hz chunk=%d stack=%u priority=%u",
             wakeword_get_chunk_samples(),
             (unsigned)WAKEWORD_TASK_STACK,
             (unsigned)WAKEWORD_TASK_PRIORITY);

    while (s_wakeword_running) {
        size_t samples_read = 0;
        const esp_err_t err = audio_hal_read_pcm(
            pcm, WAKEWORD_READ_SAMPLES, &samples_read);

        if (err != ESP_OK) {
            if (s_wakeword_running) {
                ESP_LOGE(TAG, "WakeWord MIC read gagal: %s", esp_err_to_name(err));
                vTaskDelay(1);
            }
            continue;
        }

        if (samples_read == 0) {
            vTaskDelay(1);
            continue;
        }

        if (wakeword_process_pcm16(pcm, samples_read)) {
            s_wakeword_detected = true;
            ESP_LOGI(TAG, "WakeWord event diterima AudioEngine");
        }

        vTaskDelay(1);
    }

    s_wakeword_task = nullptr;
    ESP_LOGI(TAG, "WakeWord task STOP");
    vTaskDelete(nullptr);
}

static bool stop_wakeword_and_wait(void)
{
    if (!s_wakeword_running && s_wakeword_task == nullptr) return true;

    s_wakeword_running = false;
    (void)audio_hal_stop_capture();

    for (uint32_t i = 0; i < 200 && s_wakeword_task != nullptr; ++i)
        vTaskDelay(1);

    if (s_wakeword_task != nullptr) {
        ESP_LOGE(TAG, "WakeWord task belum berhenti; MIC ownership tetap dikunci");
        return false;
    }
    return true;
}

static void sink_task(void *arg)
{
    (void)arg;
    uint8_t batch[MIC_TX_BATCH_BYTES];
    ESP_LOGI(TAG, "Mic transport worker aktif; WakeWord/conversation tidak mengerjakan TX");

    for (;;) {
        if (xQueueReceive(s_tx_queue, batch, portMAX_DELAY) != pdTRUE)
            continue;

        audio_engine_mic_sink_cb_t sink = s_mic_sink;
        void *sink_ctx = s_mic_sink_ctx;

        if (!sink || !s_input_session_active || audio_engine_turn_active())
            continue;

        sink(batch, MIC_TX_BATCH_BYTES, sink_ctx);
    }
}

static void conversation_task(void *arg)
{
    (void)arg;
    static uint8_t read_buffer[MIC_READ_BYTES];
    static uint8_t frame_buffer[MIC_FRAME_BYTES];
    static uint8_t tx_batch[MIC_TX_BATCH_BYTES];
    size_t frame_pos = 0;
    size_t tx_batch_pos = 0;
    bool tx_batch_has_activity = false;
    bool tx_speech_active = false;

    auto reset_tx_batch = [&]() {
        tx_batch_pos = 0;
        tx_batch_has_activity = false;
        memset(tx_batch, 0, sizeof(tx_batch));
    };

    reset_tx_batch();
    ESP_LOGI(TAG,
             "Conversation MIC owner START: PCM16 16kHz frame=%uB TX batch=%uB read=%uB stack=%u",
             (unsigned)MIC_FRAME_BYTES,
             (unsigned)MIC_TX_BATCH_BYTES,
             (unsigned)MIC_READ_BYTES,
             (unsigned)CONVERSATION_TASK_STACK);

    while (s_input_session_active) {
        size_t samples_read = 0;
        const esp_err_t err = audio_hal_read_pcm(
            reinterpret_cast<int16_t *>(read_buffer),
            MIC_READ_BYTES / sizeof(int16_t),
            &samples_read);

        if (err != ESP_OK) {
            if (s_input_session_active) {
                ESP_LOGE(TAG, "Conversation MIC read gagal: %s", esp_err_to_name(err));
                vTaskDelay(1);
            }
            continue;
        }
        if (samples_read == 0) {
            vTaskDelay(1);
            continue;
        }

        const size_t bytes_read = samples_read * sizeof(int16_t);
        size_t offset = 0;
        while (offset < bytes_read && s_input_session_active) {
            const size_t copy_len = (MIC_FRAME_BYTES - frame_pos < bytes_read - offset)
                                  ? (MIC_FRAME_BYTES - frame_pos)
                                  : (bytes_read - offset);
            memcpy(frame_buffer + frame_pos, read_buffer + offset, copy_len);
            frame_pos += copy_len;
            offset += copy_len;

            if (frame_pos != MIC_FRAME_BYTES) continue;
            frame_pos = 0;

            if (audio_engine_turn_active()) {
                reset_tx_batch();
                tx_speech_active = false;
                continue;
            }

            const bool active = frame_has_activity(frame_buffer, MIC_FRAME_BYTES);
            const int64_t now_us = esp_timer_get_time();
            if (active) {
                s_last_activity_us = now_us;
                tx_speech_active = true;
            }

            if (s_last_activity_us != 0 &&
                now_us - s_last_activity_us >= (int64_t)MIC_IDLE_TIMEOUT_MS * 1000LL) {
                ESP_LOGI(TAG, "Input idle %ums: AudioEngine mengakhiri sesi MIC",
                         (unsigned)MIC_IDLE_TIMEOUT_MS);
                s_input_session_active = false;
                reset_tx_batch();
                tx_speech_active = false;
                flush_mic_tx_queue();
                break;
            }

            if (tx_speech_active && s_last_activity_us != 0 &&
                now_us - s_last_activity_us >= (int64_t)MIC_VAD_HANGOVER_MS * 1000LL) {
                tx_speech_active = false;
            }

            if (tx_speech_active) {
                memcpy(tx_batch + tx_batch_pos, frame_buffer, MIC_FRAME_BYTES);
                tx_batch_pos += MIC_FRAME_BYTES;
                tx_batch_has_activity = true;
            }

            const bool tail_expired = !tx_speech_active && tx_batch_has_activity;
            if (tx_batch_pos == MIC_TX_BATCH_BYTES || tail_expired) {
                if (s_tx_queue && s_mic_sink && tx_batch_has_activity &&
                    xQueueSend(s_tx_queue, tx_batch, 0) != pdTRUE) {
                    ++s_tx_queue_drops;
                    if ((s_tx_queue_drops & 0x3FU) == 1U)
                        ESP_LOGW(TAG, "MIC transport queue penuh; batch drop total=%u",
                                 (unsigned)s_tx_queue_drops);
                }
                reset_tx_batch();
            }
        }

        vTaskDelay(1);
    }

    s_capture_task = nullptr;
    ESP_LOGI(TAG, "Conversation MIC owner STOP");
    vTaskDelete(nullptr);
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

    const BaseType_t sink_rc = xTaskCreatePinnedToCore(
        sink_task, "mic_tx", 4096, nullptr, 4, &s_sink_task, 0);
    if (sink_rc != pdPASS) {
        s_sink_task = nullptr;
        s_tx_queue = nullptr;
        return false;
    }

    s_capture_started = true;
    s_wakeword_detected = false;
    ESP_LOGI(TAG, "AudioEngine MIC subsystem READY; WakeWord is idle-mode MIC owner");
    return true;
}

bool audio_engine_start_wakeword(void)
{
    if (!s_capture_started) {
        ESP_LOGE(TAG, "start_wakeword sebelum audio_engine_start_capture");
        return false;
    }
    if (s_wakeword_running) return true;
    if (s_input_session_active) {
        ESP_LOGE(TAG, "Tidak bisa start WakeWord saat Gemini MIC aktif");
        return false;
    }

    if (!wakeword_init()) {
        ESP_LOGE(TAG, "WakeWord initialization failed");
        return false;
    }
    if (wakeword_get_sample_rate() != 16000 || wakeword_get_chunk_samples() != 512) {
        ESP_LOGE(TAG, "WakeWord configuration mismatch: rate=%d chunk=%d",
                 wakeword_get_sample_rate(), wakeword_get_chunk_samples());
        return false;
    }

    flush_mic_tx_queue();
    s_wakeword_detected = false;

    if (audio_hal_start_capture() != ESP_OK) {
        ESP_LOGE(TAG, "Gagal start MIC capture untuk WakeWord");
        return false;
    }

    s_wakeword_running = true;
    const BaseType_t rc = xTaskCreate(
        wakeword_task, "wakeword_task", WAKEWORD_TASK_STACK,
        nullptr, WAKEWORD_TASK_PRIORITY, &s_wakeword_task);
    if (rc != pdPASS) {
        s_wakeword_running = false;
        s_wakeword_task = nullptr;
        (void)audio_hal_stop_capture();
        ESP_LOGE(TAG, "Gagal membuat WakeWord task");
        return false;
    }

    ESP_LOGI(TAG, "WakeWord capture START: AudioEngine -> Repo5 WakeWord engine");
    return true;
}

void audio_engine_stop_wakeword(void)
{
    if (!s_wakeword_running && s_wakeword_task == nullptr) return;
    (void)stop_wakeword_and_wait();
    s_wakeword_detected = false;
    ESP_LOGI(TAG, "WakeWord capture STOP: MIC released");
}

bool audio_engine_wakeword_detected(void)
{
    return s_wakeword_detected;
}

void audio_engine_clear_wakeword(void)
{
    s_wakeword_detected = false;
}

void audio_engine_start_input_session(void)
{
    if (!s_capture_started) {
        ESP_LOGW(TAG, "start_input_session sebelum capture subsystem aktif");
        return;
    }
    if (s_input_session_active) return;

    if (!stop_wakeword_and_wait()) return;
    flush_mic_tx_queue();
    s_last_activity_us = esp_timer_get_time();

    if (audio_hal_start_capture() != ESP_OK) {
        ESP_LOGE(TAG, "Gagal start MIC capture untuk Gemini");
        return;
    }

    s_input_session_active = true;
    const BaseType_t rc = xTaskCreatePinnedToCore(
        conversation_task, "audio_capture", CONVERSATION_TASK_STACK,
        nullptr, CONVERSATION_TASK_PRIORITY, &s_capture_task, 1);
    if (rc != pdPASS) {
        s_input_session_active = false;
        s_capture_task = nullptr;
        (void)audio_hal_stop_capture();
        ESP_LOGE(TAG, "Gagal membuat Conversation MIC task");
        return;
    }

    ESP_LOGI(TAG, "MIC session START: Gemini owns MIC; WakeWord stopped");
}

void audio_engine_stop_input_session(void)
{
    if (!s_input_session_active && s_capture_task == nullptr) {
        flush_mic_tx_queue();
        return;
    }

    s_input_session_active = false;
    (void)audio_hal_stop_capture();

    for (uint32_t i = 0; i < 200 && s_capture_task != nullptr; ++i)
        vTaskDelay(1);

    flush_mic_tx_queue();
    ESP_LOGI(TAG, "MIC session STOP: Gemini released MIC");
}

bool audio_engine_input_session_active(void)
{
    return s_input_session_active;
}
