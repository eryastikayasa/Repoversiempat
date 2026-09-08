#include "websocket_internal.h"
#include "display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"

#include <stdint.h>
#include <string.h>

static const char *TAG = "WS_EVENT";
static volatile bool lifecycle_invalidated = false;
static volatile bool websocket_cleanup_pending = false;
static volatile bool websocket_finish_received = false;
static int64_t ws_audio_last_event_us = 0;

static void invalidate_connection_generation(void)
{
    if (lifecycle_invalidated) return;
    lifecycle_invalidated = true;
    websocket_connection_generation = websocket_connection_generation + 1;
    ESP_LOGW(TAG, "Connection generation invalidated: %lu",
             (unsigned long)websocket_connection_generation);
    audio_engine_notify(AUDIO_ENGINE_EVENT_GENERATION_CHANGED,
                        websocket_connection_generation);
}

static void log_close_diagnostics(const char *event_name, const esp_websocket_event_data_t *data)
{
    if (!data) {
        ESP_LOGW(TAG, "%s: event_data=NULL close_status=0", event_name);
        return;
    }

    ESP_LOGW(TAG,
             "%s: close_status_code=%d (0x%04X) data_len=%d payload_len=%d payload_offset=%d opcode=0x%02X fin=%d",
             event_name,
             (int)data->close_status_code,
             (unsigned)((uint16_t)data->close_status_code),
             (int)data->data_len,
             (int)data->payload_len,
             (int)data->payload_offset,
             (unsigned)data->op_code,
             data->fin ? 1 : 0);

    if (data->data_ptr && data->data_len > 0) {
        size_t n = (size_t)data->data_len;
        if (n > 96) n = 96;
        char preview[97];
        for (size_t i = 0; i < n; ++i) {
            unsigned char c = (unsigned char)data->data_ptr[i];
            preview[i] = (c >= 32 && c <= 126) ? (char)c : '.';
        }
        preview[n] = '\0';
        ESP_LOGW(TAG, "%s: close/data preview=%s", event_name, preview);
    }
}

void websocket_event_handler(void *handler_args, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    (void)base;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    esp_websocket_client_handle_t event_client =
        (esp_websocket_client_handle_t)handler_args;

    if (client != NULL && event_client != NULL && event_client != client) {
        ESP_LOGW(TAG, "Event dari client lama diabaikan: event=%ld", (long)event_id);
        return;
    }

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WebSocket TERHUBUNG ke Gemini!");
            lifecycle_invalidated = false;
            websocket_cleanup_pending = false;
            websocket_finish_received = false;
            ws_audio_last_event_us = 0;
            is_connected = true;
            setup_complete = false;
            websocket_tx_error = false;
            websocket_connection_generation = websocket_connection_generation + 1;
            ESP_LOGI(TAG, "Connection generation=%lu",
                     (unsigned long)websocket_connection_generation);
            /* Transport lifecycle only informs AudioEngine of the new generation.
             * AudioEngine owns the resulting audio reset/turn state transition. */
            audio_engine_notify(AUDIO_ENGINE_EVENT_GENERATION_CHANGED,
                                websocket_connection_generation);
            websocket_tx_flush_queue();
            websocket_rx_flush_queue();
            websocket_rx_request_reset();
            (void)websocket_rx_ingest_init();
            display_status("AI Terhubung...");
            websocket_schedule_setup(websocket_connection_generation);
            break;

        case WEBSOCKET_EVENT_DATA:
            if (!data) break;
            if (!is_connected || websocket_tx_error) break;
            if (data->op_code == 0x08) {
                ESP_LOGW(TAG, "GEMINI CLOSE FRAME");
                log_close_diagnostics("CLOSE_FRAME", data);
                break;
            }
            if ((data->op_code == 0x00 || data->op_code == 0x01 || data->op_code == 0x02) &&
                data->data_ptr && data->data_len > 0) {
                const int64_t event_start_us = esp_timer_get_time();
                const int64_t event_gap_ms =
                    (ws_audio_last_event_us > 0)
                        ? (event_start_us - ws_audio_last_event_us) / 1000LL
                        : -1LL;

                /* WebSocket callback only hands transport fragments to RX worker. */
                (void)websocket_rx_ingest_enqueue(
                    data,
                    websocket_connection_generation);

                const int64_t callback_ms =
                    (esp_timer_get_time() - event_start_us) / 1000LL;
                ws_audio_last_event_us = esp_timer_get_time();

                if (event_gap_ms >= 1000LL || callback_ms >= 1000LL) {
                    ESP_LOGW(TAG,
                             "AUDIO WS TIMELINE: event_gap=%lldms callback=%lldms frag=%d payload=%d offset=%d",
                             (long long)event_gap_ms,
                             (long long)callback_ms,
                             (int)data->data_len,
                             (int)data->payload_len,
                             (int)data->payload_offset);
                }
            }
            break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket Error!");
            if (data) {
                ESP_LOGE(TAG,
                         "WS error_type=%d sock_errno=%d tls_esp_err=0x%x tls_stack_err=0x%x handshake=%d close_status=%d",
                         (int)data->error_handle.error_type,
                         data->error_handle.esp_transport_sock_errno,
                         (unsigned)data->error_handle.esp_tls_last_esp_err,
                         (unsigned)data->error_handle.esp_tls_stack_err,
                         data->error_handle.esp_ws_handshake_status_code,
                         (int)data->close_status_code);
                log_close_diagnostics("ERROR", data);
            }
            is_connected = false;
            setup_complete = false;
            websocket_tx_error = true;
            face_set_state(FACE_ERROR);
            display_status("AI Error!");
            invalidate_connection_generation();
            websocket_tx_flush_queue();
            websocket_rx_flush_queue();
            websocket_rx_request_reset();
            websocket_cleanup_pending = true;
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "WebSocket TERPUTUS dari Gemini");
            log_close_diagnostics("DISCONNECTED", data);
            is_connected = false;
            setup_complete = false;
            websocket_tx_error = true;
            invalidate_connection_generation();
            websocket_tx_flush_queue();
            websocket_rx_flush_queue();
            websocket_rx_request_reset();
            display_status("AI Disconnected");
            if (session_resumable && session_handle[0] != '\0')
                ESP_LOGI(TAG, "Session resumption handle dipertahankan");
            websocket_cleanup_pending = true;
            break;

        case WEBSOCKET_EVENT_CLOSED:
            ESP_LOGW(TAG, "WebSocket CLOSED");
            log_close_diagnostics("CLOSED", data);
            is_connected = false;
            setup_complete = false;
            websocket_tx_error = true;
            invalidate_connection_generation();
            websocket_tx_flush_queue();
            websocket_rx_flush_queue();
            websocket_rx_request_reset();
            websocket_cleanup_pending = true;
            break;

        case WEBSOCKET_EVENT_FINISH:
            ESP_LOGI(TAG, "WebSocket FINISH");
            log_close_diagnostics("FINISH", data);
            websocket_finish_received = true;
            websocket_cleanup_pending = true;
            websocket_reset_started();
            break;

        default:
            break;
    }
}

bool websocket_cleanup_is_pending(void)
{
    return websocket_cleanup_pending && websocket_finish_received;
}

void websocket_cleanup_complete(void)
{
    websocket_cleanup_pending = false;
    websocket_finish_received = false;
}
