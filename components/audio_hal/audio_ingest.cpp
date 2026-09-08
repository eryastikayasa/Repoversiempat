#include "audio_ingest.h"
#include "audio_engine.h"
#include "websocket_mgr.h"

#include "esp_log.h"
#include "mbedtls/base64.h"

#include <string.h>

static const char *TAG = "AUDIO_INGEST";

#define AUDIO_INGEST_WORKSPACE_SIZE (24U * 1024U)

static uint8_t s_pcm_workspace[AUDIO_INGEST_WORKSPACE_SIZE];

extern uint64_t audio_bytes_received;
extern uint32_t audio_chunks_received;

static bool decode_and_queue(const char *base64, size_t base64_len)
{
    if (!base64 || base64_len == 0) return false;

    size_t pcm_len = 0;
    int ret = mbedtls_base64_decode(NULL, 0, &pcm_len,
                                    (const unsigned char *)base64, base64_len);
    if (ret != 0 && ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        ESP_LOGW(TAG, "Base64 audio length gagal: ret=-0x%04X", -ret);
        return false;
    }
    if (pcm_len == 0 || pcm_len > sizeof(s_pcm_workspace)) {
        ESP_LOGW(TAG, "PCM audio terlalu besar: need=%u capacity=%u",
                 (unsigned)pcm_len, (unsigned)sizeof(s_pcm_workspace));
        return false;
    }

    size_t decoded = pcm_len;
    ret = mbedtls_base64_decode(s_pcm_workspace, sizeof(s_pcm_workspace),
                                &decoded,
                                (const unsigned char *)base64, base64_len);
    if (ret != 0 || decoded == 0) {
        ESP_LOGW(TAG, "Base64 audio decode gagal: ret=-0x%04X", -ret);
        return false;
    }

    ++audio_chunks_received;
    audio_bytes_received += decoded;

    if (!queue_audio_pcm(s_pcm_workspace, decoded)) {
        ESP_LOGW(TAG, "PCM audio gagal masuk ke audio buffer: len=%u", (unsigned)decoded);
        return false;
    }

    ESP_LOGI(TAG, "Gemini audio: base64=%u -> PCM=%u -> audio buffer",
             (unsigned)base64_len, (unsigned)decoded);
    return true;
}

bool audio_ingest_base64(const char *base64, size_t base64_len)
{
    return decode_and_queue(base64, base64_len);
}

bool audio_ingest_compact_large_json_audio(char *json, size_t *io_len)
{
    if (!json || !io_len || *io_len == 0) return false;

    const size_t len = *io_len;
    const char *inline_key = strstr(json, "\"inlineData\"");
    if (!inline_key) return false;

    const char *data_key = strstr(inline_key, "\"data\"");
    if (!data_key || data_key >= json + len) return false;

    const char *p = data_key + strlen("\"data\"");
    const char *end = json + len;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    if (p >= end || *p != ':') return false;
    ++p;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    if (p >= end || *p != '\"') return false;
    ++p;

    char *b64 = (char *)p;
    char *q = b64;
    while (q < end) {
        if (*q == '\\') return false;
        if (*q == '\"') break;
        ++q;
    }
    if (q >= end) return false;

    const size_t b64_len = (size_t)(q - b64);
    if (b64_len < 4096) return false;

    if (!decode_and_queue(b64, b64_len)) return false;

    memmove(b64, q, (size_t)(end - q));
    *io_len = len - b64_len;
    return true;
}
