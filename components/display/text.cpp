#include "text.h"

#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include <string.h>
#include <stdio.h>

namespace {

constexpr int WIDTH = OLED_WIDTH;
constexpr int HEIGHT = OLED_HEIGHT;

static uint8_t s_text_buffer[WIDTH * HEIGHT / 8] = {0};
static char s_user_text[256] = {0};
static char s_gemini_text[256] = {0};
static uint16_t s_user_offset = 0;
static uint16_t s_gemini_offset = 0;
static portMUX_TYPE s_text_mux = portMUX_INITIALIZER_UNLOCKED;

static uint8_t glyph_row(char c, int row)
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
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;
    uint8_t &b = s_text_buffer[x + (y >> 3) * WIDTH];
    const uint8_t mask = (uint8_t)(1U << (y & 7));
    if (on) b |= mask;
    else b &= (uint8_t)~mask;
}

static void draw_char(int x, int y, char c)
{
    for (int row = 0; row < 5; ++row) {
        const uint8_t bits = glyph_row(c, row);
        for (int col = 0; col < 5; ++col)
            if (bits & (1U << (4 - col))) pixel(x + col, y + row, true);
    }
}

static void sanitize(char *dst, size_t size, const char *src, uint16_t &offset)
{
    if (!dst || size == 0) return;
    dst[0] = '\0';
    offset = 0;
    if (!src) return;

    size_t out = 0;
    for (size_t i = 0; src[i] != '\0' && out + 1 < size; ++i) {
        const unsigned char c = (unsigned char)src[i];
        if (c >= 0x20 && c <= 0x7E) dst[out++] = (char)c;
    }
    dst[out] = '\0';
}

static void append(char *dst, size_t size, const char *src)
{
    if (!dst || size == 0 || !src || !src[0]) return;

    size_t out = strlen(dst);
    if (out > 0 && out + 1 < size) {
        dst[out++] = ' ';
        dst[out] = '\0';
    }

    for (size_t i = 0; src[i] != '\0' && out + 1 < size; ++i) {
        const unsigned char c = (unsigned char)src[i];
        if (c >= 0x20 && c <= 0x7E) dst[out++] = (char)c;
    }
    dst[out] = '\0';
}

static void draw_scrolling(const char *str, uint16_t &offset, int text_x)
{
    if (!str || !str[0]) return;

    constexpr int TEXT_Y = HEIGHT - 5;
    constexpr int CHAR_WIDTH = 6;
    constexpr int RIGHT = WIDTH - 1;
    constexpr int GAP = 12;

    const size_t len = strlen(str);
    const int text_px = (int)len * CHAR_WIDTH;
    const int visible = RIGHT - text_x + 1;
    if (visible <= 5) return;

    if (text_px <= visible) {
        for (size_t i = 0; i < len; ++i)
            draw_char(text_x + (int)i * CHAR_WIDTH, TEXT_Y, str[i]);
        offset = 0;
        return;
    }

    const int cycle = text_px + GAP;
    int pos = RIGHT + 1 - (int)offset;

    for (size_t i = 0; i < len; ++i) {
        const int x = pos + (int)i * CHAR_WIDTH;
        if (x + 5 >= text_x && x <= RIGHT) draw_char(x, TEXT_Y, str[i]);
    }

    pos += cycle;
    for (size_t i = 0; i < len; ++i) {
        const int x = pos + (int)i * CHAR_WIDTH;
        if (x + 5 >= text_x && x <= RIGHT) draw_char(x, TEXT_Y, str[i]);
    }

    offset = (uint16_t)((offset + 1) % cycle);
}

static void draw_rssi_char(int x, int y, char c)
{
    static const uint8_t glyphs[][5] = {
        {0x00,0x00,0x1F,0x00,0x00},
        {0x1E,0x11,0x11,0x11,0x1E}, {0x00,0x12,0x1F,0x10,0x00},
        {0x12,0x19,0x15,0x13,0x12}, {0x11,0x15,0x15,0x15,0x0A},
        {0x07,0x04,0x04,0x1F,0x04}, {0x17,0x15,0x15,0x15,0x09},
        {0x0E,0x15,0x15,0x15,0x08}, {0x01,0x01,0x19,0x05,0x03},
        {0x0A,0x15,0x15,0x15,0x0A}, {0x02,0x15,0x15,0x15,0x0E},
    };

    const int index = (c == '-') ? 0 : (c - '0' + 1);
    if (index < 0 || index >= (int)(sizeof(glyphs) / sizeof(glyphs[0]))) return;

    for (int col = 0; col < 5; ++col) {
        const uint8_t bits = glyphs[index][col];
        for (int row = 0; row < 5; ++row) {
            if (!(bits & (1U << row))) continue;
            const int px = x + col;
            const int py = y + row;
            if (px >= 0 && px < WIDTH && py >= 0 && py < HEIGHT)
                s_text_buffer[px + (py >> 3) * WIDTH] |= (uint8_t)(1U << (py & 7));
        }
    }
}

static int draw_rssi(void)
{
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;

    int rssi = ap.rssi;
    if (rssi > 0) rssi = 0;
    if (rssi < -99) rssi = -99;

    char value[5];
    snprintf(value, sizeof(value), "%d", rssi);

    int x = 0;
    for (size_t i = 0; value[i] != '\0'; ++i) {
        draw_rssi_char(x, HEIGHT - 5, value[i]);
        x += 6;
    }
    return x;
}

static void render(const char *str, uint16_t &offset)
{
    memset(s_text_buffer, 0, sizeof(s_text_buffer));
    const int rssi_end = draw_rssi();
    draw_scrolling(str, offset, rssi_end > 0 ? rssi_end + 4 : 4);
}

} // namespace

void text_init(void)
{
    memset(s_text_buffer, 0, sizeof(s_text_buffer));
}

void text_update(uint32_t now_ms)
{
    (void)now_ms;
}

void text_set_user(const char *text)
{
    portENTER_CRITICAL(&s_text_mux);
    sanitize(s_user_text, sizeof(s_user_text), text, s_user_offset);
    portEXIT_CRITICAL(&s_text_mux);
}

void text_set_gemini(const char *text)
{
    portENTER_CRITICAL(&s_text_mux);
    sanitize(s_gemini_text, sizeof(s_gemini_text), text, s_gemini_offset);
    portEXIT_CRITICAL(&s_text_mux);
}

void text_append_user(const char *text)
{
    portENTER_CRITICAL(&s_text_mux);
    append(s_user_text, sizeof(s_user_text), text);
    portEXIT_CRITICAL(&s_text_mux);
}

void text_append_gemini(const char *text)
{
    portENTER_CRITICAL(&s_text_mux);
    append(s_gemini_text, sizeof(s_gemini_text), text);
    portEXIT_CRITICAL(&s_text_mux);
}

void text_render_user(void)
{
    char copy[256] = {0};
    uint16_t offset = 0;

    portENTER_CRITICAL(&s_text_mux);
    strncpy(copy, s_user_text, sizeof(copy) - 1);
    offset = s_user_offset;
    portEXIT_CRITICAL(&s_text_mux);

    render(copy, offset);

    portENTER_CRITICAL(&s_text_mux);
    s_user_offset = offset;
    portEXIT_CRITICAL(&s_text_mux);
}

void text_render_gemini(void)
{
    char copy[256] = {0};
    uint16_t offset = 0;

    portENTER_CRITICAL(&s_text_mux);
    strncpy(copy, s_gemini_text, sizeof(copy) - 1);
    offset = s_gemini_offset;
    portEXIT_CRITICAL(&s_text_mux);

    render(copy, offset);

    portENTER_CRITICAL(&s_text_mux);
    s_gemini_offset = offset;
    portEXIT_CRITICAL(&s_text_mux);
}

const uint8_t *text_buffer(void)
{
    return s_text_buffer;
}
