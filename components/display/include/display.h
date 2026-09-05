#pragma once

#include "driver/gpio.h"
#include <stdint.h>
#include <stdbool.h>

// XiaoZhi bread-compact-wifi-128x64 reference pinout.
#define OLED_SDA_PIN  GPIO_NUM_41
#define OLED_SCL_PIN  GPIO_NUM_42
#define OLED_I2C_ADDR 0x3C
#define OLED_WIDTH    128
#define OLED_HEIGHT   64

typedef enum {
    FACE_IDLE = 0,
    FACE_LISTENING,
    FACE_THINKING,
    FACE_SPEAKING,
    FACE_HAPPY,
    FACE_SAD,
    FACE_ERROR,
    FACE_SLEEP
} face_state_t;

#ifdef __cplusplus
extern "C" {
#endif

void oled_init(void);
void display_status(const char *status);
void face_render(void);
void display_render_buffer(const uint8_t *buffer);

// Basic Mochi renderer kept for compatibility.
// expr: 0 normal, 1 listening, 2 speaking/happy, 6 sad, 99 error.
// step: 0 open, 1 blink, 2 happy closed curve, 3 sleep.
// sX/sY: shift the complete pair of eyes.
// arahLirik: 0 center, 1 left, 2 right, 3 up.
void display_render_mochi(int expr, int step, int sX, int sY, int arahLirik);

// Extended renderer used by the animation engine. gaze_x/gaze_y move only
// the pupils, allowing smooth eye movement without moving the eye shapes.
// gaze_x: -7..7, gaze_y: -5..5.
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
