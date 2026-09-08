#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Audio ingress owns Gemini audio payload decoding.
 * WebSocket code may identify the protocol field, but it must not own
 * Base64/PCM decoding or audio accounting.
 */
bool audio_ingest_base64(const char *base64, size_t base64_len);

/*
 * Decode large inlineData payloads before cJSON parsing and compact the JSON
 * so the parser does not duplicate the large Base64 string.
 */
bool audio_ingest_compact_large_json_audio(char *json, size_t *io_len);

#ifdef __cplusplus
}
#endif
