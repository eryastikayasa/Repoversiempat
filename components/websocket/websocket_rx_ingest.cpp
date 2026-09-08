#include "websocket_internal.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>

static const char *TAG = "WS_INGEST";

/*
 * The WebSocket event callback must not enter the streaming parser directly.
 * During the observed failures queue_audio_pcm() could wait for ring space,
 * which made the callback spend 54-61 ms inside the audio path. That stalls
 * delivery of later WebSocket fragments and creates artificial input gaps.
 *
 * Keep a PSRAM-backed fragment pool between the WebSocket client task and the
 * RX parser. 32 x 8 KB gives 256 KB of burst absorption without touching the
 * 32 KB audio playback ring.
 */
#define RX_INGEST_SLOT_COUNT     32U
#define RX_INGEST_FRAGMENT_SIZE  (8U * 1024U)

typedef struct {
    uint32_t generation;
    uint32_t payload_len;
    uint32_t payload_offset;
    uint16_t data_len;
    uint8_t op_code;
    uint8_t slot_id;
} rx_ingest_item_t;

static QueueHandle_t ingest_queue = NULL;
static QueueHandle_t free_slot_queue = NULL;
static TaskHandle_t ingest_task_handle = NULL;
static uint8_t *ingest_buffers[RX_INGEST_SLOT_COUNT] = {0};
static bool ingest_ready = false;
static uint32_t ingest_drops = 0;

static bool ingest_allocate_pool(void)
{
    for (uint32_t i = 0; i < RX_INGEST_SLOT_COUNT; ++i) {
        ingest_buffers[i] = (uint8_t *)heap_caps_malloc(
            RX_INGEST_FRAGMENT_SIZE,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

        if (!ingest_buffers[i]) {
            ESP_LOGE(TAG, "Pool fragment gagal: slot=%u", (unsigned)i);
            return false;
        }
    }

    return true;
}

static void ingest_task(void *arg)
{
    (void)arg;

    rx_ingest_item_t item = {};

    ESP_LOGI(TAG,
             "RX ingest worker siap: slots=%u size=%u total=%uKB",
             (unsigned)RX_INGEST_SLOT_COUNT,
             (unsigned)RX_INGEST_FRAGMENT_SIZE,
             (unsigned)((RX_INGEST_SLOT_COUNT * RX_INGEST_FRAGMENT_SIZE) / 1024U));

    for (;;) {
        if (xQueueReceive(ingest_queue, &item, portMAX_DELAY) != pdTRUE)
            continue;

        if (item.slot_id >= RX_INGEST_SLOT_COUNT) {
            memset(&item, 0, sizeof(item));
            continue;
        }

        esp_websocket_event_data_t data = {};
        data.data_ptr = (const char *)ingest_buffers[item.slot_id];
        data.data_len = (int)item.data_len;
        data.op_code = item.op_code;
        data.payload_len = (int)item.payload_len;
        data.payload_offset = (int)item.payload_offset;

        (void)websocket_rx_enqueue_data(&data, item.generation);

        uint8_t slot = item.slot_id;
        (void)xQueueSend(free_slot_queue, &slot, portMAX_DELAY);
        memset(&item, 0, sizeof(item));
    }
}

bool websocket_rx_ingest_init(void)
{
    if (ingest_ready)
        return true;

    ingest_queue = xQueueCreate(
        RX_INGEST_SLOT_COUNT,
        sizeof(rx_ingest_item_t));

    free_slot_queue = xQueueCreate(
        RX_INGEST_SLOT_COUNT,
        sizeof(uint8_t));

    if (!ingest_queue || !free_slot_queue) {
        ESP_LOGE(TAG, "Gagal membuat queue RX ingest");
        return false;
    }

    if (!ingest_allocate_pool()) {
        ESP_LOGE(TAG, "Gagal menyiapkan pool RX ingest");
        return false;
    }

    for (uint8_t i = 0; i < RX_INGEST_SLOT_COUNT; ++i)
        (void)xQueueSend(free_slot_queue, &i, 0);

    if (xTaskCreate(
            ingest_task,
            "ws_ingest",
            8192,
            NULL,
            6,
            &ingest_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "Gagal membuat task RX ingest");
        return false;
    }

    ingest_ready = true;
    return true;
}

bool websocket_rx_ingest_enqueue(
    esp_websocket_event_data_t *data,
    uint32_t generation)
{
    if (!data || !data->data_ptr || data->data_len <= 0 ||
        data->payload_len <= 0 || data->payload_offset < 0 ||
        generation != websocket_connection_generation || !is_connected) {
        return false;
    }

    if (!ingest_ready && !websocket_rx_ingest_init())
        return false;

    const size_t len = (size_t)data->data_len;

    /* Normal Gemini fragments are <= 8 KB with the current WS buffer. */
    if (len > RX_INGEST_FRAGMENT_SIZE) {
        ++ingest_drops;
        ESP_LOGW(TAG,
                 "RX ingest drop: fragment=%u > slot=%u drops=%lu",
                 (unsigned)len,
                 (unsigned)RX_INGEST_FRAGMENT_SIZE,
                 (unsigned long)ingest_drops);
        return false;
    }

    uint8_t slot = 0;
    if (xQueueReceive(free_slot_queue, &slot, 0) != pdTRUE) {
        ++ingest_drops;
        ESP_LOGW(TAG,
                 "RX ingest pool penuh: drops=%lu",
                 (unsigned long)ingest_drops);
        return false;
    }

    memcpy(ingest_buffers[slot], data->data_ptr, len);

    rx_ingest_item_t item = {};
    item.generation = generation;
    item.payload_len = (uint32_t)data->payload_len;
    item.payload_offset = (uint32_t)data->payload_offset;
    item.data_len = (uint16_t)len;
    item.op_code = data->op_code;
    item.slot_id = slot;

    if (xQueueSend(ingest_queue, &item, 0) != pdTRUE) {
        (void)xQueueSend(free_slot_queue, &slot, 0);
        ++ingest_drops;
        ESP_LOGW(TAG,
                 "RX ingest queue penuh: drops=%lu",
                 (unsigned long)ingest_drops);
        return false;
    }

    return true;
}
