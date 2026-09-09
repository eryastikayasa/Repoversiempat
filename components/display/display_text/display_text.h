#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void display_text_init(void);
void display_text_update(uint32_t now_ms);

#ifdef __cplusplus
}
#endif
