#include "display_engine.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "display_text.h"
#include "display_driver.h"

namespace {
constexpr uint32_t FRAME_MS = 50;
constexpr size_t FB_SIZE = 128 * 64 / 8;
constexpr uint32_t STACK = 4096;
static EXT_RAM_BSS_ATTR uint8_t s_final[FB_SIZE] = {0};
static TaskHandle_t s_task = nullptr;
static volatile bool s_running = false;
static bool s_initialized = false;
static const char *TAG = "DISPLAY_ENGINE";

static void compose()
{
    const uint8_t *face = display_face_buffer();
    const uint8_t *text = display_text_buffer();
    memset(s_final, 0, sizeof(s_final));
    if (face) memcpy(s_final, face, sizeof(s_final));
    if (text) for (size_t i = 0; i < sizeof(s_final); ++i) s_final[i] |= text[i];
}

static void render_text_layer()
{
    if (display_text_has_gemini()) display_text_render_gemini();
    else if (display_text_has_user()) display_text_render_user();
    else display_text_render_status();
}

static void task(void *)
{
    TickType_t last = xTaskGetTickCount();
    ESP_LOGI(TAG, "Display task: 128x64 framebuffer=%uB PSRAM frame=%ums (~20 FPS) priority=3 core=1", (unsigned)FB_SIZE, (unsigned)FRAME_MS);
    while (s_running) {
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
        display_face_update(now);
        display_text_update(now);
        render_text_layer();
        compose();
        display_driver_present(s_final, 128, 64);
        vTaskDelayUntil(&last, pdMS_TO_TICKS(FRAME_MS));
    }
    s_task = nullptr;
    vTaskDelete(nullptr);
}
}

void display_engine_init(void)
{
    if (s_initialized) return;
    display_driver_init();
    display_face_init();
    display_text_init();
    s_initialized = true;
}

void display_engine_start(void)
{
    if (!s_initialized) display_engine_init();
    if (s_running || s_task) return;
    s_running = true;
    if (xTaskCreatePinnedToCore(task, "display_engine", STACK, nullptr, 3, &s_task, 1) != pdPASS) {
        s_running = false;
        s_task = nullptr;
        ESP_LOGE(TAG, "Display engine task create gagal");
    }
}

void display_engine_stop(void)
{
    s_running = false;
}

void display_set_face_state(face_state_t face)
{
    display_face_set_state(face);
}

void display_set_status(const char *status)
{
    display_text_set_status(status ? status : "");
}

void display_set_system_state(face_state_t face, const char *status)
{
    display_set_face_state(face);
    display_set_status(status);
}
