#pragma once

#include "display.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Face owns the face framebuffer. OLED Engine composes it with Text.
void face_init(void);
void face_update(uint32_t now_ms);
void face_set_state(face_state_t state);
face_state_t face_get_state(void);

// Render one Face frame into the Face-owned framebuffer.
// expr: 0 normal, 1 listening, 2 speaking/happy, 6 sad, 99 error.
// step: 0 open, 1 blink, 2 happy curve, 3 sleep.
void face_render_mochi_gaze(int expr, int step, int sX, int sY,
                            int gaze_x, int gaze_y,
                            int eye_shift_x, int eye_shift_y);
void face_render_mochi(int expr, int step, int sX, int sY, int arahLirik);
void face_render(void);

// Read-only framebuffer for OLED Engine composition.
const uint8_t *face_buffer(void);

#ifdef __cplusplus
}
#endif
