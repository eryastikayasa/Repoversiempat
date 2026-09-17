#include "gemini_audio.h"
#include "audio_engine.h"
#include "cJSON.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "GEMINI_AUDIO";
static volatile bool s_active = false;

bool gemini_audio_turn_active(void) { return s_active; }

bool gemini_audio_process_server_message(const char *json, size_t len) {
    if (!json || !len) return false;

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return false;

    bool handled = false;
    cJSON *server_content = cJSON_GetObjectItem(root, "serverContent");
    if (cJSON_IsObject(server_content)) {
        cJSON *interrupted = cJSON_GetObjectItem(server_content, "interrupted");
        if (cJSON_IsTrue(interrupted)) {
            s_active = false;
            audio_engine_stop_playback();
            handled = true;
        }

        cJSON *model_turn = cJSON_GetObjectItem(server_content, "modelTurn");
        cJSON *parts = model_turn ? cJSON_GetObjectItem(model_turn, "parts") : nullptr;
        if (cJSON_IsArray(parts)) {
            for (int i = 0; i < cJSON_GetArraySize(parts); ++i) {
                cJSON *part = cJSON_GetArrayItem(parts, i);
                cJSON *inline_data = part ? cJSON_GetObjectItem(part, "inlineData") : nullptr;
                if (!cJSON_IsObject(inline_data)) continue;

                cJSON *data = cJSON_GetObjectItem(inline_data, "data");
                if (!cJSON_IsString(data) || !data->valuestring) continue;

                size_t encoded_len = strlen(data->valuestring);
                size_t pcm_capacity = (encoded_len / 4) * 3 + 4;
                size_t pcm_len = 0;
                uint8_t *pcm = (uint8_t *)malloc(pcm_capacity);
                if (!pcm) continue;

                if (mbedtls_base64_decode(pcm, pcm_capacity, &pcm_len,
                                          (const unsigned char *)data->valuestring,
                                          encoded_len) == 0 && pcm_len > 1) {
                    if (!s_active) {
                        s_active = true;
                        audio_engine_start_playback();
                    }
                    audio_engine_write_speaker_pcm((const int16_t *)pcm, pcm_len / 2, 100);
                    handled = true;
                }
                free(pcm);
            }
        }

        // turnComplete only ends the current model turn. It MUST NOT end
        // the Gemini session or disable the microphone. The next user turn
        // is sent on the same WebSocket session.
        cJSON *turn_complete = cJSON_GetObjectItem(server_content, "turnComplete");
        if (cJSON_IsTrue(turn_complete)) {
            s_active = false;
            handled = true;
            ESP_LOGI(TAG, "Gemini turnComplete - session remains active for next turn");
        }
    }

    cJSON_Delete(root);
    return handled;
}
