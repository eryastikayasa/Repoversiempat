#include "websocket_internal.h"

QueueHandle_t websocket_rx_queue = NULL;
TaskHandle_t websocket_rx_task_handle = NULL;
