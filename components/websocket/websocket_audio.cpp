#include "websocket_audio.h"
#include "websocket.h"
#include "gemini/gemini_audio.h"
#include "audio_engine.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>
#include <string.h>

static const char *TAG = "WS_AUDIO";
static constexpr size_t FRAME_SAMPLES = 320;
static constexpr size_t FRAME_BYTES = FRAME_SAMPLES * sizeof(int16_t);
static constexpr size_t FRAMES_PER_BUFFER = 5;
static constexpr size_t TX_BUFFER_BYTES = FRAME_BYTES * FRAMES_PER_BUFFER;
static constexpr uint32_t TASK_STACK = 8192;
static constexpr UBaseType_t TASK_PRIORITY = 6;
static constexpr int32_t SILENCE_THRESHOLD = 500;
static constexpr size_t MIN_ACTIVE_SAMPLES = 8;

static TaskHandle_t s_task = nullptr;
static volatile bool s_running = false;

static bool active(const int16_t *p, size_t n) {
    size_t active_samples = 0;
    for (size_t i = 0; i < n; ++i) {
        int32_t v = p[i];
        if (v < 0) v = -v;
        if (v >= SILENCE_THRESHOLD && ++active_samples >= MIN_ACTIVE_SAMPLES) return true;
    }
    return false;
}

static void task(void *) {
    static int16_t tx[TX_BUFFER_BYTES / 2];
    static int16_t frame[FRAME_SAMPLES];
    size_t count = 0;

    while (s_running) {
        if (!websocket_is_connected()) {
            count = 0;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (!audio_engine_read_mic_frame(frame, FRAME_SAMPLES, 100)) continue;

        memcpy(tx + count * FRAME_SAMPLES, frame, FRAME_BYTES);
        if (++count < FRAMES_PER_BUFFER) continue;

        // Do not uplink microphone audio while Gemini is returning model
        // audio. After turnComplete gemini_audio_turn_active() becomes false
        // and this same task immediately continues with the next user turn.
        if (!gemini_audio_turn_active()) {
            if (active(tx, TX_BUFFER_BYTES / 2)) {
                if (!websocket_tx_enqueue_audio((const uint8_t *)tx, TX_BUFFER_BYTES)) {
                    ESP_LOGW(TAG, "Audio TX enqueue gagal");
                }
            }
        }
        count = 0;
    }

    s_task = nullptr;
    vTaskDelete(nullptr);
}

bool websocket_audio_start(void) {
    if (s_running) return true;
    if (!audio_engine_conversation_active()) return false;

    s_running = true;
    if (xTaskCreate(task, "ws_audio", TASK_STACK, nullptr, TASK_PRIORITY, &s_task) != pdPASS) {
        s_running = false;
        s_task = nullptr;
        return false;
    }
    ESP_LOGI(TAG, "Gemini MIC uplink START - multiturn session active");
    return true;
}

void websocket_audio_stop(void) {
    s_running = false;
    ESP_LOGI(TAG, "Gemini MIC uplink STOP");
}

bool websocket_audio_running(void) { return s_running; }
