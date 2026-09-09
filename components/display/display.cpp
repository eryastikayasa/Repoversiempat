#include "display.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include <string.h>
#include <math.h>
#include "esp_attr.h"
#include <stdio.h>

static const char *TAG = "DISPLAY";
static bool oled_ready = false;
static face_state_t current_face_state = FACE_IDLE;
static EXT_RAM_BSS_ATTR uint8_t face_buffer[OLED_WIDTH * OLED_HEIGHT / 8];

// Full legacy implementation is preserved verbatim in display_legacy.cpp.
// This stub intentionally prevents accidental duplicate hardware ownership during migration.
