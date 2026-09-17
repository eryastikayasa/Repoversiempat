#include "gemini_audio.h"
#include "audio_engine.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "GEMINI_AUDIO";

// Follow Repo3's proven audio architecture: RX JSON processing only decodes
// PCM and queues it into a persistent ring buffer. A dedicated playback task
// owns the blocking I2S speaker writes. This keeps the WebSocket RX worker
// responsive while Gemini streams many audio chunks.
static constexpr size_t AUDIO_RING_BUFFER_SIZE = 256 * 1024;
static constexpr size_t AUDIO_PLAYBACK_PREBUFFER_SIZE = 32 * 1024;
static constexpr size_t AUDIO_PLAYBACK_READ_SIZE = 2048;
static constexpr size_t AUDIO_PLAYBACK_TRIGGER_SIZE = 1024;
static constexpr uint32_t AUDIO_PLAYBACK_TASK_STACK = 4096;
static constexpr UBaseType_t AUDIO_PLAYBACK_TASK_PRIORITY = 6;
static constexpr uint32_t AUDIO_PLAYBACK_READ_WAIT_MS = 5;

static volatile bool s_active = false;
static volatile bool s_turn_complete_pending = false;
static volatile bool s_clear_pending = false;

static StaticStreamBuffer_t s_stream_struct;
static StreamBufferHandle_t s_stream = nullptr;
static uint8_t *s_stream_memory = nullptr;
static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex = nullptr;
static TaskHandle_t s_playback_task = nullptr;
static bool s_playback_initialized = false;

static bool ensure_playback_pipeline(void)
{
    if (s_playback_initialized) return true;

    s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
    if (!s_mutex) {
        ESP_LOGE(TAG, "Gagal membuat audio mutex");
        return false;
    }

    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0)
        s_stream_memory = (uint8_t *)heap_caps_malloc(AUDIO_RING_BUFFER_SIZE,
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_stream_memory)
        s_stream_memory = (uint8_t *)heap_caps_malloc(AUDIO_RING_BUFFER_SIZE, MALLOC_CAP_8BIT);
    if (!s_stream_memory) {
        ESP_LOGE(TAG, "Gagal alokasi audio ring %u byte", (unsigned)AUDIO_RING_BUFFER_SIZE);
        return false;
    }

    s_stream = xStreamBufferCreateStatic(AUDIO_RING_BUFFER_SIZE,
                                          AUDIO_PLAYBACK_TRIGGER_SIZE,
                                          s_stream_memory,
                                          &s_stream_struct);
    if (!s_stream) {
        heap_caps_free(s_stream_memory);
        s_stream_memory = nullptr;
        ESP_LOGE(TAG, "Gagal membuat audio stream buffer");
        return false;
    }

    s_playback_initialized = true;
    return true;
}

static void clear_pipeline(void)
{
    if (!s_stream) return;
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        xStreamBufferReset(s_stream);
        xSemaphoreGive(s_mutex);
    } else {
        xStreamBufferReset(s_stream);
    }
    s_turn_complete_pending = false;
    s_active = false;
}

static void check_playback_complete(void)
{
    if (!s_turn_complete_pending || !s_stream) return;
    if (xStreamBufferBytesAvailable(s_stream) != 0) return;

    s_turn_complete_pending = false;
    s_active = false;
    ESP_LOGI(TAG, "Gemini AUDIO PLAYBACK COMPLETE - ready for next turn");
}

static void playback_task(void *)
{
    static uint8_t buffer[AUDIO_PLAYBACK_READ_SIZE];
    bool playback_started = false;

    ESP_LOGI(TAG,
             "Gemini playback task START: ring=%u prebuffer=%u stack=%u",
             (unsigned)AUDIO_RING_BUFFER_SIZE,
             (unsigned)AUDIO_PLAYBACK_PREBUFFER_SIZE,
             (unsigned)AUDIO_PLAYBACK_TASK_STACK);

    for (;;) {
        if (s_clear_pending) {
            s_clear_pending = false;
            clear_pipeline();
            playback_started = false;
        }

        if (!s_stream) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        size_t pending = xStreamBufferBytesAvailable(s_stream);
        if (!playback_started && pending < AUDIO_PLAYBACK_PREBUFFER_SIZE &&
            s_active && !s_turn_complete_pending) {
            vTaskDelay(pdMS_TO_TICKS(AUDIO_PLAYBACK_READ_WAIT_MS));
            continue;
        }

        size_t received = xStreamBufferReceive(s_stream, buffer, sizeof(buffer),
                                                pdMS_TO_TICKS(AUDIO_PLAYBACK_READ_WAIT_MS));
        if (received == 0) {
            check_playback_complete();
            vTaskDelay(1);
            continue;
        }

        received &= ~((size_t)1);
        if (received == 0) continue;

        if (!playback_started) {
            if (!audio_engine_start_playback()) {
                ESP_LOGE(TAG, "Gagal start speaker playback");
                s_active = false;
                s_turn_complete_pending = false;
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            playback_started = true;
        }

        if (!audio_engine_write_speaker_pcm((const int16_t *)buffer, received / 2, 100)) {
            ESP_LOGW(TAG, "Gemini speaker PCM write gagal");
        }

        check_playback_complete();
        if (!s_active && xStreamBufferBytesAvailable(s_stream) == 0) {
            audio_engine_stop_playback();
            playback_started = false;
        }
        vTaskDelay(1);
    }
}

static bool ensure_playback_task(void)
{
    if (!ensure_playback_pipeline()) return false;
    if (s_playback_task) return true;

    if (xTaskCreate(playback_task, "gemini_playback",
                    AUDIO_PLAYBACK_TASK_STACK, nullptr,
                    AUDIO_PLAYBACK_TASK_PRIORITY, &s_playback_task) != pdPASS) {
        s_playback_task = nullptr;
        ESP_LOGE(TAG, "Gagal membuat Gemini playback task");
        return false;
    }
    return true;
}

bool gemini_audio_turn_active(void) { return s_active; }

bool gemini_audio_process_server_message(const char *json, size_t len)
{
    if (!json || !len) return false;
    if (!ensure_playback_task()) return false;

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return false;

    bool handled = false;
    cJSON *server_content = cJSON_GetObjectItem(root, "serverContent");
    if (cJSON_IsObject(server_content)) {
        cJSON *interrupted = cJSON_GetObjectItem(server_content, "interrupted");
        if (cJSON_IsTrue(interrupted)) {
            s_clear_pending = true;
            s_active = false;
            handled = true;
            ESP_LOGI(TAG, "Gemini AUDIO interrupted - playback buffer clear requested");
        }

        cJSON *model_turn = cJSON_GetObjectItem(server_content, "modelTurn");
        cJSON *parts = model_turn ? cJSON_GetObjectItem(model_turn, "parts") : nullptr;
        if (cJSON_IsArray(parts)) {
            for (int i = 0; i < cJSON_GetArraySize(parts); ++i) {
                cJSON *part = cJSON_GetArrayItem(parts, i);
                cJSON *inline_data = part ? cJSON_GetObjectItem(part, "inlineData") : nullptr;
                if (!cJSON_IsObject(inline_data)) continue;

                cJSON *data = cJSON_GetObjectItem(inline_data, "data");
                if (!cJSON_IsString(data) || !data->valuestring) continue;

                const size_t encoded_len = strlen(data->valuestring);
                const size_t pcm_capacity = (encoded_len / 4) * 3 + 4;
                size_t pcm_len = 0;
                uint8_t *pcm = (uint8_t *)malloc(pcm_capacity);
                if (!pcm) {
                    ESP_LOGW(TAG, "Gagal alokasi decode PCM %u byte", (unsigned)pcm_capacity);
                    continue;
                }

                if (mbedtls_base64_decode(pcm, pcm_capacity, &pcm_len,
                                          (const unsigned char *)data->valuestring,
                                          encoded_len) == 0 && pcm_len > 1) {
                    if (!s_active) {
                        if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                            xStreamBufferReset(s_stream);
                            xSemaphoreGive(s_mutex);
                        } else {
                            xStreamBufferReset(s_stream);
                        }
                        s_turn_complete_pending = false;
                        s_active = true;
                        ESP_LOGI(TAG, "Gemini AUDIO TURN START - PCM queued");
                    }

                    pcm_len &= ~((size_t)1);
                    size_t offset = 0;
                    while (offset < pcm_len) {
                        const size_t chunk = (pcm_len - offset > 512) ? 512 : (pcm_len - offset);
                        const size_t written = xStreamBufferSend(s_stream,
                            pcm + offset, chunk, pdMS_TO_TICKS(50));
                        if (written == 0) {
                            ESP_LOGW(TAG, "Gemini audio ring penuh: offset=%u/%u",
                                     (unsigned)offset, (unsigned)pcm_len);
                            break;
                        }
                        offset += written;
                    }
                    handled = true;
                }
                free(pcm);
            }
        }

        // Repo3 behavior: turnComplete marks playback drain pending. It does
        // not close the Gemini session. The session stays available for the
        // next user turn, and MIC resumes only after the current audio drains.
        cJSON *turn_complete = cJSON_GetObjectItem(server_content, "turnComplete");
        if (cJSON_IsTrue(turn_complete)) {
            s_turn_complete_pending = true;
            handled = true;
            ESP_LOGI(TAG, "Gemini turnComplete - waiting for audio drain, session remains active");
            check_playback_complete();
        }
    }

    cJSON_Delete(root);
    return handled;
}
