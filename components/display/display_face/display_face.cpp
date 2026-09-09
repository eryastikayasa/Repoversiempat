#include "display_face.h"

#include "display_driver.h"
#include <math.h>
#include <string.h>

namespace {

constexpr int OLED_WIDTH = 128;
constexpr int OLED_HEIGHT = 64;
static uint8_t s_face_buffer[OLED_WIDTH * OLED_HEIGHT / 8] = {0};
static face_state_t s_current_face_state = FACE_IDLE;

static void pixel(int x, int y, bool on = true)
{
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) return;
    uint8_t &b = s_face_buffer[x + (y >> 3) * OLED_WIDTH];
    uint8_t m = (uint8_t)(1U << (y & 7));
    if (on) b |= m;
    else b &= (uint8_t)~m;
}

static void fill_circle(int cx, int cy, int r, bool on = true)
{
    for (int y = -r; y <= r; ++y) {
        int q = r * r - y * y;
        int dx = q > 0 ? (int)sqrtf((float)q) : 0;
        for (int x = -dx; x <= dx; ++x) pixel(cx + x, cy + y, on);
    }
}

static void line(int x0, int y0, int x1, int y1)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        pixel(x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void draw_open_eye(int cx, int cy, int gaze_x, int gaze_y,
                          int eye_shift_x, int eye_shift_y)
{
    const int eye_r = 14;
    const int pupil_r = 7;

    if (gaze_x < -7) gaze_x = -7;
    if (gaze_x > 7) gaze_x = 7;
    if (gaze_y < -5) gaze_y = -5;
    if (gaze_y > 5) gaze_y = 5;

    const int ex = cx + eye_shift_x;
    const int ey = cy + eye_shift_y;
    const int px = ex + gaze_x;
    const int py = ey + gaze_y;

    fill_circle(ex, ey, eye_r, true);
    fill_circle(px, py, pupil_r, false);
    fill_circle(px - 2, py - 3, 3, true);
}

static void draw_blink_eye(int cx, int cy)
{
    line(cx - 11, cy, cx - 5, cy + 1);
    line(cx - 5, cy + 1, cx + 5, cy + 1);
    line(cx + 5, cy + 1, cx + 11, cy);
}

static void draw_happy_eye(int cx, int cy)
{
    for (int x = -12; x <= 12; ++x) {
        float t = (float)x / 12.0f;
        int y = (int)(7.0f * (1.0f - t * t));
        pixel(cx + x, cy + y, true);
        if ((x & 1) == 0) pixel(cx + x, cy + y + 1, true);
    }
}

static void draw_sad_eye(int cx, int cy)
{
    line(cx - 11, cy, cx - 5, cy + 2);
    line(cx - 5, cy + 2, cx + 5, cy + 2);
    line(cx + 5, cy + 2, cx + 11, cy);
}

static void draw_sleep_eye(int cx, int cy)
{
    line(cx - 12, cy, cx + 12, cy);
    line(cx - 9, cy + 1, cx + 9, cy + 1);
}

static void draw_error_eye(int cx, int cy)
{
    const int s = 10;
    line(cx - s, cy - s, cx + s, cy + s);
    line(cx + s, cy - s, cx - s, cy + s);
}

static void render_mochi_gaze(int expr, int step, int sX, int sY,
                              int gaze_x, int gaze_y,
                              int eye_shift_x, int eye_shift_y)
{
    memset(s_face_buffer, 0, sizeof(s_face_buffer));

    const int L = 34 + sX;
    const int R = 94 + sX;
    const int Y = 28 + sY;

    if (step == 3) {
        draw_sleep_eye(L + eye_shift_x, Y + eye_shift_y);
        draw_sleep_eye(R + eye_shift_x, Y + eye_shift_y);
    }
    else if (expr == 2 && step == 2) {
        draw_happy_eye(L + eye_shift_x, Y + eye_shift_y);
        draw_happy_eye(R + eye_shift_x, Y + eye_shift_y);
    }
    else if (expr == 2 || expr == 1 || expr == 0) {
        if (step == 1) {
            draw_blink_eye(L + eye_shift_x, Y + eye_shift_y);
            draw_blink_eye(R + eye_shift_x, Y + eye_shift_y);
        }
        else {
            draw_open_eye(L, Y, gaze_x, gaze_y, eye_shift_x, eye_shift_y);
            draw_open_eye(R, Y, gaze_x, gaze_y, eye_shift_x, eye_shift_y);
        }
    }
    else if (expr == 6) {
        draw_sad_eye(L + eye_shift_x, Y + eye_shift_y);
        draw_sad_eye(R + eye_shift_x, Y + eye_shift_y);
    }
    else if (expr == 99) {
        draw_error_eye(L + eye_shift_x, Y + eye_shift_y);
        draw_error_eye(R + eye_shift_x, Y + eye_shift_y);
    }
    else {
        draw_open_eye(L, Y, gaze_x, gaze_y, eye_shift_x, eye_shift_y);
        draw_open_eye(R, Y, gaze_x, gaze_y, eye_shift_x, eye_shift_y);
    }
}

}

void display_face_init(void)
{
    s_current_face_state = FACE_IDLE;
    memset(s_face_buffer, 0, sizeof(s_face_buffer));
}

void display_face_update(uint32_t now_ms)
{
    (void)now_ms;
    // Staging only. The existing face_animation/display.cpp runtime is untouched.
}

void display_face_set_state(face_state_t state)
{
    s_current_face_state = state;
}

face_state_t display_face_get_state(void)
{
    return s_current_face_state;
}

void display_face_render_mochi_gaze(int expr, int step, int sX, int sY,
                                    int gaze_x, int gaze_y,
                                    int eye_shift_x, int eye_shift_y)
{
    render_mochi_gaze(expr, step, sX, sY, gaze_x, gaze_y, eye_shift_x, eye_shift_y);
    // This function is intentionally not called by the current runtime.
    display_driver_present(s_face_buffer, OLED_WIDTH, OLED_HEIGHT);
}

void display_face_render_mochi(int expr, int step, int sX, int sY, int arahLirik)
{
    int gaze_x = 0;
    int gaze_y = 0;

    switch (arahLirik) {
        case 1: gaze_x = -5; break;
        case 2: gaze_x = 5; break;
        case 3: gaze_y = -5; break;
        default: break;
    }

    display_face_render_mochi_gaze(
        expr, step, sX, sY, gaze_x, gaze_y, 0, 0);
}

void display_face_render(void)
{
    int expr = 0;
    int step = 0;

    switch (s_current_face_state) {
        case FACE_LISTENING: expr = 1; break;
        case FACE_THINKING:  expr = 0; break;
        case FACE_SPEAKING:  expr = 2; break;
        case FACE_HAPPY:     expr = 2; step = 2; break;
        case FACE_SAD:       expr = 6; break;
        case FACE_ERROR:     expr = 99; break;
        case FACE_SLEEP:     expr = 0; step = 3; break;
        case FACE_IDLE:
        default:             expr = 0; break;
    }

    display_face_render_mochi_gaze(expr, step, 0, 0, 0, 0, 0, 0);
}

const uint8_t *display_face_buffer(void)
{
    return s_face_buffer;
}
