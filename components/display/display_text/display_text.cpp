#include "display_text.h"

#include "freertos/FreeRTOS.h"
#include "esp_attr.h"
#include <string.h>

namespace {

constexpr int OLED_WIDTH = 128;
constexpr int OLED_HEIGHT = 64;
constexpr uint32_t TEXT_TYPE_INTERVAL_MS = 55;
constexpr uint32_t TEXT_STATUS_SCROLL_STEP_MS = 33;
constexpr size_t TRANSCRIPT_TEXT_CAP = 512;

static EXT_RAM_BSS_ATTR uint8_t s_text_buffer[OLED_WIDTH * OLED_HEIGHT / 8] = {0};
static char s_user_target[TRANSCRIPT_TEXT_CAP] = {0};
static char s_gemini_target[TRANSCRIPT_TEXT_CAP] = {0};
static char s_status_text[64] = {0};
static char s_user_render_copy[TRANSCRIPT_TEXT_CAP] = {0};
static char s_gemini_render_copy[TRANSCRIPT_TEXT_CAP] = {0};
static uint16_t s_user_visible_len = 0;
static uint16_t s_gemini_visible_len = 0;
static uint16_t s_status_scroll_offset = 0;
static uint32_t s_last_type_ms = 0;
static uint32_t s_last_status_update_ms = 0;
static portMUX_TYPE s_text_mux = portMUX_INITIALIZER_UNLOCKED;

static uint8_t glyph_row(char c, int row)
{
    static const uint8_t letters[26][5] = {
        {14,17,31,17,17},{30,17,30,17,30},{15,16,16,16,15},{30,17,17,17,30},
        {31,16,30,16,31},{31,16,30,16,16},{15,16,23,17,15},{17,17,31,17,17},
        {31,4,4,4,31},{7,2,2,18,12},{17,18,28,18,17},{16,16,16,16,31},
        {17,27,21,17,17},{17,25,21,19,17},{14,17,17,17,14},{30,17,30,16,16},
        {14,17,21,19,15},{30,17,30,18,17},{15,16,14,1,30},{31,4,4,4,4},
        {17,17,17,17,14},{17,17,17,10,4},{17,17,21,27,17},{17,10,4,10,17},
        {17,10,4,4,4},{31,2,4,8,31}
    };
    static const uint8_t digits[10][5] = {
        {14,17,19,21,14},{4,12,4,4,14},{14,1,6,8,31},{30,1,6,1,30},{18,18,31,2,2},
        {31,16,30,1,30},{14,16,30,17,14},{31,1,2,4,4},{14,17,14,17,14},{14,17,15,1,14}
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
        default: return 0;
    }
}

static void pixel(int x, int y)
{
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) return;
    s_text_buffer[x + (y >> 3) * OLED_WIDTH] |= (uint8_t)(1U << (y & 7));
}

static void draw_char(int x, int y, char c)
{
    for (int row = 0; row < 5; ++row) {
        const uint8_t bits = glyph_row(c, row);
        for (int col = 0; col < 5; ++col)
            if (bits & (1U << (4 - col))) pixel(x + col, y + row);
    }
}

static bool printable(unsigned char c) { return c >= 0x20 && c <= 0x7E; }

// Repo6 behavior: replace the target while preserving the typed common prefix.
static void set_target(char *dst, size_t dst_size, const char *text, uint16_t &visible_len)
{
    const size_t old_len = strlen(dst);
    size_t new_len = 0;
    size_t common = 0;
    bool common_active = true;

    if (text) {
        for (size_t i = 0; text[i] != '\0'; ++i) {
            const unsigned char c = (unsigned char)text[i];
            if (!printable(c)) continue;
            if (new_len + 1U >= dst_size) break;
            if (common_active && common < old_len && dst[common] == (char)c) ++common;
            else common_active = false;
            ++new_len;
        }
    }

    if (old_len == new_len && common == old_len) return;

    size_t current = visible_len;
    if (current > old_len) current = old_len;
    if (current > common) current = common;

    size_t out = 0;
    if (text) {
        for (size_t i = 0; text[i] != '\0' && out + 1U < dst_size; ++i) {
            const unsigned char c = (unsigned char)text[i];
            if (printable(c)) dst[out++] = (char)c;
        }
    }
    dst[out] = '\0';
    visible_len = (uint16_t)current;
}

static uint16_t advance_status_offset(const char *text, uint16_t offset, uint32_t steps)
{
    if (!text || !text[0]) return 0;
    constexpr uint16_t CHAR_WIDTH = 6;
    constexpr uint16_t GAP_PX = 12;
    const uint32_t text_px = (uint32_t)strlen(text) * CHAR_WIDTH;
    const uint32_t visible_width = OLED_WIDTH - 4;
    if (text_px <= visible_width) return 0;
    const uint32_t cycle_px = text_px + GAP_PX;
    return (uint16_t)((offset + steps) % cycle_px);
}

static void draw_window(const char *text, uint16_t visible_len, int text_x)
{
    if (!text || visible_len == 0) return;
    constexpr int TEXT_Y = OLED_HEIGHT - 5;
    constexpr int CHAR_WIDTH = 6;
    const int visible_width = OLED_WIDTH - text_x;
    const int max_chars = visible_width / CHAR_WIDTH;
    const size_t len = strlen(text);
    const size_t count = visible_len < len ? visible_len : len;
    if (count == 0 || max_chars <= 0) return;
    const size_t first = ((int)count > max_chars) ? count - (size_t)max_chars : 0;
    for (size_t i = first; i < count; ++i)
        draw_char(text_x + (int)(i - first) * CHAR_WIDTH, TEXT_Y, text[i]);
}

static void copy_target(const char *src, char *dst, uint16_t &visible)
{
    portENTER_CRITICAL(&s_text_mux);
    memcpy(dst, src, TRANSCRIPT_TEXT_CAP);
    visible = (src == s_user_target) ? s_user_visible_len : s_gemini_visible_len;
    portEXIT_CRITICAL(&s_text_mux);
    dst[TRANSCRIPT_TEXT_CAP - 1] = '\0';
}

} // namespace

void display_text_init(void)
{
    portENTER_CRITICAL(&s_text_mux);
    memset(s_text_buffer, 0, sizeof(s_text_buffer));
    s_user_target[0] = '\0';
    s_gemini_target[0] = '\0';
    s_status_text[0] = '\0';
    s_user_render_copy[0] = '\0';
    s_gemini_render_copy[0] = '\0';
    s_user_visible_len = 0;
    s_gemini_visible_len = 0;
    s_status_scroll_offset = 0;
    s_last_type_ms = 0;
    s_last_status_update_ms = 0;
    portEXIT_CRITICAL(&s_text_mux);
}

void display_text_update(uint32_t now_ms)
{
    portENTER_CRITICAL(&s_text_mux);
    if (s_last_type_ms == 0) s_last_type_ms = now_ms;
    if ((uint32_t)(now_ms - s_last_type_ms) >= TEXT_TYPE_INTERVAL_MS) {
        const size_t user_len = strlen(s_user_target);
        const size_t gemini_len = strlen(s_gemini_target);
        if (s_user_visible_len < user_len) ++s_user_visible_len;
        if (s_gemini_visible_len < gemini_len) ++s_gemini_visible_len;
        s_last_type_ms = now_ms;
    }
    if (s_last_status_update_ms == 0) s_last_status_update_ms = now_ms;
    const uint32_t steps = (now_ms - s_last_status_update_ms) / TEXT_STATUS_SCROLL_STEP_MS;
    if (steps) {
        s_status_scroll_offset = advance_status_offset(s_status_text, s_status_scroll_offset, steps);
        s_last_status_update_ms += steps * TEXT_STATUS_SCROLL_STEP_MS;
    }
    portEXIT_CRITICAL(&s_text_mux);
}

void display_text_set_user(const char *text)
{
    portENTER_CRITICAL(&s_text_mux);
    set_target(s_user_target, sizeof(s_user_target), text, s_user_visible_len);
    portEXIT_CRITICAL(&s_text_mux);
}

void display_text_set_gemini(const char *text)
{
    portENTER_CRITICAL(&s_text_mux);
    set_target(s_gemini_target, sizeof(s_gemini_target), text, s_gemini_visible_len);
    portEXIT_CRITICAL(&s_text_mux);
}

void display_text_append_user(const char *text)
{
    portENTER_CRITICAL(&s_text_mux);
    size_t out = strlen(s_user_target);
    if (text && text[0] && out + 1U < sizeof(s_user_target)) {
        if (out && s_user_target[out - 1] != ' ') s_user_target[out++] = ' ';
        for (size_t i = 0; text[i] && out + 1U < sizeof(s_user_target); ++i)
            if (printable((unsigned char)text[i])) s_user_target[out++] = text[i];
        s_user_target[out] = '\0';
    }
    portEXIT_CRITICAL(&s_text_mux);
}

void display_text_append_gemini(const char *text)
{
    portENTER_CRITICAL(&s_text_mux);
    size_t out = strlen(s_gemini_target);
    if (text && text[0] && out + 1U < sizeof(s_gemini_target)) {
        if (out && s_gemini_target[out - 1] != ' ') s_gemini_target[out++] = ' ';
        for (size_t i = 0; text[i] && out + 1U < sizeof(s_gemini_target); ++i)
            if (printable((unsigned char)text[i])) s_gemini_target[out++] = text[i];
        s_gemini_target[out] = '\0';
    }
    portEXIT_CRITICAL(&s_text_mux);
}

void display_text_set_status(const char *text)
{
    portENTER_CRITICAL(&s_text_mux);
    size_t out = 0;
    if (text) for (size_t i = 0; text[i] && out + 1U < sizeof(s_status_text); ++i)
        if (printable((unsigned char)text[i])) s_status_text[out++] = text[i];
    s_status_text[out] = '\0';
    s_status_scroll_offset = 0;
    portEXIT_CRITICAL(&s_text_mux);
}

bool display_text_has_user(void)
{
    bool result;
    portENTER_CRITICAL(&s_text_mux); result = s_user_target[0] != '\0'; portEXIT_CRITICAL(&s_text_mux);
    return result;
}

bool display_text_has_gemini(void)
{
    bool result;
    portENTER_CRITICAL(&s_text_mux); result = s_gemini_target[0] != '\0'; portEXIT_CRITICAL(&s_text_mux);
    return result;
}

void display_text_render_user(void)
{
    uint16_t visible = 0;
    copy_target(s_user_target, s_user_render_copy, visible);
    memset(s_text_buffer, 0, sizeof(s_text_buffer));
    draw_window(s_user_render_copy, visible, 4);
}

void display_text_render_gemini(void)
{
    uint16_t visible = 0;
    copy_target(s_gemini_target, s_gemini_render_copy, visible);
    memset(s_text_buffer, 0, sizeof(s_text_buffer));
    draw_window(s_gemini_render_copy, visible, 4);
}

void display_text_render_status(void)
{
    char text[64] = {0};
    uint16_t offset = 0;
    portENTER_CRITICAL(&s_text_mux);
    memcpy(text, s_status_text, sizeof(text));
    offset = s_status_scroll_offset;
    portEXIT_CRITICAL(&s_text_mux);
    text[sizeof(text) - 1] = '\0';

    memset(s_text_buffer, 0, sizeof(s_text_buffer));
    constexpr int CHAR_WIDTH = 6;
    constexpr int TEXT_Y = OLED_HEIGHT - 5;
    const size_t len = strlen(text);
    if (!len) return;
    if ((int)len <= OLED_WIDTH / CHAR_WIDTH) {
        const int x = (OLED_WIDTH - (int)len * CHAR_WIDTH) / 2;
        for (size_t i = 0; i < len; ++i) draw_char(x + (int)i * CHAR_WIDTH, TEXT_Y, text[i]);
        return;
    }
    const int x = OLED_WIDTH - (int)offset;
    for (size_t i = 0; i < len; ++i) {
        const int px = x + (int)i * CHAR_WIDTH;
        if (px + 5 >= 0 && px < OLED_WIDTH) draw_char(px, TEXT_Y, text[i]);
    }
}

const uint8_t *display_text_buffer(void) { return s_text_buffer; }
