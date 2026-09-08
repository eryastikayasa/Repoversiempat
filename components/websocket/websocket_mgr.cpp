#include "websocket_mgr.h"
#include "websocket_internal.h"
#include "display.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "mbedtls/base64.h"
#include "esp_heap_caps.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stddef.h>
#include <stdint.h>

static const char *TAG = "WS_MGR";
esp_websocket_client_handle_t client = NULL;
volatile bool is_connected = false;
volatile bool setup_complete = false;
volatile bool websocket_tx_error = false;
volatile uint32_t websocket_connection_generation = 0;
char session_handle[SESSION_HANDLE_MAX_LEN] = {0};
bool session_resumable = false;

StreamBufferHandle_t audio_stream = NULL;
TaskHandle_t audio_playback_task_handle = NULL;
volatile bool audio_turn_active = false;
uint32_t audio_chunks_received = 0;
uint64_t audio_bytes_received = 0;
uint64_t audio_bytes_queued = 0;
uint32_t audio_write_calls = 0;
uint64_t audio_bytes_played = 0;
uint64_t audio_bytes_dropped = 0;
static volatile bool ws_started = false;
QueueHandle_t websocket_tx_queue = NULL;
TaskHandle_t websocket_tx_task_handle = NULL;

static void log_ws_heap(const char *where)
{
    ESP_LOGI(TAG, "HEAP[%s]: free=%u largest=%u", where,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static void websocket_tx_fail(void)
{
    websocket_tx_error = true;
    is_connected = false;
    setup_complete = false;
    ESP_LOGE(TAG, "WebSocket TX failure: audio/control TX dihentikan");
}

void websocket_tx_flush_queue(void)
{
    if (!websocket_tx_queue) return;
    ws_tx_command_t stale = {};
    unsigned flushed = 0;
    while (xQueueReceive(websocket_tx_queue, &stale, 0) == pdTRUE) {
        if (stale.data) free(stale.data);
        ++flushed;
    }
    if (flushed) ESP_LOGW(TAG, "TX queue dibersihkan: %u command", flushed);
}

static void websocket_tx_task(void *arg)
{
    (void)arg;
    ws_tx_command_t cmd = {};
    ESP_LOGI(TAG, "WebSocket TX worker dimulai - V7.0.32");
    for (;;) {
        if (xQueueReceive(websocket_tx_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;
        uint8_t *audio_data = cmd.data;
        cmd.data = NULL;
        if (cmd.generation != websocket_connection_generation || !is_connected || websocket_tx_error || !client) { free(audio_data); continue; }
        esp_websocket_client_handle_t ws = client;
        if (!esp_websocket_client_is_connected(ws)) { free(audio_data); continue; }
        if (cmd.type == WS_TX_COMMAND_SETUP) {
            char *setup_json = NULL; size_t setup_len = 0;
            if (!build_gemini_setup(&setup_json, &setup_len)) { free(audio_data); continue; }
            if (cmd.generation != websocket_connection_generation || !is_connected || websocket_tx_error || client != ws || !esp_websocket_client_is_connected(ws)) {
                free(setup_json); free(audio_data); continue;
            }
            int sent = esp_websocket_client_send_text(ws, setup_json, (int)setup_len, pdMS_TO_TICKS(5000));
            if (sent != (int)setup_len) websocket_tx_fail();
            else ESP_LOGI(TAG, "Setup Gemini terkirim: %d byte generation=%lu", sent, (unsigned)cmd.generation);
            free(setup_json); free(audio_data); continue;
        }
        if (cmd.type == WS_TX_COMMAND_AUDIO) {
            static char b64_buf[1000];
            static char json_buf[1200];
            constexpr size_t PCM_SEND_CHUNK = 640;
            constexpr TickType_t AUDIO_SEND_TIMEOUT = pdMS_TO_TICKS(3000);
            constexpr TickType_t AUDIO_SEND_RETRY_DELAY = pdMS_TO_TICKS(30);
            constexpr int AUDIO_SEND_RETRIES = 1;
            if (!audio_data || !cmd.len) { free(audio_data); continue; }
            size_t offset = 0;
            while (offset < cmd.len) {
                size_t chunk = cmd.len - offset;
                if (chunk > PCM_SEND_CHUNK) chunk = PCM_SEND_CHUNK;
                chunk &= ~((size_t)1);
                if (!chunk) break;
                size_t b64_len = 0;
                if (mbedtls_base64_encode((unsigned char *)b64_buf, sizeof(b64_buf), &b64_len,
                                          audio_data + offset, chunk) != 0) break;
                int json_len = snprintf(json_buf, sizeof(json_buf),
                                        "{\"realtimeInput\":{\"mediaChunks\":[{\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\"%.*s\"}]}}",
                                        (int)b64_len, b64_buf);
                if (json_len <= 0 || (size_t)json_len >= sizeof(json_buf)) break;
                bool sent_ok = false;
                for (int attempt = 0; attempt <= AUDIO_SEND_RETRIES; ++attempt) {
                    if (cmd.generation != websocket_connection_generation || !is_connected || websocket_tx_error || client != ws || !esp_websocket_client_is_connected(ws)) break;
                    int sent = esp_websocket_client_send_text(ws, json_buf, json_len, AUDIO_SEND_TIMEOUT);
                    if (sent == json_len) { sent_ok = true; break; }
                    if (attempt < AUDIO_SEND_RETRIES) vTaskDelay(AUDIO_SEND_RETRY_DELAY);
                }
                if (!sent_ok) { websocket_tx_fail(); break; }
                offset += chunk;
            }
            free(audio_data);
        }
    }
}

static bool websocket_tx_enqueue_internal(ws_tx_command_type_t type, const uint8_t *data, size_t len, uint32_t generation)
{
    if (!websocket_tx_queue || !data || len == 0 || len > UINT16_MAX) return false;
    ws_tx_command_t cmd = {};
    cmd.type = type;
    cmd.generation = generation;
    cmd.len = (uint16_t)len;
    cmd.data = (uint8_t *)malloc(len);
    if (!cmd.data) return false;
    memcpy(cmd.data, data, len);
    if (xQueueSend(websocket_tx_queue, &cmd, 0) != pdTRUE) {
        free(cmd.data);
        return false;
    }
    return true;
}

bool websocket_tx_enqueue_audio(const uint8_t *data, size_t len, uint32_t generation)
{
    return websocket_tx_enqueue_internal(WS_TX_COMMAND_AUDIO, data, len, generation);
}

bool websocket_tx_init(void)
{
    if (websocket_tx_queue != NULL) return true;
    websocket_tx_queue = xQueueCreate(WS_TX_QUEUE_LENGTH, sizeof(ws_tx_command_t));
    if (!websocket_tx_queue) return false;
    if (xTaskCreatePinnedToCore(websocket_tx_task, "websocket_tx", 4096, NULL, 6, &websocket_tx_task_handle, 1) != pdPASS) {
        vQueueDelete(websocket_tx_queue);
        websocket_tx_queue = NULL;
        return false;
    }
    return true;
}

void websocket_schedule_setup(uint32_t generation)
{
    char dummy = 0;
    if (!websocket_tx_enqueue_internal(WS_TX_COMMAND_SETUP, (const uint8_t *)&dummy, 1, generation))
        ESP_LOGE(TAG, "Gagal menjadwalkan setup Gemini generation=%lu", (unsigned)generation);
}

void websocket_reset_started(void)
{
    ws_started = false;
}

void websocket_disconnect(void)
{
    if (client && esp_websocket_client_is_connected(client)) esp_websocket_client_close(client, pdMS_TO_TICKS(2000));
    is_connected = false;
    setup_complete = false;
}

void websocket_cleanup_complete(void)
{
    ws_started = false;
}

bool websocket_cleanup_is_pending(void)
{
    return false;
}

static void log_audio_heap_once(void)
{
    static bool logged = false;
    if (logged) return;
    logged = true;
    log_ws_heap("audio");
}

void websocket_mgr_log_heap(void)
{
    log_audio_heap_once();
}
