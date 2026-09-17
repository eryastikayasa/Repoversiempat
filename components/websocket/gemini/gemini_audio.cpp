#include "gemini_audio.h"
#include "audio_engine.h"
#include "cJSON.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include <stdlib.h>
#include <string.h>
static const char*TAG="GEMINI_AUDIO";
static volatile bool s_active=false;
bool gemini_audio_turn_active(void){return s_active;}
bool gemini_audio_process_server_message(const char*json,size_t len){if(!json||!len)return false;cJSON*r=cJSON_ParseWithLength(json,len);if(!r)return false;bool handled=false;cJSON*sc=cJSON_GetObjectItem(r,"serverContent");if(cJSON_IsObject(sc)){cJSON*interrupt=cJSON_GetObjectItem(sc,"interrupted");if(cJSON_IsTrue(interrupt)){s_active=false;audio_engine_stop_playback();handled=true;}cJSON*turn=cJSON_GetObjectItem(sc,"modelTurn");cJSON*parts=turn?cJSON_GetObjectItem(turn,"parts"):nullptr;if(cJSON_IsArray(parts)){for(int i=0;i<cJSON_GetArraySize(parts);i++){cJSON*p=cJSON_GetArrayItem(parts,i);cJSON*in=p?cJSON_GetObjectItem(p,"inlineData"):nullptr;if(!cJSON_IsObject(in))continue;cJSON*d=cJSON_GetObjectItem(in,"data");if(!cJSON_IsString(d)||!d->valuestring)continue;size_t bl=strlen(d->valuestring),cap=(bl/4)*3+4,dl=0;uint8_t*pcm=(uint8_t*)malloc(cap);if(!pcm)continue;if(mbedtls_base64_decode(pcm,cap,&dl,(const unsigned char*)d->valuestring,bl)==0&&dl>1){if(!s_active){s_active=true;audio_engine_start_playback();}audio_engine_write_speaker_pcm((const int16_t*)pcm,dl/2,100);handled=true;}free(pcm);}}cJSON*tc=cJSON_GetObjectItem(sc,"turnComplete");if(cJSON_IsTrue(tc))s_active=false;}cJSON_Delete(r);return handled;}
