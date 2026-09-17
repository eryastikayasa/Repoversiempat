#include "websocket.h"
#include "websocket_transport.h"
#include "websocket_event.h"
#include "gemini_protocol.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "WS_MGR";
static constexpr size_t WS_TX_AUDIO_SIZE = 3200;
static constexpr size_t WS_TX_TEXT_SIZE = 8192;
static constexpr size_t WS_TX_QUEUE_LENGTH = 3;
// Keep the websocket critical section short. Repo3 uses 512-byte PCM chunks.
static constexpr size_t PCM_SEND_CHUNK = 512;
static constexpr TickType_t AUDIO_SEND_TIMEOUT = pdMS_TO_TICKS(50);
static constexpr TickType_t AUDIO_SEND_RETRY_DELAY = pdMS_TO_TICKS(20);
static constexpr int AUDIO_SEND_RETRIES = 5;
static constexpr uint32_t TX_TASK_STACK = 8192;
static constexpr UBaseType_t TX_TASK_PRIORITY = 4;

typedef enum { WS_TX_COMMAND_SETUP = 1, WS_TX_COMMAND_AUDIO = 2, WS_TX_COMMAND_TEXT = 3 } ws_tx_command_type_t;
typedef struct { ws_tx_command_type_t type; uint32_t generation; uint16_t len; uint8_t *data; } ws_tx_command_t;
static QueueHandle_t s_tx_queue = nullptr;
static TaskHandle_t s_tx_task = nullptr;
static uint32_t s_generation = 0;
static volatile bool s_tx_error = false;

static void tx_flush_queue(void) { if (!s_tx_queue) return; ws_tx_command_t stale{}; while (xQueueReceive(s_tx_queue, &stale, 0) == pdTRUE) { free(stale.data); stale = {}; } }
static void tx_fail(void) { s_tx_error = true; s_generation++; tx_flush_queue(); }
static bool tx_state_valid(uint32_t generation) { return generation == s_generation && !s_tx_error && websocket_transport_is_connected(); }

static bool send_text_checked(esp_websocket_client_handle_t ws, const char *text, size_t len, TickType_t timeout, uint32_t generation)
{
    if (!ws || !text || len == 0 || len > WS_TX_TEXT_SIZE) return false;
    if (!tx_state_valid(generation) || websocket_transport_get_client() != ws || !esp_websocket_client_is_connected(ws)) return false;
    return esp_websocket_client_send_text(ws, text, (int)len, timeout) == (int)len;
}

static void websocket_tx_task(void *)
{
    ws_tx_command_t cmd{};
    static char b64_buf[1024];
    static char json_buf[1400];
    for (;;) {
        if (xQueueReceive(s_tx_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;
        uint8_t *data = cmd.data; cmd.data = nullptr;
        if (!tx_state_valid(cmd.generation)) { free(data); continue; }
        esp_websocket_client_handle_t ws = websocket_transport_get_client();
        if (!ws || !esp_websocket_client_is_connected(ws)) { free(data); continue; }
        if (cmd.type == WS_TX_COMMAND_SETUP) {
            char *setup = nullptr; size_t setup_len = 0;
            if (!gemini_protocol_build_setup(&setup, &setup_len)) { free(data); continue; }
            if (!send_text_checked(ws, setup, setup_len, pdMS_TO_TICKS(3000), cmd.generation)) tx_fail();
            free(setup); free(data); continue;
        }
        if (cmd.type == WS_TX_COMMAND_TEXT) {
            if (!data || cmd.len == 0 || cmd.len > WS_TX_TEXT_SIZE) { free(data); continue; }
            if (!send_text_checked(ws, (const char *)data, cmd.len, pdMS_TO_TICKS(3000), cmd.generation)) tx_fail();
            free(data); continue;
        }
        if (cmd.type == WS_TX_COMMAND_AUDIO) {
            if (!data || cmd.len == 0 || cmd.len > WS_TX_AUDIO_SIZE) { free(data); continue; }
            size_t offset = 0; bool failed = false;
            while (offset < cmd.len) {
                if (!tx_state_valid(cmd.generation) || websocket_transport_get_client() != ws || !esp_websocket_client_is_connected(ws)) { failed = true; break; }
                size_t chunk_len = cmd.len - offset;
                if (chunk_len > PCM_SEND_CHUNK) chunk_len = PCM_SEND_CHUNK;
                size_t encoded_len = 0;
                if (mbedtls_base64_encode((unsigned char *)b64_buf, sizeof(b64_buf)-1, &encoded_len, data+offset, chunk_len) != 0) { failed=true; break; }
                b64_buf[encoded_len]='\0';
                const int json_len = snprintf(json_buf, sizeof(json_buf), "{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\"%s\"}}}", b64_buf);
                if (json_len < 0 || (size_t)json_len >= sizeof(json_buf)) { failed=true; break; }
                bool sent_ok=false;
                for (int attempt=0; attempt<=AUDIO_SEND_RETRIES; ++attempt) {
                    if (attempt>0) vTaskDelay(AUDIO_SEND_RETRY_DELAY);
                    if (!tx_state_valid(cmd.generation) || websocket_transport_get_client() != ws || !esp_websocket_client_is_connected(ws)) break;
                    if (esp_websocket_client_send_text(ws,json_buf,json_len,AUDIO_SEND_TIMEOUT)==json_len) { sent_ok=true; break; }
                }
                if (!sent_ok) { failed=true; break; }
                offset += chunk_len;
            }
            if (failed) tx_fail();
            free(data); continue;
        }
        free(data);
    }
}

static bool ensure_tx_worker(void)
{
    if (s_tx_task) return true;
    if (!s_tx_queue) { s_tx_queue=xQueueCreate(WS_TX_QUEUE_LENGTH,sizeof(ws_tx_command_t)); if(!s_tx_queue)return false; }
    return xTaskCreate(websocket_tx_task,"ws_tx",TX_TASK_STACK,nullptr,TX_TASK_PRIORITY,&s_tx_task)==pdPASS;
}
static bool enqueue_command(ws_tx_command_t *cmd)
{
    if (!cmd || !s_tx_queue) return false;
    if (xQueueSend(s_tx_queue,cmd,0)==pdTRUE) return true;
    ws_tx_command_t evicted{};
    if (xQueueReceive(s_tx_queue,&evicted,0)==pdTRUE) { free(evicted.data); if(xQueueSend(s_tx_queue,cmd,0)==pdTRUE)return true; }
    free(cmd->data); cmd->data=nullptr; return false;
}

esp_err_t websocket_init(void) { const esp_err_t err=websocket_transport_init(); if(err!=ESP_OK)return err; return ensure_tx_worker()?ESP_OK:ESP_FAIL; }
esp_err_t websocket_connect(void) { return ensure_tx_worker()?websocket_transport_connect():ESP_FAIL; }
esp_err_t websocket_disconnect(void) { s_tx_error=true; s_generation++; tx_flush_queue(); return websocket_transport_disconnect(); }
bool websocket_is_connected(void) { return !s_tx_error && websocket_transport_is_connected() && websocket_event_gemini_ready(); }
esp_err_t websocket_send_text(const char *text,size_t len) { if(!text||len==0||len>WS_TX_TEXT_SIZE||!websocket_transport_is_connected())return ESP_ERR_INVALID_ARG; char *copy=(char*)malloc(len); if(!copy)return ESP_ERR_NO_MEM; memcpy(copy,text,len); ws_tx_command_t cmd{}; cmd.type=WS_TX_COMMAND_TEXT;cmd.generation=s_generation;cmd.len=(uint16_t)len;cmd.data=(uint8_t*)copy;return enqueue_command(&cmd)?ESP_OK:ESP_FAIL; }
esp_err_t websocket_send_binary(const uint8_t *data,size_t len){(void)data;(void)len;return ESP_ERR_NOT_SUPPORTED;}
bool websocket_tx_enqueue_audio(const uint8_t *data,size_t len){if(!data||len==0||len>WS_TX_AUDIO_SIZE||!websocket_is_connected())return false;uint8_t*copy=(uint8_t*)malloc(len);if(!copy)return false;memcpy(copy,data,len);ws_tx_command_t cmd{};cmd.type=WS_TX_COMMAND_AUDIO;cmd.generation=s_generation;cmd.len=(uint16_t)len;cmd.data=copy;return enqueue_command(&cmd);}
void websocket_tx_schedule_setup(void){if(!ensure_tx_worker()||!websocket_transport_is_connected()||s_tx_error)return;ws_tx_command_t cmd{};cmd.type=WS_TX_COMMAND_SETUP;cmd.generation=s_generation;(void)enqueue_command(&cmd);}
void websocket_tx_handle_event(int32_t event_id){if(event_id==WEBSOCKET_EVENT_CONNECTED){if(!ensure_tx_worker())return;s_generation++;s_tx_error=false;tx_flush_queue();websocket_tx_schedule_setup();}else if(event_id==WEBSOCKET_EVENT_DISCONNECTED||event_id==WEBSOCKET_EVENT_ERROR){s_generation++;s_tx_error=true;tx_flush_queue();}}
uint32_t websocket_tx_generation(void){return s_generation;}
