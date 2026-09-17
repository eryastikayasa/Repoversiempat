#pragma once

#include "display_face.h"

#ifdef __cplusplus
extern "C" {
#endif

void display_engine_init(void);
void display_engine_start(void);
void display_engine_stop(void);

// System state is independent from conversation text. These setters only
// update the display-owned state consumed by the engine task.
void display_set_face_state(face_state_t face);
void display_set_status(const char *status);
void display_set_system_state(face_state_t face, const char *status);

#ifdef __cplusplus
}
#endif
