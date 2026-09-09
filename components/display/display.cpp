#include "display.h"
#include "display_driver.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "esp_wifi.h"
#include <string.h>
#include <math.h>
#include "esp_attr.h"
#include <stdio.h>

static const char *TAG = "DISPLAY";
static bool oled_ready = false;
static face_state_t current_face_state = FACE_IDLE;
static EXT_RAM_BSS_ATTR uint8_t face_buffer[OLED_WIDTH * OLED_HEIGHT / 8];

static char user_scroll_text[256] = {0};
static char gemini_scroll_text[256] = {0};
static uint16_t user_scroll_offset = 0;
static uint16_t gemini_scroll_offset = 0;
static portMUX_TYPE scroll_text_mux = portMUX_INITIALIZER_UNLOCKED;

void oled_init(void)
{
    if (oled_ready) return;

    display_driver_init();
    oled_ready = display_driver_is_ready();
    if (!oled_ready) return;

    memset(face_buffer, 0, sizeof(face_buffer));
    display_render_buffer(face_buffer);

    ESP_LOGI(TAG,
             "OLED via display_driver siap: SDA=%d SCL=%d ADDR=0x%02X SPEED=%dHz MIRROR=XY",
             DISPLAY_DRIVER_SDA_PIN,
             DISPLAY_DRIVER_SCL_PIN,
             DISPLAY_DRIVER_I2C_ADDR,
             DISPLAY_DRIVER_I2C_HZ);
}

void display_render_buffer(const uint8_t *buffer)
{
    if (!buffer) return;
    if (!oled_ready) oled_init();
    if (!oled_ready) return;

    display_driver_present(buffer, OLED_WIDTH, OLED_HEIGHT);
}

void display_status(const char *text)
{
    if (!oled_ready) oled_init();
    ESP_LOGI(TAG, "[OLED STATUS]: %s", text ? text : "(null)");
}

static void draw_rssi_char(int x, int y, char c)
{
    static const uint8_t glyphs[][5] = {
        {0x00,0x00,0x1F,0x00,0x00},
        {0x1E,0x11,0x11,0x11,0x1E},
        {0x00,0x12,0x1F,0x10,0x00},
        {0x12,0x19,0x15,0x13,0x12},
        {0x11,0x15,0x15,0x15,0x0A},
        {0x07,0x04,0x04,0x1F,0x04},
        {0x17,0x15,0x15,0x15,0x09},
        {0x0E,0x15,0x15,0x15,0x08},
        {0x01,0x01,0x19,0x05,0x03},
        {0x0A,0x15,0x15,0x15,0x0A},
        {0x02,0x15,0x15,0x15,0x0E},
    };

    int index = (c == '-') ? 0 : (c - '0' + 1);
    if (index < 0 || index >= (int)(sizeof(glyphs) / sizeof(glyphs[0]))) return;

    for (int col = 0; col < 5; ++col) {
        uint8_t bits = glyphs[index][col];
        for (int row = 0; row < 5; ++row) {
            if (bits & (1U << row)) {
                int px = x + col;
                int py = y + row;
                if (px >= 0 && px < OLED_WIDTH && py >= 0 && py < OLED_HEIGHT) {
                    uint8_t &b = face_buffer[px + (py >> 3) * OLED_WIDTH];
                    b |= (uint8_t)(1U << (py & 7));
                }
            }
        }
    }
}

static int draw_rssi(void)
{
    wifi_ap_record_t ap_info = {};
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) return 0;

    int rssi = ap_info.rssi;
    if (rssi > 0) rssi = 0;
    if (rssi < -99) rssi = -99;

    char text[5];
    snprintf(text, sizeof(text), "%d", rssi);

    const int char_width = 6;
    int x = 0;
    const int y = OLED_HEIGHT - 5;

    for (size_t i = 0; text[i] != '\0'; ++i) {
        draw_rssi_char(x, y, text[i]);
        x += char_width;
    }

    return x;
}

static void set_scroll_text(char *dst, size_t dst_size, const char *text, uint16_t &offset)
{
    if (!dst || dst_size == 0) return;

    dst[0] = '\0';
    offset = 0;
    if (!text) return;

    size_t out = 0;
    for (size_t i = 0; text[i] != '\0' && out + 1 < dst_size; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c >= 0x20 && c <= 0x7E)
            dst[out++] = (char)c;
    }
    dst[out] = '\0';
}

static void append_scroll_text(char *dst, size_t dst_size, const char *text)
{
    if (!dst || dst_size == 0 || !text || !text[0]) return;

    size_t out = strlen(dst);
    if (out > 0 && out + 1 < dst_size) {
        dst[out++] = ' ';
        dst[out] = '\0';
    }

    for (size_t i = 0; text[i] != '\0' && out + 1 < dst_size; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c >= 0x20 && c <= 0x7E)
            dst[out++] = (char)c;
    }
    dst[out] = '\0';
}

void display_set_user_text(const char *text)
{
    portENTER_CRITICAL(&scroll_text_mux);
    set_scroll_text(user_scroll_text, sizeof(user_scroll_text), text, user_scroll_offset);
    portEXIT_CRITICAL(&scroll_text_mux);
}

void display_set_gemini_text(const char *text)
{
    portENTER_CRITICAL(&scroll_text_mux);
    set_scroll_text(gemini_scroll_text, sizeof(gemini_scroll_text), text, gemini_scroll_offset);
    portEXIT_CRITICAL(&scroll_text_mux);
}

void display_append_user_text(const char *text)
{
    portENTER_CRITICAL(&scroll_text_mux);
    append_scroll_text(user_scroll_text, sizeof(user_scroll_text), text);
    portEXIT_CRITICAL(&scroll_text_mux);
}

void display_append_gemini_text(const char *text)
{
    portENTER_CRITICAL(&scroll_text_mux);
    append_scroll_text(gemini_scroll_text, sizeof(gemini_scroll_text), text);
    portEXIT_CRITICAL(&scroll_text_mux);
}

static uint8_t text_glyph_row(char c, int row)
{
    static const uint8_t letters[26][5] = {
        {14,17,31,17,17}, {30,17,30,17,30}, {15,16,16,16,15},
        {30,17,17,17,30}, {31,16,30,16,31}, {31,16,30,16,16},
        {15,16,23,17,15}, {17,17,31,17,17}, {31,4,4,4,31},
        {7,2,2,18,12}, {17,18,28,18,17}, {16,16,16,16,31},
        {17,27,21,17,17}, {17,25,21,19,17}, {14,17,17,17,14},
        {30,17,30,16,16}, {14,17,21,19,15}, {30,17,30,18,17},
        {15,16,14,1,30}, {31,4,4,4,4}, {17,17,17,17,14},
        {17,17,17,10,4}, {17,17,21,27,17}, {17,10,4,10,17},
        {17,10,4,4,4}, {31,2,4,8,31}
    };
    static const uint8_t digits[10][5] = {
        {14,17,19,21,14}, {4,12,4,4,14}, {14,1,6,8,31},
        {30,1,6,1,30}, {18,18,31,2,2}, {31,16,30,1,30},
        {14,16,30,17,14}, {31,1,2,4,4}, {14,17,14,17,14},
        {14,17,15,1,14}
    };

    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return letters[c - 'A'][row];
    if (c >= '0' && c <= '9') return digits[c - '0'][row];

    switch (c) {
        case '-': return row == 2 ? 14 : 0;
        case '.': return row == 4 ? 4 : 0;
        case ',': return row == 4 ? 6 : 0;
        case '!': return row < 4 ? 4 : 0;
        case ':': return (row == 1 || row == 3) ? 4 : 0;
        case '?': return row == 0 ? 14 : row == 1 ? 1 : row == 2 ? 6 : row == 4 ? 4 : 0;
        case '/': return (uint8_t)(1U << (4 - row));
        case ' ': return 0;
        default: return 0;
    }
}

static void pixel(int x, int y, bool on = true)
{
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) return;
    uint8_t &b = face_buffer[x + (y >> 3) * OLED_WIDTH];
    uint8_t m = (uint8_t)(1U << (y & 7));
    if (on) b |= m;
    else b &= (uint8_t)~m;
}

static void draw_text_char(int x, int y, char c)
{
    for (int row = 0; row < 5; ++row) {
        uint8_t bits = text_glyph_row(c, row);
        for (int col = 0; col < 5; ++col) {
            if (bits & (1U << (4 - col))) pixel(x + col, y + row, true);
        }
    }
}

static void draw_scrolling_text(const char *text, uint16_t &offset, int text_x)
{
    if (!text || !text[0]) return;

    constexpr int TEXT_Y = OLED_HEIGHT - 5;
    constexpr int CHAR_WIDTH = 6;
    constexpr int TEXT_RIGHT = OLED_WIDTH - 1;
    constexpr int GAP_PX = 12;

    const size_t len = strlen(text);
    const int text_px = (int)len * CHAR_WIDTH;
    const int visible_width = TEXT_RIGHT - text_x + 1;

    if (visible_width <= 5) return;

    if (text_px <= visible_width) {
        for (size_t i = 0; i < len; ++i) {
            draw_text_char(text_x + (int)i * CHAR_WIDTH, TEXT_Y, text[i]);
        }
        offset = 0;
        return;
    }

    const int cycle_px = text_px + visible_width + GAP_PX;
    int pos = (int)(offset % cycle_px);
    int start_x = TEXT_RIGHT - pos + 1;

    for (size_t i = 0; i < len; ++i) {
        int x = start_x + (int)i * CHAR_WIDTH;
        if (x < TEXT_RIGHT && x + 5 >= text_x) {
            draw_text_char(x, TEXT_Y, text[i]);
        }
    }

    offset = (uint16_t)((pos + 1) % cycle_px);
}

static void fill_circle(int cx, int cy, int r)
{
    for (int y = -r; y <= r; ++y) {
        for (int x = -r; x <= r; ++x) {
            if (x * x + y * y <= r * r) pixel(cx + x, cy + y, true);
        }
    }
}

static void line(int x0, int y0, int x1, int y1)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true) {
        pixel(x0, y0, true);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void face_set_state(face_state_t state)
{
    current_face_state = state;
}

face_state_t face_get_state(void)
{
    return current_face_state;
}

void display_render_mochi_gaze(int expr, int gaze_x, int gaze_y, int rssi)
{
    memset(face_buffer, 0, sizeof(face_buffer));

    int cx = OLED_WIDTH / 2;
    int cy = 30;
    int eye_y = 27;
    int eye_dx = 28;
    int eye_r = 12;

    (void)rssi;
    (void)expr;
    (void)gaze_x;
    (void)gaze_y;

    fill_circle(cx - eye_dx, eye_y, eye_r);
    fill_circle(cx + eye_dx, eye_y, eye_r);

    draw_rssi();

    portENTER_CRITICAL(&scroll_text_mux);
    draw_scrolling_text(user_scroll_text, user_scroll_offset, 0);
    portEXIT_CRITICAL(&scroll_text_mux);

    display_render_buffer(face_buffer);
}

void display_render_mochi(int expr, int rssi)
{
    display_render_mochi_gaze(expr, 0, 0, rssi);
}

void face_render(void)
{
    int rssi = 0;
    wifi_ap_record_t ap_info = {};
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi;
    }

    switch (current_face_state) {
        case FACE_IDLE:
            display_render_mochi_gaze(0, 0, 0, rssi);
            break;
        case FACE_LISTENING:
            display_render_mochi_gaze(1, -3, 0, rssi);
            break;
        case FACE_THINKING:
            display_render_mochi_gaze(2, 3, 0, rssi);
            break;
        case FACE_SPEAKING:
            display_render_mochi_gaze(3, 0, -2, rssi);
            break;
        case FACE_ERROR:
            display_render_mochi_gaze(4, 0, 0, rssi);
            break;
        default:
            display_render_mochi_gaze(0, 0, 0, rssi);
            break;
    }
}
