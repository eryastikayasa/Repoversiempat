#include "gemini_message.h"
#include "cJSON.h"

gemini_message_type_t gemini_message_classify(const char *json, size_t len)
{
    if (!json || len == 0) return GEMINI_MESSAGE_UNKNOWN;
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return GEMINI_MESSAGE_UNKNOWN;
    gemini_message_type_t type = GEMINI_MESSAGE_UNKNOWN;
    if (cJSON_GetObjectItem(root, "setupComplete")) type = GEMINI_MESSAGE_SETUP;
    else if (cJSON_GetObjectItem(root, "toolCall")) type = GEMINI_MESSAGE_TOOL;
    else if (cJSON_GetObjectItem(root, "sessionResumptionUpdate")) type = GEMINI_MESSAGE_SESSION_RESUMPTION;
    else if (cJSON_GetObjectItem(root, "serverContent")) {
        cJSON *server = cJSON_GetObjectItem(root, "serverContent");
        if (cJSON_IsObject(server) && cJSON_GetObjectItem(server, "modelTurn")) type = GEMINI_MESSAGE_AUDIO;
        else type = GEMINI_MESSAGE_TEXT;
    } else if (cJSON_GetObjectItem(root, "inputTranscription") || cJSON_GetObjectItem(root, "outputTranscription")) type = GEMINI_MESSAGE_TEXT;
    cJSON_Delete(root);
    return type;
}
