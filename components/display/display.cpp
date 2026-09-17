#include "display.h"
#include "display_driver.h"
#include "display_face.h"
#include "esp_log.h"

static const char *TAG = "DISPLAY";

void oled_init(void)
{
    display_driver_init();
    display_face_init();
}

void display_render_buffer(const uint8_t *buffer)
{
    if (!buffer) return;
    display_driver_present(buffer, OLED_WIDTH, OLED_HEIGHT);
}

void display_status(const char *text)
{
    if (!display_driver_is_ready()) display_driver_init();
    ESP_LOGI(TAG, "[OLED STATUS]: %s", text ? text : "(null)");
}

void face_set_state(face_state_t state)
{
    display_face_set_state(state);
}

face_state_t face_get_state(void)
{
    return display_face_get_state();
}

void face_show_for_ms(face_state_t state, uint32_t duration_ms)
{
    display_face_show_for_ms(state, duration_ms);
}

void display_render_mochi_gaze(int expr, int step, int sX, int sY,
                               int gaze_x, int gaze_y)
{
    if (!display_driver_is_ready()) display_driver_init();
    display_face_render_mochi_gaze(expr, step, sX, sY, gaze_x, gaze_y, 0, 0);
    display_driver_present(display_face_buffer(), OLED_WIDTH, OLED_HEIGHT);
}

void display_render_mochi(int expr, int step, int sX, int sY, int arahLirik)
{
    if (!display_driver_is_ready()) display_driver_init();
    display_face_render_mochi(expr, step, sX, sY, arahLirik);
    display_driver_present(display_face_buffer(), OLED_WIDTH, OLED_HEIGHT);
}

void face_render(void)
{
    if (!display_driver_is_ready()) display_driver_init();
    display_face_render();
    display_driver_present(display_face_buffer(), OLED_WIDTH, OLED_HEIGHT);
}
