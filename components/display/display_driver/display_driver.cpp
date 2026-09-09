#include "display_driver.h"

void display_driver_init(void) {
    // Structure only. Existing OLED driver is untouched.
}

void display_driver_present(const uint8_t *buffer, int width, int height) {
    (void)buffer;
    (void)width;
    (void)height;
    // Structure only. Hardware migration will be done later.
}
