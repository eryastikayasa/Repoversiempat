#include "websocket_internal.h"
#include "websocket_mgr.h"
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
#include <string.h>

static const char *TAG = "WS_AUDIO";

static volatile bool audio_clear_pending = false;

static StaticSemaphore_t audio_send_mutex_storage;
static SemaphoreHandle_t audio_send_mutex = NULL;
uint64_t audio_bytes_playback_dropped = 0;

/* -------------------------------------------------------------------------- */
/* Konfigurasi Audio Playback                                                 */
/* -------------------------------------------------------------------------- */
#define AUDIO_OUTPUT_SAMPLE_RATE       24000U
#define AUDIO_OUTPUT_BYTES_PER_SEC     (AUDIO_OUTPUT_SAMPLE_RATE * 2U)
#define AUDIO_RING_BUFFER_SIZE          (256U * 1024U)   // 262144 byte ≈ 5,46 detik
#define AUDIO_PLAYBACK_PREBUFFER_SIZE   (64U * 1024U)    // 65536 byte ≈ 1,36 detik
#define AUDIO_PLAYBACK_WARNING_SIZE     (32U * 1024U)    // 32768 byte ≈ 682 ms
#define AUDIO_PLAYBACK_CRITICAL_SIZE    (16U * 1024U)    // 16384 byte ≈ 341 ms

/* Ukuran pembacaan dari ring buffer ke I2S */
#define AUDIO_PLAYBACK_READ_SIZE       1024U
#define AUDIO_PLAYBACK_READ_WAIT_MS    5

/* Ukuran minimal data untuk memicu playback */
#define AUDIO_PLAYBACK_TRIGGER_SIZE    1024U

/* Ukuran chunk pengiriman ke ring buffer */
#define AUDIO_SEND_CHUNK_SIZE          1024U
#define AUDIO_SEND_WAIT_MS             50

/* Waktu drain I2S setelah turn selesai */
#define AUDIO_I2S_DRAIN_MS             20

/* -------------------------------------------------------------------------- */
/* Audio turn state                                                           */
/* -------------------------------------------------------------------------- */

static volatile uint32_t audio_turn_generation = 0;
static int64_t audio_drain_deadline_us = 0;

static uint64_t audio_received_accounted = 0;
static uint32_t audio_chunks_accounted = 0;


/* -------------------------------------------------------------------------- */
/* Underrun timeline measurement                                              */
/* -------------------------------------------------------------------------- */

static int64_t audio_last_queue_us = 0;
static int64_t audio_low_since_us = 0;
static size_t audio_last_queue_len = 0;


/* -------------------------------------------------------------------------- */
/* PCM -> playback ring                                                       */
/* -------------------------------------------------------------------------- */

static size_t send_realtime_pcm(const uint8_t *data, size_t len)
{
    if (audio_stream == NULL || data == NULL || len == 0) {
        return 0;
    }

    size_t offset = 0;

    while (offset < len) {
        size_t chunk = len - offset;
        if (chunk > AUDIO_SEND_CHUNK_SIZE) {
            chunk = AUDIO_SEND_CHUNK_SIZE;
        }

        /* PCM16 harus genap */
        chunk &= ~((size_t)1);
        if (chunk == 0) {
            break;
        }

        /*
         * Blocking send dengan timeout 1 detik.
         * Ini memberikan backpressure ke task WebSocket RX
         * agar tidak menulis lebih cepat daripada kemampuan playback.
         * Data tidak akan hilang, hanya tertunda.
         */
        size_t written = xStreamBufferSend(
            audio_stream,
            data + offset,
            chunk,
            pdMS_TO_TICKS(1000)   // timeout 1 detik
        );

        if (written == 0) {
            /*
             * Timeout terjadi jika playback macet atau terjadi deadlock.
             * Ini jarang terjadi, tetapi kita log dan hentikan pengiriman
             * untuk mencegah loop tak berujung.
             */
            ESP_LOGE(TAG, "Ring buffer send timeout!");
            break;
        }

        offset += written;
        audio_bytes_queued += written;
    }

    return offset;
}


/* -------------------------------------------------------------------------- */
/* Playback state                                                             */
/* -------------------------------------------------------------------------- */

size_t get_audio_pending_bytes(void)
{
    if (audio_stream == NULL) {
        return 0;
    }
    return xStreamBufferBytesAvailable(audio_stream);
}


/* -------------------------------------------------------------------------- */
/* Playback completion                                                        */
/* -------------------------------------------------------------------------- */

void check_audio_playback_complete(void)
{
    if (!audio_turn_complete_pending || audio_stream == NULL) {
        return;
    }

    if (xStreamBufferBytesAvailable(audio_stream) != 0) {
        audio_drain_deadline_us = 0;
        return;
    }

    const int64_t now_us = esp_timer_get_time();

    if (audio_drain_deadline_us == 0) {
        audio_drain_deadline_us =
            now_us + ((int64_t)AUDIO_I2S_DRAIN_MS * 1000LL);
        return;
    }

    if (now_us < audio_drain_deadline_us) {
        return;
    }

    audio_drain_deadline_us = 0;
    audio_turn_complete_pending = false;
    audio_turn_active = false;

    const uint64_t network_accounted =
        audio_bytes_queued + audio_bytes_dropped;

    const int64_t network_balance =
        (int64_t)audio_bytes_received -
        (int64_t)network_accounted;

    const uint64_t playback_accounted =
        audio_bytes_played + audio_bytes_playback_dropped;

    const int64_t playback_balance =
        (int64_t)audio_bytes_queued -
        (int64_t)playback_accounted;

    ESP_LOGI(
        TAG,
        "AUDIO COMPLETE: "
        "rx=%llu queued=%llu played=%llu "
        "net_drop=%llu play_drop=%llu "
        "net_bal=%lld play_bal=%lld",
        (unsigned long long)audio_bytes_received,
        (unsigned long long)audio_bytes_queued,
        (unsigned long long)audio_bytes_played,
        (unsigned long long)audio_bytes_dropped,
        (unsigned long long)audio_bytes_playback_dropped,
        (long long)network_balance,
        (long long)playback_balance
    );

    face_set_state(FACE_LISTENING);
}


/* -------------------------------------------------------------------------- */
/* Playback task                                                              */
/* -------------------------------------------------------------------------- */

static void audio_playback_task(void *arg)
{
    (void)arg;

    static uint8_t playback_buffer[AUDIO_PLAYBACK_READ_SIZE];

    bool playback_started = false;
    bool underrun_reported = false;

    uint8_t buffer_level = 0;

    uint32_t playback_generation = 0;

    int64_t last_stats_us = 0;

    ESP_LOGI(
        TAG,
        "Audio playback task: 24kHz PCM16 mono, "
        "ring=%u, prebuffer=%u (85ms), "
        "warning=%u (85ms), critical=%u (50ms), "
        "read=%u, core=%d priority=5",
        (unsigned)AUDIO_RING_BUFFER_SIZE,
        (unsigned)AUDIO_PLAYBACK_PREBUFFER_SIZE,
        (unsigned)AUDIO_PLAYBACK_WARNING_SIZE,
        (unsigned)AUDIO_PLAYBACK_CRITICAL_SIZE,
        (unsigned)AUDIO_PLAYBACK_READ_SIZE,
        xPortGetCoreID()
    );

    for (;;) {

        /* -------------------------------------------------------------- */
        /* Clear request                                                   */
        /* -------------------------------------------------------------- */

        if (audio_clear_pending) {

            audio_clear_pending = false;

            if (audio_send_mutex != NULL) {

                if (xSemaphoreTake(
                        audio_send_mutex,
                        pdMS_TO_TICKS(10)
                    ) == pdTRUE) {

                    if (audio_stream != NULL) {
                        xStreamBufferReset(audio_stream);
                    }

                    xSemaphoreGive(audio_send_mutex);

                } else {

                    ESP_LOGW(
                        TAG,
                        "Playback clear mutex busy - clear ditunda"
                    );

                    audio_clear_pending = true;

                    vTaskDelay(pdMS_TO_TICKS(2));

                    continue;
                }

            } else if (audio_stream != NULL) {

                xStreamBufferReset(audio_stream);
            }

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


        /* -------------------------------------------------------------- */
        /* Generation change                                               */
        /* -------------------------------------------------------------- */

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


        /* -------------------------------------------------------------- */
        /* Ring availability                                               */
        /* -------------------------------------------------------------- */

        if (audio_stream == NULL) {

            vTaskDelay(pdMS_TO_TICKS(100));

            continue;
        }

        size_t pending =
            xStreamBufferBytesAvailable(audio_stream);


        /* -------------------------------------------------------------- */
        /* Timeline: buffer low-state                                      */
        /* -------------------------------------------------------------- */

        if (audio_turn_active) {

            if (pending < AUDIO_PLAYBACK_PREBUFFER_SIZE) {

                if (audio_low_since_us == 0) {
                    audio_low_since_us = esp_timer_get_time();
                }

            } else {

                audio_low_since_us = 0;
            }
        }
        else {

            audio_low_since_us = 0;
        }


        /* -------------------------------------------------------------- */
        /* Buffer level warning                                            */
        /* -------------------------------------------------------------- */

        if (audio_turn_active) {

            uint8_t new_level;

            if (pending == 0) {
                new_level = 0;
            }
            else if (pending < AUDIO_PLAYBACK_CRITICAL_SIZE) {
                new_level = 1;
            }
            else if (pending < AUDIO_PLAYBACK_WARNING_SIZE) {
                new_level = 2;
            }
            else {
                new_level = 3;
            }

            if (new_level != buffer_level) {

                buffer_level = new_level;

                if (new_level == 1) {

                    ESP_LOGW(
                        TAG,
                        "AUDIO BUFFER CRITICAL: pending=%u B (~%u ms)",
                        (unsigned)pending,
                        (unsigned)(
                            (pending * 1000U) /
                            AUDIO_OUTPUT_BYTES_PER_SEC
                        )
                    );

                }
                else if (new_level == 2) {

                    ESP_LOGW(
                        TAG,
                        "AUDIO BUFFER WARNING: pending=%u B (~%u ms)",
                        (unsigned)pending,
                        (unsigned)(
                            (pending * 1000U) /
                            AUDIO_OUTPUT_BYTES_PER_SEC
                        )
                    );
                }
            }
        }


        /* -------------------------------------------------------------- */
        /* Initial prebuffer                                               */
        /* -------------------------------------------------------------- */

        if (
            !playback_started &&
            pending < AUDIO_PLAYBACK_PREBUFFER_SIZE &&
            audio_turn_active &&
            !audio_turn_complete_pending
        ) {
            vTaskDelay(1);
            continue;
        }


        /* -------------------------------------------------------------- */
        /* Mid-turn underrun                                               */
        /* -------------------------------------------------------------- */

        if (
            playback_started &&
            pending == 0 &&
            audio_turn_active &&
            !audio_turn_complete_pending
        ) {

            if (!underrun_reported) {

                const int64_t now_us =
                    esp_timer_get_time();

                const int64_t input_gap_ms =
                    (audio_last_queue_us > 0)
                        ? (now_us - audio_last_queue_us) / 1000LL
                        : -1LL;

                const int64_t low_for_ms =
                    (audio_low_since_us > 0)
                        ? (now_us - audio_low_since_us) / 1000LL
                        : 0LL;

                ESP_LOGW(
                    TAG,
                    "AUDIO UNDERRUN TIMELINE: "
                    "input_gap=%lldms "
                    "low_for=%lldms "
                    "pending=%u "
                    "last_queue=%u "
                    "rx=%llu "
                    "queued=%llu "
                    "played=%llu "
                    "net_drop=%llu "
                    "play_drop=%llu",
                    (long long)input_gap_ms,
                    (long long)low_for_ms,
                    (unsigned)pending,
                    (unsigned)audio_last_queue_len,
                    (unsigned long long)audio_bytes_received,
                    (unsigned long long)audio_bytes_queued,
                    (unsigned long long)audio_bytes_played,
                    (unsigned long long)audio_bytes_dropped,
                    (unsigned long long)audio_bytes_playback_dropped
                );

                underrun_reported = true;
            }

            playback_started = false;
            buffer_level = 0;

            vTaskDelay(
                pdMS_TO_TICKS(AUDIO_PLAYBACK_READ_WAIT_MS)
            );

            continue;
        }


        /* -------------------------------------------------------------- */
        /* Read PCM from ring                                               */
        /* -------------------------------------------------------------- */

        size_t received = xStreamBufferReceive(
            audio_stream,
            playback_buffer,
            sizeof(playback_buffer),
            pdMS_TO_TICKS(AUDIO_PLAYBACK_READ_WAIT_MS)
        );

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


        /* -------------------------------------------------------------- */
        /* Start playback                                                   */
        /* -------------------------------------------------------------- */

        if (!playback_started) {

            playback_started = true;

            face_set_state(FACE_SPEAKING);
        }

        underrun_reported = false;


        /* -------------------------------------------------------------- */
        /* Write to I2S                                                    */
        /* -------------------------------------------------------------- */

        const size_t played =
            audio_write_speaker(
                playback_buffer,
                received
            );

        audio_write_calls++;

        audio_bytes_played += played;


        /* -------------------------------------------------------------- */
        /* Playback loss accounting                                       */
        /* -------------------------------------------------------------- */

        if (played < received) {

            const size_t dropped =
                received - played;

            audio_bytes_playback_dropped += dropped;

            ESP_LOGW(
                TAG,
                "AUDIO PLAYBACK LOSS: "
                "received=%u played=%u dropped=%u",
                (unsigned)received,
                (unsigned)played,
                (unsigned)dropped
            );
        }


        /* -------------------------------------------------------------- */
        /* Check completion                                                */
        /* -------------------------------------------------------------- */

        check_audio_playback_complete();


        /* -------------------------------------------------------------- */
        /* Reduced flow log: max once / second                            */
        /* -------------------------------------------------------------- */

        int64_t now_us =
            esp_timer_get_time();

        if (
            last_stats_us == 0 ||
            now_us - last_stats_us >= 1000000
        ) {

            last_stats_us = now_us;

            ESP_LOGI(
                TAG,
                "AUDIO FLOW: "
                "pending=%u/%u "
                "received=%llu "
                "queued=%llu "
                "played=%llu "
                "net_drop=%llu "
                "play_drop=%llu",
                (unsigned)xStreamBufferBytesAvailable(audio_stream),
                (unsigned)AUDIO_RING_BUFFER_SIZE,
                (unsigned long long)audio_bytes_received,
                (unsigned long long)audio_bytes_queued,
                (unsigned long long)audio_bytes_played,
                (unsigned long long)audio_bytes_dropped,
                (unsigned long long)audio_bytes_playback_dropped
            );
        }


        /* -------------------------------------------------------------- */
        /* Idle state                                                       */
        /* -------------------------------------------------------------- */

        if (
            !audio_turn_active &&
            xStreamBufferBytesAvailable(audio_stream) == 0
        ) {

            playback_started = false;
            underrun_reported = false;
            buffer_level = 0;

            audio_last_queue_us = 0;
            audio_low_since_us = 0;
            audio_last_queue_len = 0;
        }
    }
}


/* -------------------------------------------------------------------------- */
/* Start playback                                                             */
/* -------------------------------------------------------------------------- */

bool start_audio_playback(void)
{
    if (audio_stream != NULL) {
        return true;
    }

    if (audio_send_mutex == NULL) {
        audio_send_mutex =
            xSemaphoreCreateMutexStatic(
                &audio_send_mutex_storage
            );

        if (audio_send_mutex == NULL) {
            ESP_LOGE(TAG, "Gagal membuat audio send mutex");
            return false;
        }
    }

    uint8_t *buffer_mem =
        (uint8_t *)heap_caps_malloc(
            AUDIO_RING_BUFFER_SIZE,
            MALLOC_CAP_SPIRAM
        );

    if (buffer_mem == NULL) {
        buffer_mem =
            (uint8_t *)heap_caps_malloc(
                AUDIO_RING_BUFFER_SIZE,
                MALLOC_CAP_INTERNAL
            );

        if (buffer_mem == NULL) {
            ESP_LOGE(
                TAG,
                "Gagal alokasi %u byte untuk audio buffer",
                (unsigned)AUDIO_RING_BUFFER_SIZE
            );
            return false;
        }

        ESP_LOGW(TAG, "Menggunakan RAM internal untuk audio buffer");
    }

    static StaticStreamBuffer_t stream_buffer_struct;

    audio_stream =
        xStreamBufferCreateStatic(
            AUDIO_RING_BUFFER_SIZE,
            AUDIO_PLAYBACK_TRIGGER_SIZE,
            buffer_mem,
            &stream_buffer_struct
        );

    if (audio_stream == NULL) {
        ESP_LOGE(TAG, "Gagal membuat static stream buffer");
        heap_caps_free(buffer_mem);
        return false;
    }

    /*
     * Task playback dipindah ke core 0 dan prioritas dinaikkan ke 5
     * agar tidak kalah bersaing dengan task WebSocket RX (prioritas 5)
     * atau task WiFi. Core 0 biasanya lebih sedikit bebannya.
     */
    BaseType_t result =
        xTaskCreatePinnedToCore(
            audio_playback_task,
            "audio_playback",
            4096,
            NULL,
            5,          // prioritas dinaikkan
            &audio_playback_task_handle,
            0           // core 0
        );

    if (result != pdPASS) {
        ESP_LOGE(
            TAG,
            "Gagal membuat audio_task/playback task: "
            "free_internal=%u largest=%u",
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)
        );

        vStreamBufferDelete(audio_stream);
        audio_stream = NULL;
        audio_playback_task_handle = NULL;
        return false;
    }

    ESP_LOGI(
        TAG,
        "Audio ring buffer siap: "
        "%u byte, prebuffer=%u (85ms), "
        "warning=%u (85ms), critical=%u (50ms), "
        "target=%u B/s, playback core=0 priority=5",
        (unsigned)AUDIO_RING_BUFFER_SIZE,
        (unsigned)AUDIO_PLAYBACK_PREBUFFER_SIZE,
        (unsigned)AUDIO_PLAYBACK_WARNING_SIZE,
        (unsigned)AUDIO_PLAYBACK_CRITICAL_SIZE,
        (unsigned)AUDIO_OUTPUT_BYTES_PER_SEC
    );

    return true;
}


/* -------------------------------------------------------------------------- */
/* Clear playback buffer                                                      */
/* -------------------------------------------------------------------------- */

void request_audio_buffer_clear(void)
{
    audio_clear_pending = true;
}


void clear_audio_buffer(void)
{
    audio_clear_pending = false;

    if (audio_send_mutex != NULL) {
        xSemaphoreTake(audio_send_mutex, portMAX_DELAY);
        if (audio_stream != NULL) {
            xStreamBufferReset(audio_stream);
        }
        xSemaphoreGive(audio_send_mutex);
    } else if (audio_stream != NULL) {
        xStreamBufferReset(audio_stream);
    }

    audio_turn_complete_pending = false;
    audio_turn_active = false;
    audio_drain_deadline_us = 0;

    audio_last_queue_us = 0;
    audio_low_since_us = 0;
    audio_last_queue_len = 0;
}


/* -------------------------------------------------------------------------- */
/* Reset per-turn statistics                                                  */
/* -------------------------------------------------------------------------- */

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


/* -------------------------------------------------------------------------- */
/* Begin new Gemini audio turn                                                */
/* -------------------------------------------------------------------------- */

void begin_audio_turn(void)
{
    if (audio_turn_active) {
        return;
    }

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
    if (next_generation == 0U) {
        next_generation = 1U;
    }
    audio_turn_generation = next_generation;

    audio_turn_active = true;
    audio_turn_complete_pending = false;
}


/* -------------------------------------------------------------------------- */
/* Queue decoded Gemini PCM                                                   */
/* -------------------------------------------------------------------------- */

bool queue_audio_pcm(
    const uint8_t *pcm,
    size_t len
)
{
    if (pcm == NULL || len == 0) {
        return false;
    }

    len &= ~((size_t)1);
    if (len == 0) {
        return false;
    }

    if (audio_stream == NULL && !start_audio_playback()) {
        return false;
    }

    if (audio_stream == NULL) {
        return false;
    }

    if (audio_send_mutex == NULL) {
        ESP_LOGE(TAG, "Audio send mutex belum siap");
        return false;
    }

    if (xSemaphoreTake(audio_send_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Gagal mengambil audio send mutex");
        return false;
    }

    begin_audio_turn();

    const int64_t now_us = esp_timer_get_time();

    if (audio_last_queue_us != 0) {
        const int64_t gap_us = now_us - audio_last_queue_us;
        // Hanya log gap yang signifikan (>= 100 ms) untuk mengurangi spam
        if (gap_us >= 100000LL) {
            ESP_LOGW(
                TAG,
                "AUDIO INPUT GAP: "
                "gap=%lldms "
                "len=%u "
                "pending=%u "
                "rx=%llu "
                "queued=%llu "
                "played=%llu",
                (long long)(gap_us / 1000LL),
                (unsigned)len,
                (unsigned)xStreamBufferBytesAvailable(audio_stream),
                (unsigned long long)audio_bytes_received,
                (unsigned long long)audio_bytes_queued,
                (unsigned long long)audio_bytes_played
            );
        }
    }

    audio_last_queue_us = now_us;
    audio_last_queue_len = len;

    audio_received_accounted += len;
    audio_chunks_accounted++;

    audio_bytes_received = audio_received_accounted;
    audio_chunks_received = audio_chunks_accounted;

    uint64_t queued_before = audio_bytes_queued;
    uint64_t dropped_before = audio_bytes_dropped;

    // send_realtime_pcm sekarang blocking, jadi semua data akan masuk
    (void)send_realtime_pcm(pcm, len);

    const uint64_t queued_delta = audio_bytes_queued - queued_before;
    const uint64_t dropped_delta = audio_bytes_dropped - dropped_before;
    const uint64_t accounted_delta = queued_delta + dropped_delta;

    if (accounted_delta < (uint64_t)len) {
        const uint64_t missing = (uint64_t)len - accounted_delta;
        audio_bytes_dropped += missing;
        ESP_LOGW(
            TAG,
            "Audio accounting guard: %llu byte -> dropped",
            (unsigned long long)missing
        );
    } else if (accounted_delta > (uint64_t)len) {
        ESP_LOGW(
            TAG,
            "Audio accounting anomaly: accounted_delta=%llu len=%u",
            (unsigned long long)accounted_delta,
            (unsigned)len
        );
    }

    xSemaphoreGive(audio_send_mutex);

    return queued_delta == (uint64_t)len;
}
