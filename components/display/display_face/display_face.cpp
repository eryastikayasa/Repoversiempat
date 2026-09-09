#include "display_face.h"

#include <math.h>
#include <string.h>

namespace {

static uint8_t s_face_buffer[DISPLAY_FACE_BUFFER_SIZE] = {0};
static face_state_t s_current_face_state = FACE_IDLE;

// -----------------------------------------------------------------------------
// Low-level framebuffer drawing
// -----------------------------------------------------------------------------

static void pixel(int x, int y, bool on = true)
{
    if (x < 0 || x >= DISPLAY_FACE_WIDTH || y < 0 || y >= DISPLAY_FACE_HEIGHT) {
        return;
    }

    uint8_t &byte = s_face_buffer[x + (y >> 3) * DISPLAY_FACE_WIDTH];
    const uint8_t mask = (uint8_t)(1U << (y & 7));

    if (on) {
        byte |= mask;
    } else {
        byte &= (uint8_t)~mask;
    }
}

static void fill_circle(int cx, int cy, int radius)
{
    for (int y = -radius; y <= radius; ++y) {
        const int q = radius * radius - y * y;
        const int dx = q > 0 ? (int)sqrtf((float)q) : 0;

        for (int x = -dx; x <= dx; ++x) {
            pixel(cx + x, cy + y);
        }
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

        if (x0 == x1 && y0 == y1) {
            break;
        }

        const int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

// -----------------------------------------------------------------------------
// Eye shapes
// -----------------------------------------------------------------------------

static void draw_open_eye(int cx, int cy, int gaze_x, int gaze_y,
                          int eye_shift_x, int eye_shift_y)
{
    constexpr int EYE_RADIUS = 14;
    constexpr int PUPIL_RADIUS = 7;

    if (gaze_x < -7) gaze_x = -7;
    if (gaze_x > 7)  gaze_x = 7;
    if (gaze_y < -5) gaze_y = -5;
    if (gaze_y > 5)  gaze_y = 5;

    const int eye_x = cx + eye_shift_x;
    const int eye_y = cy + eye_shift_y;
    const int pupil_x = eye_x + gaze_x;
    const int pupil_y = eye_y + gaze_y;

    // White eye.
    fill_circle(eye_x, eye_y, EYE_RADIUS);

    // Clear the pupil area to create the black pupil.
    for (int y = -PUPIL_RADIUS; y <= PUPIL_RADIUS; ++y) {
        const int q = PUPIL_RADIUS * PUPIL_RADIUS - y * y;
        const int dx = q > 0 ? (int)sqrtf((float)q) : 0;

        for (int x = -dx; x <= dx; ++x) {
            pixel(pupil_x + x, pupil_y + y, false);
        }
    }

    // Small white highlight.
    fill_circle(pupil_x - 2, pupil_y - 3, 3);
}

static void draw_blink_eye(int cx, int cy)
{
    line(cx - 11, cy,     cx - 5, cy + 1);
    line(cx - 5,  cy + 1, cx + 5,  cy + 1);
    line(cx + 5,  cy + 1, cx + 11, cy);
}

static void draw_happy_eye(int cx, int cy)
{
    for (int x = -12; x <= 12; ++x) {
        const float t = (float)x / 12.0f;
        const int y = (int)(7.0f * (1.0f - t * t));

        pixel(cx + x, cy + y);
        if ((x & 1) == 0) {
            pixel(cx + x, cy + y + 1);
        }
    }
}

static void draw_sad_eye(int cx, int cy)
{
    line(cx - 11, cy,     cx - 5, cy + 2);
    line(cx - 5,  cy + 2, cx + 5,  cy + 2);
    line(cx + 5,  cy + 2, cx + 11, cy);
}

static void draw_sleep_eye(int cx, int cy)
{
    line(cx - 12, cy,     cx + 12, cy);
    line(cx - 9,  cy + 1, cx + 9,  cy + 1);
}

static void draw_error_eye(int cx, int cy)
{
    constexpr int SIZE = 10;

    line(cx - SIZE, cy - SIZE, cx + SIZE, cy + SIZE);
    line(cx + SIZE, cy - SIZE, cx - SIZE, cy + SIZE);
}

// -----------------------------------------------------------------------------
// Common Mochi renderer
// -----------------------------------------------------------------------------

static void render_mochi_gaze(int expr, int step,
                              int sX, int sY,
                              int gaze_x, int gaze_y,
                              int eye_shift_x, int eye_shift_y)
{
    memset(s_face_buffer, 0, sizeof(s_face_buffer));

    const int left_x = 34 + sX;
    const int right_x = 94 + sX;
    const int eye_y = 28 + sY;

    if (step == 3) {
        draw_sleep_eye(left_x + eye_shift_x, eye_y + eye_shift_y);
        draw_sleep_eye(right_x + eye_shift_x, eye_y + eye_shift_y);
        return;
    }

    if (expr == 2 && step == 2) {
        draw_happy_eye(left_x + eye_shift_x, eye_y + eye_shift_y);
        draw_happy_eye(right_x + eye_shift_x, eye_y + eye_shift_y);
        return;
    }

    if (expr == 6) {
        draw_sad_eye(left_x + eye_shift_x, eye_y + eye_shift_y);
        draw_sad_eye(right_x + eye_shift_x, eye_y + eye_shift_y);
        return;
    }

    if (expr == 99) {
        draw_error_eye(left_x + eye_shift_x, eye_y + eye_shift_y);
        draw_error_eye(right_x + eye_shift_x, eye_y + eye_shift_y);
        return;
    }

    if (step == 1) {
        draw_blink_eye(left_x + eye_shift_x, eye_y + eye_shift_y);
        draw_blink_eye(right_x + eye_shift_x, eye_y + eye_shift_y);
        return;
    }

    draw_open_eye(left_x, eye_y, gaze_x, gaze_y, eye_shift_x, eye_shift_y);
    draw_open_eye(right_x, eye_y, gaze_x, gaze_y, eye_shift_x, eye_shift_y);
}

// -----------------------------------------------------------------------------
// State-specific face definitions
// Each function only defines the visual state. No hardware access is allowed.
// -----------------------------------------------------------------------------

static void render_face_idle()
{
    render_mochi_gaze(0, 0, 0, 0, 0, 0, 0, 0);
}

static void render_face_listening()
{
    render_mochi_gaze(1, 0, 0, 0, 0, 0, 0, 0);
}

static void render_face_thinking()
{
    render_mochi_gaze(0, 0, 0, 0, 0, -5, 0, 0);
}

static void render_face_speaking()
{
    render_mochi_gaze(2, 0, 0, 0, 0, 0, 0, 0);
}

static void render_face_happy()
{
    render_mochi_gaze(2, 2, 0, 0, 0, 0, 0, 0);
}

static void render_face_sad()
{
    render_mochi_gaze(6, 0, 0, 0, 0, 0, 0, 0);
}

static void render_face_error()
{
    render_mochi_gaze(99, 0, 0, 0, 0, 0, 0, 0);
}

static void render_face_sleep()
{
    render_mochi_gaze(0, 3, 0, 0, 0, 0, 0, 0);
}

static void render_current_face()
{
    switch (s_current_face_state) {
        case FACE_IDLE:      render_face_idle();      break;
        case FACE_LISTENING: render_face_listening(); break;
        case FACE_THINKING:  render_face_thinking();  break;
        case FACE_SPEAKING:  render_face_speaking();  break;
        case FACE_HAPPY:     render_face_happy();     break;
        case FACE_SAD:       render_face_sad();       break;
        case FACE_ERROR:     render_face_error();     break;
        case FACE_SLEEP:     render_face_sleep();     break;
        default:             render_face_idle();      break;
    }
}

} // namespace

void display_face_init(void)
{
    s_current_face_state = FACE_IDLE;
    memset(s_face_buffer, 0, sizeof(s_face_buffer));
    render_current_face();
}

void display_face_update(uint32_t now_ms)
{
    (void)now_ms;
    render_current_face();
}

void display_face_set_state(face_state_t state)
{
    if (state < FACE_IDLE || state > FACE_SLEEP) {
        state = FACE_IDLE;
    }

    s_current_face_state = state;
    render_current_face();
}

face_state_t display_face_get_state(void)
{
    return s_current_face_state;
}

void display_face_render_mochi_gaze(int expr, int step,
                                    int sX, int sY,
                                    int gaze_x, int gaze_y,
                                    int eye_shift_x, int eye_shift_y)
{
    render_mochi_gaze(expr, step, sX, sY,
                      gaze_x, gaze_y,
                      eye_shift_x, eye_shift_y);
}

void display_face_render_mochi(int expr, int step,
                               int sX, int sY, int arahLirik)
{
    int gaze_x = 0;
    int gaze_y = 0;

    switch (arahLirik) {
        case 1: gaze_x = -5; break;
        case 2: gaze_x = 5;  break;
        case 3: gaze_y = -5; break;
        default: break;
    }

    display_face_render_mochi_gaze(expr, step,
                                   sX, sY,
                                   gaze_x, gaze_y,
                                   0, 0);
}

void display_face_render(void)
{
    render_current_face();
}

const uint8_t *display_face_buffer(void)
{
    return s_face_buffer;
}
