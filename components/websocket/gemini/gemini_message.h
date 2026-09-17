#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { GEMINI_MESSAGE_SETUP = 0, GEMINI_MESSAGE_AUDIO, GEMINI_MESSAGE_TEXT, GEMINI_MESSAGE_TOOL, GEMINI_MESSAGE_SESSION_RESUMPTION, GEMINI_MESSAGE_UNKNOWN } gemini_message_type_t;
gemini_message_type_t gemini_message_classify(const char *json, size_t len);
#ifdef __cplusplus
}
#endif
