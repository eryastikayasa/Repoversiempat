#include "websocket_internal.h"
#include "websocket_mgr.h"
#include "audio_engine.h"
#include "audio_hal.h"
#include "display.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/semphr.h"
#include <stddef.h>
#include <stdint.h>

static const char *TAG = "WS_AUDIO";
static volatile bool audio_clear_pending = false;
static StaticSemaphore_t audio_send_mutex_storage;
static SemaphoreHandle_t audio_send_mutex = NULL;
uint64_t audio_bytes_playback_dropped = 0;

#define AUDIO_OUTPUT_SAMPLE_RATE        24000U
#define AUDIO_OUTPUT_BYTES_PER_SEC      (AUDIO_OUTPUT_SAMPLE_RATE * 2U)
#define AUDIO_RING_BUFFER_SIZE          (512U * 1024U)
#define AUDIO_PLAYBACK_PREBUFFER_SIZE   (128U * 1024U)
#define AUDIO_PLAYBACK_WARNING_SIZE     (64U * 1024U)
#define AUDIO_PLAYBACK_CRITICAL_SIZE    (32U * 1024U)
#define AUDIO_PLAYBACK_READ_SIZE        1024U
#define AUDIO_PLAYBACK_READ_WAIT_MS     5
#define AUDIO_PLAYBACK_TRIGGER_SIZE     1024U
#define AUDIO_SEND_CHUNK_SIZE           1024U
#define AUDIO_I2S_DRAIN_MS              20

static volatile uint32_t audio_turn_generation = 0;
static int64_t audio_drain_deadline_us = 0;
static uint64_t audio_received_accounted = 0;
static uint32_t audio_chunks_accounted = 0;
static int64_t audio_last_queue_us = 0;
static int64_t audio_low_since_us = 0;
static size_t audio_last_queue_len = 0;

static size_t send_realtime_pcm(const uint8_t *data, size_t len)
{
    if (audio_stream == NULL || data == NULL || len == 0) return 0;

    size_t offset = 0;
    while (offset < len) {
        size_t chunk = len - offset;
        if (chunk > AUDIO_SEND_CHUNK_SIZE) chunk = AUDIO_SEND_CHUNK_SIZE;
        chunk &= ~((size_t)1);
        if (chunk == 0) break;

        size_t written = xStreamBufferSend(audio_stream, data + offset, chunk, pdMS_TO_TICKS(1000));
        if (written == 0) {
            ESP_LOGE(TAG, "Ring buffer send timeout!");
            break;
        }
        offset += written;
        audio_bytes_queued += written;
    }
    return offset;
}

size_t get_audio_pending_bytes(void)
{
    return audio_stream ? xStreamBufferBytesAvailable(audio_stream) : 0;
}

void check_audio_playback_complete(void)
{
    if (!audio_turn_complete_pending || audio_stream == NULL) return;

    if (xStreamBufferBytesAvailable(audio_stream) != 0) {
        audio_drain_deadline_us = 0;
        return;
    }

    const int64_t now_us = esp_timer_get_time();
    if (audio_drain_deadline_us == 0) {
        audio_drain_deadline_us = now_us + ((int64_t)AUDIO_I2S_DRAIN_MS * 1000LL);
        return;
    }
    if (now_us < audio_drain_deadline_us) return;

    audio_drain_deadline_us = 0;

    audio_engine_notify(AUDIO_ENGINE_EVENT_PLAYBACK_DRAINED, websocket_connection_generation);
    audio_engine_notify(AUDIO_ENGINE_EVENT_I2S_DRAINED, websocket_connection_generation);

    const uint64_t network_accounted = audio_bytes_queued + audio_bytes_dropped;
    const int64_t network_balance = (int64_t)audio_bytes_received - (int64_t)network_accounted;
    const uint64_t playback_accounted = audio_bytes_played + audio_bytes_playback_dropped;
    const int64_t playback_balance = (int64_t)audio_bytes_queued - (int64_t)playback_accounted;

    ESP_LOGI(TAG,
             "AUDIO COMPLETE: rx=%llu queued=%llu played=%llu net_drop=%llu play_drop=%llu net_bal=%lld play_bal=%lld",
             (unsigned long long)audio_bytes_received,
             (unsigned long long)audio_bytes_queued,
             (unsigned long long)audio_bytes_played,
             (unsigned long long)audio_bytes_dropped,
             (unsigned long long)audio_bytes_playback_dropped,
             (long long)network_balance,
             (long long)playback_balance);

    /* Transitional compatibility: AudioEngine now owns the lifecycle state. */
    audio_turn_complete_pending = false;
    audio_turn_active = false;
}

static void audio_playback_task(void *arg)
{
    (void)arg;
    static uint8_t playback_buffer[AUDIO_PLAYBACK_READ_SIZE];
    bool playback_started = false;
    bool underrun_reported = false;
    uint8_t buffer_level = 0;
    uint32_t playback_generation = 0;
    int64_t last_stats_us = 0;

    ESP_LOGI(TAG,
             "Audio playback: %uHz PCM16 mono, ring=%u, prebuffer=%u (~%ums), warning=%u (~%ums), critical=%u (~%ums), read=%u, core=%d priority=5",
             (unsigned)AUDIO_OUTPUT_SAMPLE_RATE,
             (unsigned)AUDIO_RING_BUFFER_SIZE,
             (unsigned)AUDIO_PLAYBACK_PREBUFFER_SIZE,
             (unsigned)((AUDIO_PLAYBACK_PREBUFFER_SIZE * 1000U) / AUDIO_OUTPUT_BYTES_PER_SEC),
             (unsigned)AUDIO_PLAYBACK_WARNING_SIZE,
             (unsigned)((AUDIO_PLAYBACK_WARNING_SIZE * 1000U) / AUDIO_OUTPUT_BYTES_PER_SEC),
             (unsigned)AUDIO_PLAYBACK_CRITICAL_SIZE,
             (unsigned)((AUDIO_PLAYBACK_CRITICAL_SIZE * 1000U) / AUDIO_OUTPUT_BYTES_PER_SEC),
             (unsigned)AUDIO_PLAYBACK_READ_SIZE,
             xPortGetCoreID());

    for (;;) {
        if (audio_clear_pending) {
            audio_clear_pending = false;

            if (audio_send_mutex != NULL && xSemaphoreTake(audio_send_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
                audio_clear_pending = true;
                vTaskDelay(pdMS_TO_TICKS(2));
                continue;
            }

            if (audio_stream != NULL) xStreamBufferReset(audio_stream);
            if (audio_send_mutex != NULL) xSemaphoreGive(audio_send_mutex);

            audio_turn_complete_pending = false;
            audio_turn_active = false;
            audio_drain_deadline_us = 0;
            audio_last_queue_us = 0;
            audio_low_since_us = 0;
            audio_last_queue_len = 0;
            playback_started = false;
            underrun_reported = false;
            buffer_level = 0;
            playback_generation = audio_turn_generation;
        }

        uint32_t current_generation = audio_turn_generation;
        if (current_generation != playback_generation) {
            playback_generation = current_generation;
            playback_started = false;
            underrun_reported = false;
            buffer_level = 0;
            audio_drain_deadline_us = 0;
            audio_last_queue_us = 0;
            audio_low_since_us = 0;
            audio_last_queue_len = 0;
        }

        if (audio_stream == NULL) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        size_t pending = xStreamBufferBytesAvailable(audio_stream);

        if (audio_turn_active) {
            if (pending < AUDIO_PLAYBACK_PREBUFFER_SIZE) {
                if (audio_low_since_us == 0) audio_low_since_us = esp_timer_get_time();
            } else {
                audio_low_since_us = 0;
            }

            uint8_t new_level = 0;
            if (pending >= AUDIO_PLAYBACK_WARNING_SIZE) new_level = 3;
            else if (pending >= AUDIO_PLAYBACK_CRITICAL_SIZE) new_level = 2;
            else if (pending > 0) new_level = 1;

            if (new_level != buffer_level) {
                buffer_level = new_level;
                if (new_level == 1) {
                    ESP_LOGW(TAG, "AUDIO BUFFER CRITICAL: pending=%u B (~%u ms)",
                             (unsigned)pending,
                             (unsigned)((pending * 1000U) / AUDIO_OUTPUT_BYTES_PER_SEC));
                    audio_engine_notify(AUDIO_ENGINE_EVENT_PLAYBACK_LOW, websocket_connection_generation);
                } else if (new_level == 2) {
                    ESP_LOGW(TAG, "AUDIO BUFFER WARNING: pending=%u B (~%u ms)",
                             (unsigned)pending,
                             (unsigned)((pending * 1000U) / AUDIO_OUTPUT_BYTES_PER_SEC));
                }
            }
        } else {
            audio_low_since_us = 0;
        }

        if (!playback_started && pending < AUDIO_PLAYBACK_PREBUFFER_SIZE && audio_turn_active && !audio_turn_complete_pending) {
            vTaskDelay(1);
            continue;
        }

        if (playback_started && pending == 0 && audio_turn_active && !audio_turn_complete_pending) {
            if (!underrun_reported) {
                const int64_t now_us = esp_timer_get_time();
                const int64_t input_gap_ms = audio_last_queue_us > 0 ? (now_us - audio_last_queue_us) / 1000LL : -1LL;
                const int64_t low_for_ms = audio_low_since_us > 0 ? (now_us - audio_low_since_us) / 1000LL : 0LL;
                ESP_LOGW(TAG,
                         "AUDIO UNDERRUN TIMELINE: input_gap=%lldms low_for=%lldms pending=%u last_queue=%u rx=%llu queued=%llu played=%llu net_drop=%llu play_drop=%llu",
                         (long long)input_gap_ms,
                         (long long)low_for_ms,
                         (unsigned)pending,
                         (unsigned)audio_last_queue_len,
                         (unsigned long long)audio_bytes_received,
                         (unsigned long long)audio_bytes_queued,
                         (unsigned long long)audio_bytes_played,
                         (unsigned long long)audio_bytes_dropped,
                         (unsigned long long)audio_bytes_playback_dropped);
                audio_engine_note_underrun();
                underrun_reported = true;
            }
            playback_started = false;
            buffer_level = 0;
            vTaskDelay(pdMS_TO_TICKS(AUDIO_PLAYBACK_READ_WAIT_MS));
            continue;
        }

        size_t received = xStreamBufferReceive(audio_stream, playback_buffer, sizeof(playback_buffer), pdMS_TO_TICKS(AUDIO_PLAYBACK_READ_WAIT_MS));
        if (received == 0) {
            check_audio_playback_complete();
            vTaskDelay(1);
            continue;
        }

        received &= ~((size_t)1);
        if (received == 0) {
            vTaskDelay(1);
            continue;
        }

        if (!playback_started) {
            playback_started = true;
            audio_engine_notify(AUDIO_ENGINE_EVENT_PLAYBACK_STARTED, websocket_connection_generation);
            face_set_state(FACE_SPEAKING);
        }
        underrun_reported = false;

        const size_t played = audio_write_speaker(playback_buffer, received);
        audio_write_calls++;
        audio_bytes_played += played;
        audio_engine_note_playback(played);

        if (played < received) {
            const size_t dropped = received - played;
            audio_bytes_playback_dropped += dropped;
            ESP_LOGW(TAG, "AUDIO PLAYBACK LOSS: received=%u played=%u dropped=%u",
                     (unsigned)received, (unsigned)played, (unsigned)dropped);
        }

        check_audio_playback_complete();

        const int64_t now_us = esp_timer_get_time();
        if (last_stats_us == 0 || now_us - last_stats_us >= 1000000LL) {
            last_stats_us = now_us;
            ESP_LOGI(TAG,
                     "AUDIO FLOW: pending=%u/%u received=%llu queued=%llu played=%llu net_drop=%llu play_drop=%llu",
                     (unsigned)xStreamBufferBytesAvailable(audio_stream),
                     (unsigned)AUDIO_RING_BUFFER_SIZE,
                     (unsigned long long)audio_bytes_received,
                     (unsigned long long)audio_bytes_queued,
                     (unsigned long long)audio_bytes_played,
                     (unsigned long long)audio_bytes_dropped,
                     (unsigned long long)audio_bytes_playback_dropped);
        }

        if (!audio_turn_active && xStreamBufferBytesAvailable(audio_stream) == 0) {
            playback_started = false;
            underrun_reported = false;
            buffer_level = 0;
            audio_last_queue_us = 0;
            audio_low_since_us = 0;
            audio_last_queue_len = 0;
        }
    }
}

bool start_audio_playback(void)
{
    if (audio_stream != NULL) return true;

    if (audio_send_mutex == NULL) {
        audio_send_mutex = xSemaphoreCreateMutexStatic(&audio_send_mutex_storage);
        if (audio_send_mutex == NULL) {
            ESP_LOGE(TAG, "Gagal membuat audio send mutex");
            return false;
        }
    }

    uint8_t *buffer_mem = (uint8_t *)heap_caps_malloc(AUDIO_RING_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
    if (buffer_mem == NULL) buffer_mem = (uint8_t *)heap_caps_malloc(AUDIO_RING_BUFFER_SIZE, MALLOC_CAP_INTERNAL);
    if (buffer_mem == NULL) {
        ESP_LOGE(TAG, "Gagal alokasi %u byte untuk audio buffer", (unsigned)AUDIO_RING_BUFFER_SIZE);
        return false;
    }

    static StaticStreamBuffer_t stream_buffer_struct;
    audio_stream = xStreamBufferCreateStatic(AUDIO_RING_BUFFER_SIZE, AUDIO_PLAYBACK_TRIGGER_SIZE, buffer_mem, &stream_buffer_struct);
    if (audio_stream == NULL) {
        heap_caps_free(buffer_mem);
        ESP_LOGE(TAG, "Gagal membuat static stream buffer");
        return false;
    }

    BaseType_t result = xTaskCreatePinnedToCore(audio_playback_task, "audio_playback", 4096, NULL, 5, &audio_playback_task_handle, 0);
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Gagal membuat audio playback task: free_internal=%u largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        vStreamBufferDelete(audio_stream);
        audio_stream = NULL;
        audio_playback_task_handle = NULL;
        return false;
    }

    ESP_LOGI(TAG,
             "Audio ring buffer siap: %u byte, prebuffer=%u, warning=%u, critical=%u, target=%u B/s, playback core=0 priority=5",
             (unsigned)AUDIO_RING_BUFFER_SIZE,
             (unsigned)AUDIO_PLAYBACK_PREBUFFER_SIZE,
             (unsigned)AUDIO_PLAYBACK_WARNING_SIZE,
             (unsigned)AUDIO_PLAYBACK_CRITICAL_SIZE,
             (unsigned)AUDIO_OUTPUT_BYTES_PER_SEC);
    return true;
}

void request_audio_buffer_clear(void)
{
    audio_clear_pending = true;
}

void clear_audio_buffer(void)
{
    audio_clear_pending = false;
    if (audio_send_mutex != NULL) xSemaphoreTake(audio_send_mutex, portMAX_DELAY);
    if (audio_stream != NULL) xStreamBufferReset(audio_stream);
    if (audio_send_mutex != NULL) xSemaphoreGive(audio_send_mutex);

    audio_turn_complete_pending = false;
    audio_turn_active = false;
    audio_drain_deadline_us = 0;
    audio_last_queue_us = 0;
    audio_low_since_us = 0;
    audio_last_queue_len = 0;
}

void reset_audio_turn_stats(void)
{
    audio_chunks_received = 0;
    audio_bytes_received = 0;
    audio_bytes_queued = 0;
    audio_write_calls = 0;
    audio_bytes_played = 0;
    audio_bytes_dropped = 0;
    audio_bytes_playback_dropped = 0;
    audio_received_accounted = 0;
    audio_chunks_accounted = 0;
    audio_turn_active = false;
    audio_turn_complete_pending = false;
    audio_drain_deadline_us = 0;
    audio_last_queue_us = 0;
    audio_low_since_us = 0;
    audio_last_queue_len = 0;
}

void begin_audio_turn(void)
{
    if (audio_turn_active) return;

    audio_chunks_received = 0;
    audio_bytes_received = 0;
    audio_bytes_queued = 0;
    audio_write_calls = 0;
    audio_bytes_played = 0;
    audio_bytes_dropped = 0;
    audio_bytes_playback_dropped = 0;
    audio_received_accounted = 0;
    audio_chunks_accounted = 0;
    audio_drain_deadline_us = 0;
    audio_last_queue_us = 0;
    audio_low_since_us = 0;
    audio_last_queue_len = 0;

    if (audio_stream != NULL) {
        size_t stale = xStreamBufferBytesAvailable(audio_stream);
        if (stale > 0) {
            xStreamBufferReset(audio_stream);
            ESP_LOGW(TAG, "Audio stale PCM dibuang saat turn baru: %u byte", (unsigned)stale);
        }
    }

    uint32_t next_generation = audio_turn_generation + 1U;
    if (next_generation == 0U) next_generation = 1U;
    audio_turn_generation = next_generation;
    audio_turn_active = true;
    audio_turn_complete_pending = false;

    audio_engine_notify(AUDIO_ENGINE_EVENT_MODEL_BEGIN, websocket_connection_generation);
}

bool queue_audio_pcm(const uint8_t *pcm, size_t len)
{
    if (pcm == NULL || len == 0) return false;
    len &= ~((size_t)1);
    if (len == 0) return false;
    if (audio_stream == NULL && !start_audio_playback()) return false;
    if (audio_stream == NULL || audio_send_mutex == NULL) return false;
    if (xSemaphoreTake(audio_send_mutex, portMAX_DELAY) != pdTRUE) return false;

    begin_audio_turn();
    audio_engine_notify(AUDIO_ENGINE_EVENT_MODEL_AUDIO, websocket_connection_generation);

    const int64_t now_us = esp_timer_get_time();
    if (audio_last_queue_us != 0) {
        const int64_t gap_us = now_us - audio_last_queue_us;
        if (gap_us >= 100000LL) {
            ESP_LOGW(TAG, "AUDIO INPUT GAP: gap=%lldms len=%u pending=%u rx=%llu queued=%llu played=%llu",
                     (long long)(gap_us / 1000LL),
                     (unsigned)len,
                     (unsigned)xStreamBufferBytesAvailable(audio_stream),
                     (unsigned long long)audio_bytes_received,
                     (unsigned long long)audio_bytes_queued,
                     (unsigned long long)audio_bytes_played);
        }
    }

    audio_last_queue_us = now_us;
    audio_last_queue_len = len;
    audio_received_accounted += len;
    audio_chunks_accounted++;
    audio_bytes_received = audio_received_accounted;
    audio_chunks_received = audio_chunks_accounted;

    const uint64_t queued_before = audio_bytes_queued;
    const uint64_t dropped_before = audio_bytes_dropped;
    const size_t queued = send_realtime_pcm(pcm, len);
    const uint64_t queued_delta = audio_bytes_queued - queued_before;
    const uint64_t dropped_delta = audio_bytes_dropped - dropped_before;
    const uint64_t accounted_delta = queued_delta + dropped_delta;

    if (accounted_delta < (uint64_t)len) {
        audio_bytes_dropped += (uint64_t)len - accounted_delta;
        ESP_LOGW(TAG, "Audio accounting guard: %u byte -> dropped", (unsigned)((uint64_t)len - accounted_delta));
    } else if (accounted_delta > (uint64_t)len) {
        ESP_LOGW(TAG, "Audio accounting anomaly: accounted_delta=%llu len=%u",
                 (unsigned long long)accounted_delta, (unsigned)len);
    }

    xSemaphoreGive(audio_send_mutex);
    audio_engine_note_audio(queued);
    return queued_delta == (uint64_t)len;
}
