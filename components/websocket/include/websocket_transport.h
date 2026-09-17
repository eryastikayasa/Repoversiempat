#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_websocket_client.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t websocket_transport_init(void);
esp_err_t websocket_transport_connect(void);
esp_err_t websocket_transport_disconnect(void);
bool websocket_transport_is_connected(void);
esp_err_t websocket_transport_send_text(const char *text, size_t len);
esp_err_t websocket_transport_send_binary(const uint8_t *data, size_t len);
esp_websocket_client_handle_t websocket_transport_get_client(void);
void websocket_transport_handle_event(int32_t event_id, void *event_data);
#ifdef __cplusplus
}
#endif
