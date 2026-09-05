#include "display.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

static const char *TAG = "FACE_ANIM";
static TaskHandle_t anim_task_handle = NULL;
static SemaphoreHandle_t anim_start_mutex = NULL;

// Temporary Face override requested by Gemini tool.
// Non-blocking: the animation task checks the deadline periodically.
static face_state_t face_override_previous = FACE_IDLE;
static int64_t face_override_until_us = 0;

// OLED is relatively slow at 400 kHz. 45 ms keeps gaze movement visibly smooth
// without turning the animation task into an aggressive I2C producer.
static constexpr TickType_t FRAME_DELAY = pdMS_TO_TICKS(45);

static uint32_t rnd(uint32_t max_value)
{
    return max_value ? esp_random() % max_value : 0;
}

static bool state_ok(face_state_t state)
{
    return face_get_state() == state;
}

static void render(int expr, int step, int sx, int sy, int gaze_x, int gaze_y)
{
    display_render_mochi_gaze(expr, step, sx, sy, gaze_x, gaze_y);
}

static bool smooth_gaze(face_state_t state,
                        int expr,
                        int from_x,
                        int from_y,
                        int to_x,
                        int to_y,
                        uint32_t frames)
{
    if (!state_ok(state)) return false;
    if (frames == 0) frames = 1;

    for (uint32_t i = 1; i <= frames; ++i) {
        if (!state_ok(state)) return false;

        // Smoothstep avoids a robotic constant-speed jump at the endpoints.
        float t = (float)i / (float)frames;
        t = t * t * (3.0f - 2.0f * t);

        int gx = from_x + (int)((to_x - from_x) * t);
        int gy = from_y + (int)((to_y - from_y) * t);
        render(expr, 0, 0, 0, gx, gy);
        vTaskDelay(FRAME_DELAY);
    }

    return state_ok(state);
}

static bool blink(face_state_t state, int expr, int gaze_x, int gaze_y)
{
    if (!state_ok(state)) return false;

    // A short close/open cycle looks more natural than an instantaneous swap.
    render(expr, 1, 0, 0, gaze_x, gaze_y);
    vTaskDelay(pdMS_TO_TICKS(75 + rnd(25)));
    if (!state_ok(state)) return false;

    render(expr, 0, 0, 0, gaze_x, gaze_y);
    vTaskDelay(pdMS_TO_TICKS(55));
    return state_ok(state);
}

static bool natural_blink(face_state_t state, int expr, int gaze_x, int gaze_y)
{
    if (!blink(state, expr, gaze_x, gaze_y)) return false;

    // Occasional double blink, but deliberately uncommon.
    if (rnd(6) == 0) {
        vTaskDelay(pdMS_TO_TICKS(110 + rnd(90)));
        if (!blink(state, expr, gaze_x, gaze_y)) return false;
    }
    return true;
}

static void idle_sequence(void)
{
    // Do not animate continuously while idle; this keeps the device calm.
    vTaskDelay(pdMS_TO_TICKS(900 + rnd(1500)));
    if (!state_ok(FACE_IDLE)) return;

    uint32_t b = rnd(100);

    // Natural blink is the most common idle action.
    if (b < 28) {
        natural_blink(FACE_IDLE, 0, 0, 0);
        return;
    }

    int target_x = 0;
    int target_y = 0;

    if (b < 52) {
        target_x = -6;
    } else if (b < 76) {
        target_x = 6;
    } else if (b < 90) {
        target_x = (rnd(2) == 0) ? -4 : 4;
    } else {
        // Very occasional upward glance.
        target_y = -4;
        target_x = (int)rnd(5) - 2;
    }

    // Move there, hold briefly, then return to center.
    if (!smooth_gaze(FACE_IDLE, 0, 0, 0, target_x, target_y, 5)) return;
    vTaskDelay(pdMS_TO_TICKS(220 + rnd(300)));
    if (!smooth_gaze(FACE_IDLE, 0, target_x, target_y, 0, 0, 5)) return;

    // Sometimes blink after looking around.
    if (rnd(4) == 0) natural_blink(FACE_IDLE, 0, 0, 0);
}

static void listening_sequence(void)
{
    // Listening is attentive: mostly center, with small believable gaze shifts.
    if (!smooth_gaze(FACE_LISTENING, 1, 0, 0, 0, 0, 2)) return;
    vTaskDelay(pdMS_TO_TICKS(350 + rnd(500)));
    if (!state_ok(FACE_LISTENING)) return;

    uint32_t b = rnd(100);
    int target_x = 0;
    int target_y = 0;

    if (b < 24) target_x = -5;
    else if (b < 48) target_x = 5;
    else if (b < 58) target_y = -3;
    else {
        natural_blink(FACE_LISTENING, 1, 0, 0);
        return;
    }

    if (!smooth_gaze(FACE_LISTENING, 1, 0, 0, target_x, target_y, 4)) return;
    vTaskDelay(pdMS_TO_TICKS(180 + rnd(260)));
    if (!smooth_gaze(FACE_LISTENING, 1, target_x, target_y, 0, 0, 4)) return;

    if (rnd(3) == 0) natural_blink(FACE_LISTENING, 1, 0, 0);
}

static void thinking_sequence(void)
{
    if (!state_ok(FACE_THINKING)) return;

    // Thinking: eyes naturally drift upward, then settle back down.
    if (!smooth_gaze(FACE_THINKING, 0, 0, 0, 0, -5, 5)) return;
    vTaskDelay(pdMS_TO_TICKS(350 + rnd(450)));
    if (!state_ok(FACE_THINKING)) return;

    if (rnd(3) != 0) {
        if (!blink(FACE_THINKING, 0, 0, -5)) return;
    }

    if (!state_ok(FACE_THINKING)) return;
    vTaskDelay(pdMS_TO_TICKS(250 + rnd(350)));
    smooth_gaze(FACE_THINKING, 0, 0, -5, 0, 0, 5);
}

static void speaking_sequence(void)
{
    // Speaking stays expressive but restrained so the face does not distract.
    uint32_t b = rnd(100);
    int target_x = 0;
    int target_y = 0;

    if (b < 18) target_x = -3;
    else if (b < 36) target_x = 3;
    else if (b < 42) target_y = -2;

    if (!smooth_gaze(FACE_SPEAKING, 2, 0, 0, target_x, target_y, 3)) return;
    vTaskDelay(pdMS_TO_TICKS(500 + rnd(800)));
    if (!state_ok(FACE_SPEAKING)) return;

    if (rnd(3) == 0) {
        natural_blink(FACE_SPEAKING, 2, target_x, target_y);
    }

    smooth_gaze(FACE_SPEAKING, 2, target_x, target_y, 0, 0, 3);
}

static void happy_sequence(void)
{
    if (!state_ok(FACE_HAPPY)) return;

    // Keep HAPPY as its own state. Do not silently change it to LISTENING.
    render(2, 2, 0, 0, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(450));
    if (!state_ok(FACE_HAPPY)) return;

    render(2, 2, 0, -1, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(180));
    if (!state_ok(FACE_HAPPY)) return;

    render(2, 2, 0, 0, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(250 + rnd(250)));
}

static void sad_sequence(void)
{
    if (!state_ok(FACE_SAD)) return;

    render(6, 0, 0, 0, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(800 + rnd(600)));
}

static void error_sequence(void)
{
    if (!state_ok(FACE_ERROR)) return;

    // Short shake, then settle. No high-frequency loop.
    render(99, 0, -1, 0, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(65));
    if (!state_ok(FACE_ERROR)) return;

    render(99, 0, 1, 0, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(65));
    if (!state_ok(FACE_ERROR)) return;

    render(99, 0, 0, 0, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(450));
}

static void sleep_sequence(void)
{
    if (!state_ok(FACE_SLEEP)) return;

    render(0, 3, 0, 0, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(1200));
}

static void state_sequence(face_state_t state)
{
    switch (state) {
        case FACE_IDLE:      idle_sequence(); break;
        case FACE_LISTENING: listening_sequence(); break;
        case FACE_THINKING:  thinking_sequence(); break;
        case FACE_SPEAKING:  speaking_sequence(); break;
        case FACE_HAPPY:     happy_sequence(); break;
        case FACE_SAD:       sad_sequence(); break;
        case FACE_ERROR:     error_sequence(); break;
        case FACE_SLEEP:     sleep_sequence(); break;
        default:             idle_sequence(); break;
    }
}

void face_show_for_ms(face_state_t state, uint32_t duration_ms)
{
    face_override_previous = face_get_state();
    face_set_state(state);

    if (duration_ms == 0) {
        face_override_until_us = 0;
        return;
    }

    face_override_until_us =
        esp_timer_get_time() + ((int64_t)duration_ms * 1000LL);

    ESP_LOGI(TAG,
             "FACE TOOL: state=%d duration=%lu ms previous=%d",
             (int)state,
             (unsigned long)duration_ms,
             (int)face_override_previous);
}

static void face_animation_task(void *arg)
{
    (void)arg;
    oled_init();

    while (1) {
    if (face_override_until_us > 0 &&
        esp_timer_get_time() >= face_override_until_us) {

        face_override_until_us = 0;

        face_state_t restore_state = face_override_previous;

        // Jangan restore ke state temporary yang sudah berakhir.
        face_set_state(restore_state);

        ESP_LOGI(TAG,
                 "FACE TOOL: selesai 5 detik -> restore state=%d",
                 (int)restore_state);
    }

    state_sequence(face_get_state());
    }
}

void face_animation_start(void)
{
    if (!anim_start_mutex) {
        anim_start_mutex = xSemaphoreCreateMutex();
        if (!anim_start_mutex) {
            ESP_LOGE(TAG, "Gagal membuat mutex animasi OLED");
            return;
        }
    }

    xSemaphoreTake(anim_start_mutex, portMAX_DELAY);

    if (!anim_task_handle) {
        BaseType_t ok = xTaskCreate(
            face_animation_task,
            "face_anim",
            6144,
            NULL,
            2,
            &anim_task_handle
        );

        if (ok != pdPASS) {
            ESP_LOGE(TAG, "Gagal membuat task animasi OLED");
            anim_task_handle = NULL;
        } else {
            ESP_LOGI(TAG, "Mochi OLED animation aktif");
        }
    }

    xSemaphoreGive(anim_start_mutex);
}

void face_animation_stop(void)
{
    if (!anim_start_mutex) return;

    xSemaphoreTake(anim_start_mutex, portMAX_DELAY);
    if (anim_task_handle) {
        vTaskDelete(anim_task_handle);
        anim_task_handle = NULL;
    }
    xSemaphoreGive(anim_start_mutex);
}
