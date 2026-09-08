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

#define AUDIO_OUTPUT_SAMPLE_RATE       24000U
#define AUDIO_OUTPUT_BYTES_PER_SEC     (AUDIO_OUTPUT_SAMPLE_RATE * 2U)

#define AUDIO_RING_BUFFER_SIZE         32768U

#define AUDIO_PLAYBACK_PREBUFFER_SIZE  9600U
#define AUDIO_PLAYBACK_WARNING_SIZE    4800U
#define AUDIO_PLAYBACK_CRITICAL_SIZE   2400U

#define AUDIO_PLAYBACK_READ_SIZE       2048U
#define AUDIO_PLAYBACK_READ_WAIT_MS    5

#define AUDIO_PLAYBACK_TRIGGER_SIZE    1024U

#define AUDIO_SEND_CHUNK_SIZE          512U
#define AUDIO_SEND_WAIT_MS             50

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
/*                                                                            */
/* Tujuan:                                                                    */
/* 1. Mengukur gap antar PCM yang masuk ke ring.                              */
/* 2. Mengukur berapa lama buffer berada di bawah prebuffer.                  */
/* 3. Mengetahui kondisi tepat saat underrun.                                 */
/*                                                                            */
/* Tidak mengubah mekanisme playback.                                         */
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

        TickType_t start = xTaskGetTickCount();

        while (xStreamBufferSpacesAvailable(audio_stream) < chunk) {

            if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(50)) {
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(1));
        }

        if (xStreamBufferSpacesAvailable(audio_stream) < chunk) {
            break;
        }

        size_t written = xStreamBufferSend(
            audio_stream,
            data + offset,
            chunk,
            pdMS_TO_TICKS(AUDIO_SEND_WAIT_MS)
        );

        if (written > 0) {

            if (written > chunk) {
                written = chunk;
            }

            written &= ~((size_t)1);

            offset += written;
            audio_bytes_queued += written;

            continue;
        }

        ESP_LOGW(
            TAG,
            "Audio ring penuh: offset=%u/%u pending=%u spaces=%u",
            (unsigned)offset,
            (unsigned)len,
            (unsigned)xStreamBufferBytesAvailable(audio_stream),
            (unsigned)xStreamBufferSpacesAvailable(audio_stream)
        );
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

    /*
     * Ring masih mempunyai PCM.
     * Jangan mulai drain timer sebelum ring benar-benar kosong.
     */
    if (xStreamBufferBytesAvailable(audio_stream) != 0) {
        audio_drain_deadline_us = 0;
        return;
    }

    /*
     * Ring sudah kosong, tetapi data mungkin masih berada
     * di DMA/I2S hardware.
     *
     * Beri waktu 20 ms agar tail PCM keluar secara alami.
     */
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

    /*
     * Network/ring accounting:
     *
     * received = queued + net_drop
     */
    const uint64_t network_accounted =
        audio_bytes_queued + audio_bytes_dropped;

    const int64_t network_balance =
        (int64_t)audio_bytes_received -
        (int64_t)network_accounted;

    /*
     * Playback accounting:
     *
     * queued = played + playback_drop
     */
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
        "ring=%u, prebuffer=%u (200ms), "
        "warning=%u (100ms), critical=%u (50ms), "
        "read=%u, core=%d priority=3",
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

            /* Reset underrun timeline state */
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

            /*
             * Jangan membawa timeline dari turn sebelumnya
             * ke turn baru.
             */
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

        /*
         * Kita mulai stopwatch ketika pending berada
         * di bawah target prebuffer 9600 byte.
         *
         * Ini hanya pengukuran, tidak mengubah playback.
         */
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

                /*
                 * Berapa lama sejak PCM terakhir masuk.
                 *
                 * Ini sangat penting:
                 *
                 * input_gap besar
                 *     -> kemungkinan supply/RX terlambat
                 *
                 * input_gap kecil
                 *     -> data sebenarnya baru saja masuk,
                 *        perlu audit scheduling/queue/playback.
                 */
                const int64_t input_gap_ms =
                    (audio_last_queue_us > 0)
                        ? (now_us - audio_last_queue_us) / 1000LL
                        : -1LL;

                /*
                 * Berapa lama buffer berada di bawah
                 * prebuffer 9600 byte sebelum underrun.
                 */
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

            ESP_LOGE(
                TAG,
                "Gagal membuat audio send mutex"
            );

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

        ESP_LOGW(
            TAG,
            "Menggunakan RAM internal untuk audio buffer"
        );
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

        ESP_LOGE(
            TAG,
            "Gagal membuat static stream buffer"
        );

        heap_caps_free(buffer_mem);

        return false;
    }


    BaseType_t result =
        xTaskCreatePinnedToCore(
            audio_playback_task,
            "audio_playback",
            4096,
            NULL,
            3,
            &audio_playback_task_handle,
            1
        );


    if (result != pdPASS) {

        ESP_LOGE(
            TAG,
            "Gagal membuat audio_task/playback task: "
            "free_internal=%u largest=%u",
            (unsigned)heap_caps_get_free_size(
                MALLOC_CAP_INTERNAL
            ),
            (unsigned)heap_caps_get_largest_free_block(
                MALLOC_CAP_INTERNAL
            )
        );

        vStreamBufferDelete(audio_stream);

        audio_stream = NULL;
        audio_playback_task_handle = NULL;

        return false;
    }


    ESP_LOGI(
        TAG,
        "Audio ring buffer siap: "
        "%u byte, prebuffer=%u (200ms), "
        "warning=%u (100ms), critical=%u (50ms), "
        "target=%u B/s, playback core=1 priority=3",
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

        xSemaphoreTake(
            audio_send_mutex,
            portMAX_DELAY
        );

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

    /* Reset underrun timeline state */
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

    /* Reset underrun timeline state */
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

    /*
     * Reset timeline untuk turn baru.
     */
    audio_last_queue_us = 0;
    audio_low_since_us = 0;
    audio_last_queue_len = 0;


    /*
     * Buang PCM sisa dari turn sebelumnya.
     *
     * Penting:
     * stale PCM TIDAK dimasukkan ke audio_bytes_dropped,
     * karena bukan bagian dari audio_bytes_received turn baru.
     */
    if (audio_stream != NULL) {

        size_t stale =
            xStreamBufferBytesAvailable(audio_stream);

        if (stale > 0) {

            xStreamBufferReset(audio_stream);

            ESP_LOGW(
                TAG,
                "Audio stale PCM dibuang saat turn baru: %u byte",
                (unsigned)stale
            );
        }
    }


    uint32_t next_generation =
        audio_turn_generation + 1U;

    if (next_generation == 0U) {
        next_generation = 1U;
    }

    audio_turn_generation =
        next_generation;

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

    /* PCM16 harus genap */
    len &= ~((size_t)1);

    if (len == 0) {
        return false;
    }


    if (
        audio_stream == NULL &&
        !start_audio_playback()
    ) {
        return false;
    }


    if (audio_stream == NULL) {
        return false;
    }


    if (audio_send_mutex == NULL) {

        ESP_LOGE(
            TAG,
            "Audio send mutex belum siap"
        );

        return false;
    }


    if (
        xSemaphoreTake(
            audio_send_mutex,
            portMAX_DELAY
        ) != pdTRUE
    ) {

        ESP_LOGE(
            TAG,
            "Gagal mengambil audio send mutex"
        );

        return false;
    }


    begin_audio_turn();

    /*
     * --------------------------------------------------------------
     * Timeline measurement
     * --------------------------------------------------------------
     *
     * Catat waktu setiap PCM chunk masuk.
     *
     * Kita hanya log jika gap >= 30 ms.
     * Tujuannya mencari jeda supply PCM yang cukup panjang
     * untuk menguras playback ring.
     */

    const int64_t now_us =
        esp_timer_get_time();

    if (audio_last_queue_us != 0) {

        const int64_t gap_us =
            now_us - audio_last_queue_us;

        if (gap_us >= 30000LL) {

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
                (unsigned)xStreamBufferBytesAvailable(
                    audio_stream
                ),
                (unsigned long long)audio_bytes_received,
                (unsigned long long)audio_bytes_queued,
                (unsigned long long)audio_bytes_played
            );
        }
    }

    audio_last_queue_us = now_us;
    audio_last_queue_len = len;


    /*
     * Accounting authoritative:
     *
     * queue_audio_pcm() menjadi sumber kebenaran
     * untuk received/chunks.
     *
     * Caller lama masih mungkin melakukan:
     *
     *   audio_bytes_received += ...
     *   audio_chunks_received++;
     *
     * begin_audio_turn() di atas bisa mereset nilai tersebut.
     * Karena itu accounting di sini ditulis kembali
     * menggunakan panjang PCM yang sudah dinormalisasi genap.
     */
    audio_received_accounted += len;
    audio_chunks_accounted++;

    audio_bytes_received =
        audio_received_accounted;

    audio_chunks_received =
        audio_chunks_accounted;


    uint64_t queued_before =
        audio_bytes_queued;

    uint64_t dropped_before =
        audio_bytes_dropped;


    /*
     * Gemini PCM masuk ke playback tanpa modifikasi.
     */
    (void)send_realtime_pcm(
        pcm,
        len
    );


    const uint64_t queued_delta =
        audio_bytes_queued -
        queued_before;

    const uint64_t dropped_delta =
        audio_bytes_dropped -
        dropped_before;

    const uint64_t accounted_delta =
        queued_delta +
        dropped_delta;


    /*
     * Safety accounting:
     *
     * setiap byte yang diterima harus berakhir sebagai:
     *
     * queued ATAU network dropped.
     */
    if (
        accounted_delta <
        (uint64_t)len
    ) {

        const uint64_t missing =
            (uint64_t)len -
            accounted_delta;

        audio_bytes_dropped +=
            missing;

        ESP_LOGW(
            TAG,
            "Audio accounting guard: "
            "%llu byte -> dropped",
            (unsigned long long)missing
        );

    }
    else if (
        accounted_delta >
        (uint64_t)len
    ) {

        ESP_LOGW(
            TAG,
            "Audio accounting anomaly: "
            "accounted_delta=%llu len=%u",
            (unsigned long long)accounted_delta,
            (unsigned)len
        );
    }


    xSemaphoreGive(
        audio_send_mutex
    );


    return queued_delta ==
           (uint64_t)len;
}
