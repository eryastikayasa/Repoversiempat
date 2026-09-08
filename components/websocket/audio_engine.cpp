#include "audio_engine.h"
#include "websocket_internal.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "AUDIO_ENGINE";

/* Gemini Live output: PCM16 mono at 24 kHz. */
static constexpr uint32_t ENGINE_OUTPUT_SAMPLE_RATE = 24000U;
static constexpr uint32_t ENGINE_OUTPUT_BYTES_PER_SEC = ENGINE_OUTPUT_SAMPLE_RATE * 2U;
static constexpr size_t ENGINE_PREBUFFER_BYTES = 128U * 1024U;
static constexpr size_t ENGINE_WARNING_BYTES = 64U * 1024U;
static constexpr size_t ENGINE_CRITICAL_BYTES = 32U * 1024U;

static volatile bool s_initialized = false;
static volatile audio_engine_state_t s_state = AUDIO_ENGINE_IDLE;
static audio_engine_turn_t s_turn = {};
static uint32_t s_last_generation = 0;
static TaskHandle_t s_task = nullptr;

static const char *state_name(audio_engine_state_t state)
{
    switch (state) {
        case AUDIO_ENGINE_IDLE: return "IDLE";
        case AUDIO_ENGINE_LISTENING: return "LISTENING";
        case AUDIO_ENGINE_THINKING: return "THINKING";
        case AUDIO_ENGINE_BUFFERING: return "BUFFERING";
        case AUDIO_ENGINE_PLAYING: return "PLAYING";
        case AUDIO_ENGINE_PLAYING_LOW: return "PLAYING_LOW";
        case AUDIO_ENGINE_DRAINING: return "DRAINING";
        case AUDIO_ENGINE_COMPLETE: return "COMPLETE";
        case AUDIO_ENGINE_INTERRUPTED: return "INTERRUPTED";
        case AUDIO_ENGINE_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

static void set_state(audio_engine_state_t next)
{
    if (s_state == next) return;
    ESP_LOGI(TAG, "STATE: %s -> %s", state_name(s_state), state_name(next));
    s_state = next;
}

static void reset_turn(uint32_t generation)
{
    s_turn = {};
    s_turn.generation = generation;
}

static void sync_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG,
             "AudioEngine: output=%uHz PCM16, prebuffer=%uB (~%ums), warning=%uB (~%ums), critical=%uB (~%ums)",
             (unsigned)ENGINE_OUTPUT_SAMPLE_RATE,
             (unsigned)ENGINE_PREBUFFER_BYTES,
             (unsigned)((ENGINE_PREBUFFER_BYTES * 1000U) / ENGINE_OUTPUT_BYTES_PER_SEC),
             (unsigned)ENGINE_WARNING_BYTES,
             (unsigned)((ENGINE_WARNING_BYTES * 1000U) / ENGINE_OUTPUT_BYTES_PER_SEC),
             (unsigned)ENGINE_CRITICAL_BYTES,
             (unsigned)((ENGINE_CRITICAL_BYTES * 1000U) / ENGINE_OUTPUT_BYTES_PER_SEC));

    for (;;) {
        audio_engine_sync_legacy_state();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

bool audio_engine_init(void)
{
    if (s_initialized) return true;

    s_last_generation = websocket_connection_generation;
    reset_turn(s_last_generation);

    BaseType_t rc = xTaskCreate(
        sync_task, "audio_engine", 3072, nullptr, 4, &s_task);
    if (rc != pdPASS) {
        s_state = AUDIO_ENGINE_ERROR;
        s_task = nullptr;
        ESP_LOGE(TAG, "Gagal membuat AudioEngine control task");
        return false;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "AudioEngine aktif: 1 otak audio");
    return true;
}

audio_engine_state_t audio_engine_get_state(void) { return s_state; }
const char *audio_engine_state_name(audio_engine_state_t state) { return state_name(state); }
const audio_engine_turn_t *audio_engine_get_turn(void) { return &s_turn; }

void audio_engine_notify(audio_engine_event_type_t event, uint32_t generation)
{
    if (!s_initialized) return;

    if (generation != 0 && generation != s_turn.generation) {
        reset_turn(generation);
        s_last_generation = generation;
        audio_turn_active = false;
        audio_turn_complete_pending = false;
        set_state(AUDIO_ENGINE_IDLE);
    }

    switch (event) {
        case AUDIO_ENGINE_EVENT_MODEL_BEGIN:
            s_turn.model_started = true;
            audio_turn_active = true;
            audio_turn_complete_pending = false;
            if (s_state == AUDIO_ENGINE_IDLE || s_state == AUDIO_ENGINE_LISTENING || s_state == AUDIO_ENGINE_THINKING)
                set_state(AUDIO_ENGINE_BUFFERING);
            break;

        case AUDIO_ENGINE_EVENT_MODEL_AUDIO:
            s_turn.model_started = true;
            audio_turn_active = true;
            if (s_state == AUDIO_ENGINE_IDLE || s_state == AUDIO_ENGINE_LISTENING || s_state == AUDIO_ENGINE_THINKING)
                set_state(AUDIO_ENGINE_BUFFERING);
            break;

        case AUDIO_ENGINE_EVENT_MODEL_TURN_COMPLETE:
            s_turn.model_complete = true;
            audio_turn_complete_pending = true;
            if (s_state == AUDIO_ENGINE_PLAYING || s_state == AUDIO_ENGINE_PLAYING_LOW || s_state == AUDIO_ENGINE_BUFFERING)
                set_state(AUDIO_ENGINE_DRAINING);
            break;

        case AUDIO_ENGINE_EVENT_PLAYBACK_STARTED:
            s_turn.playback_started = true;
            set_state(AUDIO_ENGINE_PLAYING);
            break;

        case AUDIO_ENGINE_EVENT_PLAYBACK_LOW:
            if (s_state == AUDIO_ENGINE_PLAYING)
                set_state(AUDIO_ENGINE_PLAYING_LOW);
            break;

        case AUDIO_ENGINE_EVENT_PLAYBACK_DRAINED:
            s_turn.playback_drained = true;
            break;

        case AUDIO_ENGINE_EVENT_I2S_DRAINED:
            s_turn.playback_drained = true;
            if (s_turn.model_complete) {
                audio_turn_complete_pending = false;
                audio_turn_active = false;
                set_state(AUDIO_ENGINE_COMPLETE);
                set_state(AUDIO_ENGINE_IDLE);
            }
            break;

        case AUDIO_ENGINE_EVENT_INTERRUPT:
            audio_turn_complete_pending = false;
            audio_turn_active = false;
            set_state(AUDIO_ENGINE_INTERRUPTED);
            reset_turn(s_turn.generation);
            break;

        case AUDIO_ENGINE_EVENT_GENERATION_CHANGED:
            reset_turn(generation);
            s_last_generation = generation;
            audio_turn_complete_pending = false;
            audio_turn_active = false;
            set_state(AUDIO_ENGINE_IDLE);
            break;

        case AUDIO_ENGINE_EVENT_ERROR:
            set_state(AUDIO_ENGINE_ERROR);
            break;

        default:
            break;
    }
}

void audio_engine_note_audio(size_t bytes)
{
    (void)bytes;
    if (!s_initialized) return;
    s_turn.pending_bytes = get_audio_pending_bytes();
}

void audio_engine_note_playback(size_t bytes)
{
    (void)bytes;
    if (!s_initialized) return;
    s_turn.pending_bytes = get_audio_pending_bytes();
}

void audio_engine_note_underrun(void)
{
    if (!s_initialized) return;
    s_turn.underrun_count++;
}

void audio_engine_sync_legacy_state(void)
{
    if (!s_initialized) return;

    const uint32_t generation = websocket_connection_generation;
    if (generation != s_last_generation) {
        s_last_generation = generation;
        reset_turn(generation);
        audio_turn_active = false;
        audio_turn_complete_pending = false;
        set_state(AUDIO_ENGINE_IDLE);
        return;
    }

    s_turn.generation = generation;
    s_turn.pending_bytes = get_audio_pending_bytes();
    s_turn.bytes_received = audio_bytes_received;
    s_turn.bytes_queued = audio_bytes_queued;
    s_turn.bytes_played = audio_bytes_played;
    s_turn.network_drop = audio_bytes_dropped;
    s_turn.playback_drop = audio_bytes_playback_dropped;

    if (audio_turn_complete_pending)
        s_turn.model_complete = true;

    if (s_turn.model_complete) {
        if (s_turn.pending_bytes > 0) {
            set_state(AUDIO_ENGINE_DRAINING);
        } else if (s_state != AUDIO_ENGINE_COMPLETE && s_state != AUDIO_ENGINE_IDLE) {
            /* Existing playback completion owns the physical I2S drain. */
            set_state(AUDIO_ENGINE_COMPLETE);
            set_state(AUDIO_ENGINE_IDLE);
        }
        return;
    }

    if (audio_turn_active) {
        if (s_turn.pending_bytes >= ENGINE_PREBUFFER_BYTES && s_state != AUDIO_ENGINE_PLAYING && s_state != AUDIO_ENGINE_PLAYING_LOW)
            set_state(AUDIO_ENGINE_PLAYING);
        else if (s_turn.pending_bytes > 0 && s_state == AUDIO_ENGINE_IDLE)
            set_state(AUDIO_ENGINE_BUFFERING);

        if (s_state == AUDIO_ENGINE_PLAYING && s_turn.pending_bytes < ENGINE_CRITICAL_BYTES)
            set_state(AUDIO_ENGINE_PLAYING_LOW);
        else if (s_state == AUDIO_ENGINE_PLAYING_LOW && s_turn.pending_bytes >= ENGINE_WARNING_BYTES)
            set_state(AUDIO_ENGINE_PLAYING);
    }
}
