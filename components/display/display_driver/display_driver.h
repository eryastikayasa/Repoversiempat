#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void display_driver_init(void);
void display_driver_present(const uint8_t *buffer, int width, int height);

#ifdef __cplusplus
}
#endif
