#include "display.h"
#include "display_face.h"
#include "display_engine.h"

// Compatibility facade. Animation/render ownership moved to display_engine;
// keep the legacy public API used by the application and Gemini tool path.
void face_animation_start(void)
{
    display_engine_start();
}

void face_animation_stop(void)
{
    display_engine_stop();
}

void face_show_for_ms(face_state_t state, uint32_t duration_ms)
{
    display_face_show_for_ms(state, duration_ms);
}
