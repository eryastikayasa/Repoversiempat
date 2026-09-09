#include "face.h"

#include <math.h>
#include <string.h>

namespace {

constexpr int WIDTH = OLED_WIDTH;
constexpr int HEIGHT = OLED_HEIGHT;

static uint8_t s_face_buffer[WIDTH * HEIGHT / 8] = {0};
static face_state_t s_current_state = FACE_IDLE;

static void pixel(int x, int y, bool on = true)
{
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;

    uint8_t &b = s_face_buffer[x + (y >> 3) * WIDTH];
    const uint8_t mask = (uint8_t)(1U << (y & 7));

    if (on) b |= mask;
    else b &= (uint8_t)~mask;
}

static void fill_circle(int cx, int cy, int r, bool on = true)
{
    for (int y = -r; y <= r; ++y) {
        const int q = r * r - y * y;
        const int dx = q > 0 ? (int)sqrtf((float)q) : 0;

        for (int x = -dx; x <= dx; ++x)
            pixel(cx + x, cy + y, on);
    }
}

static void line(int x0, int y0, int x1, int y1)
{
    const int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        pixel(x0, y0);
        if (x0 == x1 && y0 == y1) break;

        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// Original Mochi open eye: large white eye, cut-out pupil and highlight.
static void draw_open_eye(int cx, int cy, int gaze_x, int gaze_y,
                          int eye_shift_x, int eye_shift_y)
{
    constexpr int EYE_R = 14;
    constexpr int PUPIL_R = 7;

    if (gaze_x < -7) gaze_x = -7;
    if (gaze_x > 7) gaze_x = 7;
    if (gaze_y < -5) gaze_y = -5;
    if (gaze_y > 5) gaze_y = 5;

    const int ex = cx + eye_shift_x;
    const int ey = cy + eye_shift_y;
    const int px = ex + gaze_x;
    const int py = ey + gaze_y;

    fill_circle(ex, ey, EYE_R, true);
    fill_circle(px, py, PUPIL_R, false);
    fill_circle(px - 2, py - 3, 3, true);
}

static void draw_blink_eye(int cx, int cy)
{
    line(cx - 11, cy,     cx - 5, cy + 1);
    line(cx - 5,  cy + 1, cx + 5, cy + 1);
    line(cx + 5,  cy + 1, cx + 11, cy);
}

static void draw_happy_eye(int cx, int cy)
{
    for (int x = -12; x <= 12; ++x) {
        const float t = (float)x / 12.0f;
        const int y = (int)(7.0f * (1.0f - t * t));

        pixel(cx + x, cy + y, true);
        if ((x & 1) == 0) pixel(cx + x, cy + y + 1, true);
    }
}

static void draw_sad_eye(int cx, int cy)
{
    line(cx - 11, cy,     cx - 5, cy + 2);
    line(cx - 5,  cy + 2, cx + 5, cy + 2);
    line(cx + 5,  cy + 2, cx + 11, cy);
}

static void draw_sleep_eye(int cx, int cy)
{
    line(cx - 12, cy,     cx + 12, cy);
    line(cx - 9,  cy + 1, cx + 9,  cy + 1);
}

static void draw_error_eye(int cx, int cy)
{
    constexpr int S = 10;
    line(cx - S, cy - S, cx + S, cy + S);
    line(cx + S, cy - S, cx - S, cy + S);
}

static void render_mochi_gaze(int expr, int step, int sX, int sY,
                              int gaze_x, int gaze_y,
                              int eye_shift_x, int eye_shift_y)
{
    memset(s_face_buffer, 0, sizeof(s_face_buffer));

    // Original design positions.
    const int left  = 34 + sX;
    const int right = 94 + sX;
    const int y     = 28 + sY;

    if (step == 3) {
        draw_sleep_eye(left + eye_shift_x,  y + eye_shift_y);
        draw_sleep_eye(right + eye_shift_x, y + eye_shift_y);
    }
    else if (expr == 2 && step == 2) {
        draw_happy_eye(left + eye_shift_x,  y + eye_shift_y);
        draw_happy_eye(right + eye_shift_x, y + eye_shift_y);
    }
    else if (expr == 2 || expr == 1 || expr == 0) {
        if (step == 1) {
            draw_blink_eye(left + eye_shift_x,  y + eye_shift_y);
            draw_blink_eye(right + eye_shift_x, y + eye_shift_y);
        }
        else {
            draw_open_eye(left,  y, gaze_x, gaze_y, eye_shift_x, eye_shift_y);
            draw_open_eye(right, y, gaze_x, gaze_y, eye_shift_x, eye_shift_y);
        }
    }
    else if (expr == 6) {
        draw_sad_eye(left + eye_shift_x,  y + eye_shift_y);
        draw_sad_eye(right + eye_shift_x, y + eye_shift_y);
    }
    else if (expr == 99) {
        draw_error_eye(left + eye_shift_x,  y + eye_shift_y);
        draw_error_eye(right + eye_shift_x, y + eye_shift_y);
    }
    else {
        draw_open_eye(left,  y, gaze_x, gaze_y, eye_shift_x, eye_shift_y);
        draw_open_eye(right, y, gaze_x, gaze_y, eye_shift_x, eye_shift_y);
    }
}

static void state_to_expression(face_state_t state, int &expr, int &step)
{
    expr = 0;
    step = 0;

    switch (state) {
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
}

} // namespace

void face_init(void)
{
    s_current_state = FACE_IDLE;
    memset(s_face_buffer, 0, sizeof(s_face_buffer));
}

void face_set_state(face_state_t state)
{
    s_current_state = state;
}

face_state_t face_get_state(void)
{
    return s_current_state;
}

void face_update(uint32_t now_ms)
{
    // Animation timing remains owned by Face. The current step is rendered
    // into the Face framebuffer; OLED Engine decides when/how to compose it.
    (void)now_ms;
    face_render();
}

void face_render_mochi_gaze(int expr, int step, int sX, int sY,
                            int gaze_x, int gaze_y,
                            int eye_shift_x, int eye_shift_y)
{
    render_mochi_gaze(expr, step, sX, sY,
                      gaze_x, gaze_y,
                      eye_shift_x, eye_shift_y);
}

void face_render_mochi(int expr, int step, int sX, int sY, int arahLirik)
{
    int gaze_x = 0;
    int gaze_y = 0;

    switch (arahLirik) {
        case 1: gaze_x = -5; break;
        case 2: gaze_x =  5; break;
        case 3: gaze_y = -5; break;
        default: break;
    }

    face_render_mochi_gaze(expr, step, sX, sY,
                           gaze_x, gaze_y, 0, 0);
}

void face_render(void)
{
    int expr = 0;
    int step = 0;
    state_to_expression(s_current_state, expr, step);

    face_render_mochi_gaze(expr, step,
                           0, 0,
                           0, 0,
                           0, 0);
}

const uint8_t *face_buffer(void)
{
    return s_face_buffer;
}
