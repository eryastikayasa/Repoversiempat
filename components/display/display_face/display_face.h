#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void display_face_init(void);
void display_face_update(uint32_t now_ms);

#ifdef __cplusplus
}
#endif
