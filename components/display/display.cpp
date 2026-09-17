#include "display.h"
#include "display_driver.h"
#include "display_face.h"
#include "display_text.h"
#include "display_engine.h"

void oled_init(void)
{
    display_engine_init();
    display_engine_start();
}

void display_render_buffer(const uint8_t *buffer)
{
    // Legacy API retained for compatibility. The display engine owns the final
    // OLED presentation; callers may not bypass the compositor anymore.
    (void)buffer;
}

void display_status(const char *text)
{
    display_text_set_status(text ? text : "");
}

void face_set_state(face_state_t state)
{
    display_face_set_state(state);
}

face_state_t face_get_state(void)
{
    return display_face_get_state();
}

void display_render_mochi_gaze(int expr, int step, int sX, int sY,
                               int gaze_x, int gaze_y)
{
    display_face_render_mochi_gaze(expr, step, sX, sY, gaze_x, gaze_y, 0, 0);
}

void display_render_mochi(int expr, int step, int sX, int sY, int arahLirik)
{
    display_face_render_mochi(expr, step, sX, sY, arahLirik);
}

void face_render(void)
{
    display_face_render();
}
