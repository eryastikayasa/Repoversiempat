#pragma once

#include "driver/gpio.h"
#include "display_face.h"
#include <stdint.h>
#include <stdbool.h>

// Repo4 hardware compatibility facade; physical configuration is owned by display_driver.
#define OLED_SDA_PIN  GPIO_NUM_41
#define OLED_SCL_PIN  GPIO_NUM_42
#define OLED_I2C_ADDR 0x3C
#define OLED_WIDTH    128
#define OLED_HEIGHT   64

#ifdef __cplusplus
extern "C" {
#endif

void oled_init(void);
void display_status(const char *status);
void face_render(void);
void display_render_buffer(const uint8_t *buffer);

// Legacy compatibility APIs retained because Repo4 application code still calls them.
void display_render_mochi(int expr, int step, int sX, int sY, int arahLirik);
void display_render_mochi_gaze(
    int expr,
    int step,
    int sX,
    int sY,
    int gaze_x,
    int gaze_y
);

void face_animation_start(void);
void face_animation_stop(void);
void face_set_state(face_state_t state);
face_state_t face_get_state(void);
void face_show_for_ms(face_state_t state, uint32_t duration_ms);

#ifdef __cplusplus
}
#endif
