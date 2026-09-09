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
// Exact legacy implementation is preserved in display_legacy.cpp.
// Do not use this temporary file for build; migration will replace it from the preserved blob.
