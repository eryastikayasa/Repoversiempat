#pragma once

#include "display.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DISPLAY_FACE_WIDTH OLED_WIDTH
#define DISPLAY_FACE_HEIGHT OLED_HEIGHT
#define DISPLAY_FACE_BUFFER_SIZE (DISPLAY_FACE_WIDTH * DISPLAY_FACE_HEIGHT / 8)

// Framebuffer producer only. Never touches SSD1306/I2C.
void display_face_init(void);
void display_face_update(uint32_t now_ms);
void display_face_set_state(face_state_t state);
void display_face_show_for_ms(face_state_t state, uint32_t duration_ms);
face_state_t display_face_get_state(void);
void display_face_render_mochi_gaze(int expr, int step, int sX, int sY,
                                    int gaze_x, int gaze_y,
                                    int eye_shift_x, int eye_shift_y);
void display_face_render_mochi(int expr, int step, int sX, int sY, int arahLirik);
void display_face_render(void);
const uint8_t *display_face_buffer(void);

#ifdef __cplusplus
}
#endif
