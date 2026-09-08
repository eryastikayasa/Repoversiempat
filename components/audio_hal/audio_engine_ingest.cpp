#include "audio_engine.h"

#include "esp_log.h"
#include "mbedtls/base64.h"

#include <stdint.h>
#include <stddef.h>

static const char *TAG = "AUDIO_INGEST";

/* Decode in small bounded blocks so a large Gemini JSON message never needs
 * a large temporary PCM workspace. AudioEngine remains the sole owner of the
 * PCM ring, playback policy, and audio accounting. */
static constexpr size_t DECODE_INPUT_QUAD_CHARS = 4;
static constexpr size_t DECODE_OUTPUT_BLOCK = 768;

extern "C" bool audio_engine_push_model_audio_base64(const char *b64, size_t len, uint32_t generation)
{
    if (!b64 || len == 0) return false;

    size_t quad_len = 0;
    unsigned char quad[DECODE_INPUT_QUAD_CHARS];
    uint8_t pcm[DECODE_OUTPUT_BLOCK];
    bool pushed_any = false;

    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = (unsigned char)b64[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;

        quad[quad_len++] = c;
        if (quad_len != DECODE_INPUT_QUAD_CHARS) continue;

        size_t decoded = 0;
        const int ret = mbedtls_base64_decode(
            pcm, sizeof(pcm), &decoded, quad, DECODE_INPUT_QUAD_CHARS);
        quad_len = 0;

        if (ret != 0 || decoded == 0) {
            ESP_LOGE(TAG, "Base64 audio decode gagal: ret=%d", ret);
            return false;
        }

        if (!audio_engine_push_model_audio(pcm, decoded, generation)) {
            ESP_LOGW(TAG, "AudioEngine menolak PCM Base64 chunk: bytes=%u",
                     (unsigned)decoded);
            return pushed_any;
        }
        pushed_any = true;
    }

    if (quad_len != 0) {
        ESP_LOGW(TAG, "Base64 audio tidak lengkap: sisa=%u karakter", (unsigned)quad_len);
        return false;
    }

    return pushed_any;
}
