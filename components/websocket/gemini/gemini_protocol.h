#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
bool gemini_protocol_build_setup(char **output, size_t *output_len);
bool gemini_protocol_build_realtime_audio(const int16_t *pcm16, size_t samples, char **output, size_t *output_len);
bool gemini_protocol_process_message(const char *json, size_t len);
#ifdef __cplusplus
}
#endif
