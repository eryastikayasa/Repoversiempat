#pragma once

#include "display.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Text owns the bottom text/RSSI framebuffer. OLED Engine composes it with Face.
void text_init(void);
void text_update(uint32_t now_ms);
void text_set_user(const char *text);
void text_set_gemini(const char *text);
void text_append_user(const char *text);
void text_append_gemini(const char *text);
void text_render_user(void);
void text_render_gemini(void);
const uint8_t *text_buffer(void);

#ifdef __cplusplus
}
#endif
