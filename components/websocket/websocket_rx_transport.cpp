#include "websocket_internal.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "WS_RX";
static volatile bool ws_rx_reset_pending = false;

static uint8_t *rx_slots[WS_RX_SLOT_COUNT] = {0};
static size_t rx_slot_capacity[WS_RX_SLOT_COUNT] = {0};
static bool rx_slot_in_use[WS_RX_SLOT_COUNT] = {false};
static bool rx_slots_preallocated = false;

static int rx_capture_slot = -1;
static size_t rx_received = 0;
static size_t rx_expected = 0;
static uint32_t rx_generation = 0;
static bool rx_active = false;

static uint32_t rx_fragments_received = 0;
static uint32_t rx_fragments_dropped = 0;
static uint32_t rx_complete_messages = 0;
static UBaseType_t rx_queue_high_water = 0;
static uint32_t rx_sequence_errors = 0;
static uint32_t rx_buffer_drops = 0;
static uint32_t rx_queue_drops = 0;
static uint32_t rx_invalid_json = 0;
static uint32_t rx_oversize_drops = 0;
static uint32_t rx_largest_payload = 0;

static void release_slot(uint8_t slot_id)
{
    if (slot_id < WS_RX_SLOT_COUNT) rx_slot_in_use[slot_id] = false;
}

static int reserve_slot(void)
{
    for (int i = 0; i < WS_RX_SLOT_COUNT; ++i) {
        if (!rx_slot_in_use[i]) {
            rx_slot_in_use[i] = true;
            return i;
        }
    }
    return -1;
}

static bool allocate_slot(int slot, size_t target)
{
    if (slot < 0 || slot >= WS_RX_SLOT_COUNT || target == 0 ||
        target > WS_RX_MAX_PAYLOAD_SIZE + WS_RX_TERMINATOR_SIZE) return false;

    uint8_t *p = (uint8_t *)heap_caps_malloc(
        target, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = (uint8_t *)heap_caps_malloc(target, MALLOC_CAP_8BIT);
    if (!p) return false;

    if (rx_slots[slot]) heap_caps_free(rx_slots[slot]);
    rx_slots[slot] = p;
    rx_slot_capacity[slot] = target;
    return true;
}

static bool preallocate_rx_slots(void)
{
    if (rx_slots_preallocated) return true;

    for (int i = 0; i < WS_RX_SLOT_COUNT; ++i) {
        if (!allocate_slot(i, WS_RX_SLOT_SIZE + WS_RX_TERMINATOR_SIZE)) {
            ESP_LOGE(TAG, "Prealokasi RX slot gagal: slot=%d", i);
            return false;
        }
        release_slot((uint8_t)i);
    }

    rx_slots_preallocated = true;
    ESP_LOGI(TAG, "RX transport slot pool siap: %dx%uB",
             (unsigned)WS_RX_SLOT_COUNT, (unsigned)WS_RX_SLOT_SIZE);
    return true;
}

static bool ensure_slot_buffer(int slot, size_t required_payload)
{
    if (slot < 0 || slot >= WS_RX_SLOT_COUNT || required_payload == 0 ||
        required_payload > WS_RX_MAX_PAYLOAD_SIZE) return false;

    const size_t needed = required_payload + WS_RX_TERMINATOR_SIZE;
    if (rx_slots[slot] && rx_slot_capacity[slot] >= needed) return true;

    size_t target = rx_slot_capacity[slot] ? rx_slot_capacity[slot] : WS_RX_SLOT_SIZE;
    while (target < needed) {
        if (target >= WS_RX_MAX_PAYLOAD_SIZE + WS_RX_TERMINATOR_SIZE) {
            target = needed;
            break;
        }
        target *= 2;
    }
    if (target > WS_RX_MAX_PAYLOAD_SIZE + WS_RX_TERMINATOR_SIZE)
        target = WS_RX_MAX_PAYLOAD_SIZE + WS_RX_TERMINATOR_SIZE;

    return allocate_slot(slot, target);
}

static void free_command(ws_rx_command_t *cmd)
{
    if (!cmd) return;
    release_slot(cmd->slot_id);
    cmd->buffer = NULL;
}

static void log_rx_stats(const char *reason, uint32_t message_len, uint32_t process_ms,
                         size_t heap_before, size_t heap_after,
                         size_t largest_before, size_t largest_after)
{
    if (message_len <= 8192 && (rx_complete_messages % 10) != 0) return;

    ESP_LOGI(TAG,
             "RX STATS [%s]: fragments=%lu dropped=%lu messages=%lu queue_hwm=%u seq_err=%lu buffer_drop=%lu queue_drop=%lu invalid=%lu oversize=%lu max_payload=%lu",
             reason,
             (unsigned long)rx_fragments_received,
             (unsigned long)rx_fragments_dropped,
             (unsigned long)rx_complete_messages,
             (unsigned)rx_queue_high_water,
             (unsigned long)rx_sequence_errors,
             (unsigned long)rx_buffer_drops,
             (unsigned long)rx_queue_drops,
             (unsigned long)rx_invalid_json,
             (unsigned long)rx_oversize_drops,
             (unsigned long)rx_largest_payload);

    ESP_LOGI(TAG, "RX PROCESS: len=%lu time=%lu ms heap=%u->%u largest=%u->%u",
             (unsigned long)message_len, (unsigned long)process_ms,
             (unsigned)heap_before, (unsigned)heap_after,
             (unsigned)largest_before, (unsigned)largest_after);
}

static void reset_capture_state(void)
{
    if (rx_capture_slot >= 0 && rx_capture_slot < WS_RX_SLOT_COUNT)
        release_slot((uint8_t)rx_capture_slot);
    rx_capture_slot = -1;
    rx_received = 0;
    rx_expected = 0;
    rx_generation = 0;
    rx_active = false;
}

static void websocket_rx_task(void *arg)
{
    (void)arg;
    ws_rx_command_t cmd = {};
    ESP_LOGI(TAG, "WebSocket RX worker dimulai - transport assembler only");

    for (;;) {
        if (xQueueReceive(websocket_rx_queue, &cmd, pdMS_TO_TICKS(50)) != pdTRUE) {
            if (ws_rx_reset_pending) {
                ws_rx_reset_pending = false;
                reset_rx_buffer();
            }
            continue;
        }

        if (ws_rx_reset_pending) {
            ws_rx_reset_pending = false;
            reset_rx_buffer();
        }

        if (!cmd.buffer || cmd.len == 0) {
            free_command(&cmd);
            memset(&cmd, 0, sizeof(cmd));
            continue;
        }

        if (cmd.generation != websocket_connection_generation || !is_connected) {
            free_command(&cmd);
            memset(&cmd, 0, sizeof(cmd));
            continue;
        }

        const size_t heap_before = esp_get_free_heap_size();
        const size_t largest_before = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        const int64_t start_us = esp_timer_get_time();

        /* Protocol parsing belongs to websocket_json.cpp. Transport only
         * delivers a complete raw JSON message. */
        process_gemini_message((const char *)cmd.buffer, (size_t)cmd.len);

        const uint32_t process_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
        const size_t heap_after = esp_get_free_heap_size();
        const size_t largest_after = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        ++rx_complete_messages;
        log_rx_stats("processed", cmd.len, process_ms,
                     heap_before, heap_after, largest_before, largest_after);

        free_command(&cmd);
        memset(&cmd, 0, sizeof(cmd));
    }
}

bool websocket_rx_init(void)
{
    if (!websocket_rx_queue) {
        websocket_rx_queue = xQueueCreate(WS_RX_QUEUE_LENGTH, sizeof(ws_rx_command_t));
        if (!websocket_rx_queue) {
            ESP_LOGE(TAG, "Gagal membuat RX queue");
            return false;
        }
    }

    if (!preallocate_rx_slots()) return false;

    if (!websocket_rx_task_handle) {
        if (xTaskCreate(websocket_rx_task, "ws_rx", 8192, NULL, 4,
                        &websocket_rx_task_handle) != pdPASS) {
            ESP_LOGE(TAG, "Gagal membuat RX worker");
            return false;
        }
    }
    return true;
}

void websocket_rx_request_reset(void)
{
    ws_rx_reset_pending = true;
}

void websocket_rx_flush_queue(void)
{
    if (!websocket_rx_queue) return;

    ws_rx_command_t stale = {};
    size_t flushed = 0;
    while (xQueueReceive(websocket_rx_queue, &stale, 0) == pdTRUE) {
        release_slot(stale.slot_id);
        ++flushed;
    }
    if (flushed)
        ESP_LOGW(TAG, "RX queue dibersihkan: %u message", (unsigned)flushed);
}

void websocket_rx_note_invalid_json(size_t len)
{
    ++rx_invalid_json;
    ESP_LOGW(TAG, "RX INVALID JSON #%lu: len=%u",
             (unsigned long)rx_invalid_json, (unsigned)len);
}

bool websocket_rx_enqueue_data(esp_websocket_event_data_t *data, uint32_t generation)
{
    if (!data || !data->data_ptr || data->data_len <= 0 || data->payload_len <= 0 ||
        data->payload_offset < 0 || generation != websocket_connection_generation ||
        !is_connected) {
        ++rx_fragments_dropped;
        return false;
    }

    ++rx_fragments_received;

    const size_t payload_len = (size_t)data->payload_len;
    const size_t offset = (size_t)data->payload_offset;
    const size_t len = (size_t)data->data_len;

    if (payload_len > rx_largest_payload) rx_largest_payload = (uint32_t)payload_len;
    if (payload_len > WS_RX_MAX_PAYLOAD_SIZE || offset > payload_len ||
        len > payload_len - offset) {
        ++rx_fragments_dropped;
        if (payload_len > WS_RX_MAX_PAYLOAD_SIZE) ++rx_oversize_drops;
        return false;
    }

    if (offset == 0) {
        if (rx_active) {
            ++rx_fragments_dropped;
            ++rx_sequence_errors;
            reset_capture_state();
        }

        const int slot = reserve_slot();
        if (slot < 0) {
            ++rx_fragments_dropped;
            ++rx_buffer_drops;
            return false;
        }

        if (!ensure_slot_buffer(slot, payload_len)) {
            ++rx_fragments_dropped;
            ++rx_buffer_drops;
            release_slot((uint8_t)slot);
            return false;
        }

        rx_capture_slot = slot;
        rx_received = 0;
        rx_expected = payload_len;
        rx_generation = generation;
        rx_active = true;
    } else if (!rx_active || rx_capture_slot < 0 || rx_capture_slot >= WS_RX_SLOT_COUNT ||
               rx_expected != payload_len || rx_generation != generation ||
               offset != rx_received) {
        ++rx_fragments_dropped;
        ++rx_sequence_errors;
        reset_capture_state();
        return false;
    }

    if (!rx_slots[rx_capture_slot] || len > rx_expected - rx_received) {
        ++rx_fragments_dropped;
        ++rx_sequence_errors;
        reset_capture_state();
        return false;
    }

    memcpy(rx_slots[rx_capture_slot] + offset, data->data_ptr, len);
    rx_received += len;

    if (rx_received != rx_expected) return true;

    rx_slots[rx_capture_slot][rx_expected] = '\0';

    ws_rx_command_t cmd = {};
    cmd.generation = generation;
    cmd.buffer = rx_slots[rx_capture_slot];
    cmd.len = (uint32_t)rx_expected;
    cmd.slot_id = (uint8_t)rx_capture_slot;

    rx_active = false;
    rx_capture_slot = -1;
    rx_received = 0;
    rx_expected = 0;
    rx_generation = 0;

    if (xQueueSend(websocket_rx_queue, &cmd, 0) != pdTRUE) {
        ++rx_fragments_dropped;
        ++rx_queue_drops;
        release_slot(cmd.slot_id);
        return false;
    }

    const UBaseType_t waiting = uxQueueMessagesWaiting(websocket_rx_queue);
    if (waiting > rx_queue_high_water) rx_queue_high_water = waiting;
    return true;
}

void reset_rx_buffer(void)
{
    reset_capture_state();
}

bool ensure_rx_buffer(size_t required_size)
{
    return required_size > 0 &&
           required_size <= WS_RX_MAX_PAYLOAD_SIZE &&
           required_size <= WS_RX_SLOT_SIZE;
}

void process_websocket_payload(esp_websocket_event_data_t *data)
{
    if (!data) return;
    (void)websocket_rx_enqueue_data(data, websocket_connection_generation);
}
