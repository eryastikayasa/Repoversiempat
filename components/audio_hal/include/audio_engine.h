#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AUDIO_ENGINE_IDLE = 0,
    AUDIO_ENGINE_LISTENING,
    AUDIO_ENGINE_THINKING,
    AUDIO_ENGINE_BUFFERING,
    AUDIO_ENGINE_PLAYING,
    AUDIO_ENGINE_PLAYING_LOW,
    AUDIO_ENGINE_DRAINING,
    AUDIO_ENGINE_COMPLETE,
    AUDIO_ENGINE_INTERRUPTED,
    AUDIO_ENGINE_ERROR
} audio_engine_state_t;

typedef enum {
    AUDIO_ENGINE_EVENT_NONE = 0,
    AUDIO_ENGINE_EVENT_MODEL_BEGIN,
    AUDIO_ENGINE_EVENT_MODEL_AUDIO,
    AUDIO_ENGINE_EVENT_MODEL_TURN_COMPLETE,
    AUDIO_ENGINE_EVENT_PLAYBACK_STARTED,
    AUDIO_ENGINE_EVENT_PLAYBACK_LOW,
    AUDIO_ENGINE_EVENT_PLAYBACK_DRAINED,
    AUDIO_ENGINE_EVENT_I2S_DRAINED,
    AUDIO_ENGINE_EVENT_INTERRUPT,
    AUDIO_ENGINE_EVENT_GENERATION_CHANGED,
    AUDIO_ENGINE_EVENT_ERROR
} audio_engine_event_type_t;

typedef struct {
    uint32_t generation;
    bool model_started;
    bool model_complete;
    bool playback_started;
    bool playback_drained;
    uint64_t bytes_received;
    uint64_t bytes_queued;
    uint64_t bytes_played;
    uint64_t network_drop;
    uint64_t playback_drop;
    uint32_t underrun_count;
    size_t pending_bytes;
} audio_engine_turn_t;

bool audio_engine_init(void);
audio_engine_state_t audio_engine_get_state(void);
const char *audio_engine_state_name(audio_engine_state_t state);
bool audio_engine_turn_active(void);
const audio_engine_turn_t *audio_engine_get_turn(void);
void audio_engine_notify(audio_engine_event_type_t event, uint32_t generation);
void audio_engine_note_audio(size_t bytes);
void audio_engine_note_playback(size_t bytes);
void audio_engine_note_underrun(void);
void audio_engine_sync_legacy_state(void);

#ifdef __cplusplus
}
#endif
