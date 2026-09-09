#pragma once

// Temporary compatibility bridge for modules not yet migrated to the
// split Display Face/Text APIs. No OLED/I2C implementation lives here.
#include "display_face.h"
#include "display_text.h"

#define display_status              display_text_set_status
#define display_set_user_text       display_text_set_user
#define display_set_gemini_text     display_text_set_gemini
#define display_append_user_text    display_text_append_user
#define display_append_gemini_text  display_text_append_gemini
