#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t websocket_init(void);
esp_err_t websocket_connect(void);
esp_err_t websocket_disconnect(void);
bool websocket_is_connected(void);
esp_err_t websocket_send_text(const char *text, size_t len);
esp_err_t websocket_send_binary(const uint8_t *data, size_t len);
bool websocket_tx_enqueue_audio(const uint8_t *data, size_t len);
void websocket_tx_schedule_setup(void);
void websocket_tx_handle_event(int32_t event_id);
uint32_t websocket_tx_generation(void);
bool websocket_audio_start(void);
void websocket_audio_stop(void);
bool websocket_audio_running(void);

#ifdef __cplusplus
}
#endif
