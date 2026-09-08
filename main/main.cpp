#include "display.h"
#include "wifi_manager.h"
#include "websocket_mgr.h"
#include "audio_hal.h"
#include "audio_engine.h"
#include "uart_control.h"
#include "web_config.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"
#include "driver/gpio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs_flash.h"
#include "esp_sntp.h"

#include <sys/time.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

static const char *TAG = "MAIN";
static constexpr gpio_num_t BOOT_BUTTON_GPIO = GPIO_NUM_0;
static constexpr char WAKE_MODEL_NAME[] = "wn9_hiesp";

static srmodel_list_t *sr_models = nullptr;
static const esp_wn_iface_t *wake_iface = nullptr;
static model_iface_data_t *wake_model = nullptr;
static int wake_chunk_samples = 0;

static volatile bool assistant_active = false;
static volatile int reconnect_attempts = 0;
static int64_t connect_start_us = 0;

static bool wakeword_init(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "ESP-SR WAKE WORD INIT");
    ESP_LOGI(TAG, "Model: %s", WAKE_MODEL_NAME);

    sr_models = esp_srmodel_init("model");
    if (!sr_models) {
        ESP_LOGE(TAG, "ESP-SR model loader gagal");
        return false;
    }
    if (esp_srmodel_exists(sr_models, (char *)WAKE_MODEL_NAME) < 0) {
        ESP_LOGE(TAG, "WakeNet model tidak ditemukan: %s", WAKE_MODEL_NAME);
        esp_srmodel_deinit(sr_models);
        sr_models = nullptr;
        return false;
    }

    wake_iface = esp_wn_handle_from_name(WAKE_MODEL_NAME);
    if (!wake_iface) {
        ESP_LOGE(TAG, "WakeNet handle tidak ditemukan: %s", WAKE_MODEL_NAME);
        esp_srmodel_deinit(sr_models);
        sr_models = nullptr;
        return false;
    }

    ESP_LOGI(TAG, "WAKE HEAP: PSRAM free=%u largest=%u INTERNAL free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    wake_model = wake_iface->create(WAKE_MODEL_NAME, DET_MODE_90);
    if (!wake_model) {
        ESP_LOGE(TAG, "Gagal membuat WakeNet model: %s", WAKE_MODEL_NAME);
        wake_iface = nullptr;
        esp_srmodel_deinit(sr_models);
        sr_models = nullptr;
        return false;
    }

    wake_chunk_samples = wake_iface->get_samp_chunksize(wake_model);
    const int wake_rate = wake_iface->get_samp_rate(wake_model);
    const int wake_channels = wake_iface->get_channel_num(wake_model);
    ESP_LOGI(TAG, "WakeNet ready: rate=%dHz chunk=%d samples channels=%d",
             wake_rate, wake_chunk_samples, wake_channels);

    if (wake_rate != MIC_SAMPLE_RATE || wake_channels != 1) {
        ESP_LOGE(TAG, "WakeNet audio mismatch: expected %dHz mono", MIC_SAMPLE_RATE);
        wake_iface->destroy(wake_model);
        wake_model = nullptr;
        wake_iface = nullptr;
        wake_chunk_samples = 0;
        esp_srmodel_deinit(sr_models);
        sr_models = nullptr;
        return false;
    }

    ESP_LOGI(TAG, "Wake word aktif: HI, ESP");
    return true;
}

static void wakeword_frame_cb(const uint8_t *pcm, size_t len, void *ctx)
{
    (void)ctx;
    if (assistant_active || !wake_iface || !wake_model || wake_chunk_samples <= 0)
        return;

    static int16_t wake_buffer[1024];
    static size_t wake_buffer_samples = 0;
    const size_t incoming_samples = len / sizeof(int16_t);
    const int16_t *samples = reinterpret_cast<const int16_t *>(pcm);

    if (!samples || incoming_samples == 0) return;

    if (wake_buffer_samples + incoming_samples > (sizeof(wake_buffer) / sizeof(wake_buffer[0]))) {
        wake_buffer_samples = 0;
    }

    memcpy(wake_buffer + wake_buffer_samples, samples,
           incoming_samples * sizeof(int16_t));
    wake_buffer_samples += incoming_samples;

    while (!assistant_active &&
           wake_buffer_samples >= (size_t)wake_chunk_samples) {
        const int result = wake_iface->detect(wake_model, wake_buffer);
        if (result > 0) {
            ESP_LOGW(TAG, ">>> WAKE WORD TERDETEKSI: HI, ESP (id=%d)", result);
            assistant_active = true;
            reconnect_attempts = 0;
            connect_start_us = esp_timer_get_time();
            audio_engine_start_input_session();
            face_set_state(FACE_HAPPY);
            websocket_app_start();
            wake_buffer_samples = 0;
            return;
        }

        const size_t remainder = wake_buffer_samples - (size_t)wake_chunk_samples;
        if (remainder > 0)
            memmove(wake_buffer, wake_buffer + wake_chunk_samples,
                    remainder * sizeof(int16_t));
        wake_buffer_samples = remainder;
    }
}

static void start_assistant_session(void)
{
    if (assistant_active) return;
    assistant_active = true;
    reconnect_attempts = 0;
    connect_start_us = esp_timer_get_time();
    audio_engine_start_input_session();
    face_set_state(FACE_HAPPY);
    websocket_app_start();
}

static void app_supervisor_task(void *arg)
{
    (void)arg;

    for (;;) {
        if (!assistant_active) {
            if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                vTaskDelay(pdMS_TO_TICKS(50));
                if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                    while (gpio_get_level(BOOT_BUTTON_GPIO) == 0)
                        vTaskDelay(pdMS_TO_TICKS(10));
                    ESP_LOGI(TAG, "Tombol ditekan! Memulai sesi...");
                    start_assistant_session();
                }
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        /* AudioEngine owns the 60-second audio-idle decision. */
        if (!audio_engine_input_session_active()) {
            ESP_LOGI(TAG, "AudioEngine mengakhiri sesi MIC");
            assistant_active = false;
            websocket_disconnect();
            face_set_state(FACE_SLEEP);
            reconnect_attempts = 0;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (!websocket_is_connected()) {
            if (esp_timer_get_time() - connect_start_us > 15 * 1000000LL) {
                if (reconnect_attempts < 5) {
                    const int delay_sec = 2 << reconnect_attempts;
                    ++reconnect_attempts;
                    ESP_LOGW(TAG, "Reconnect attempt %d in %d sec...",
                             reconnect_attempts, delay_sec);
                    vTaskDelay(pdMS_TO_TICKS(delay_sec * 1000));
                    websocket_app_start();
                    connect_start_us = esp_timer_get_time();
                } else {
                    ESP_LOGW(TAG, "Reconnect gagal, kembali ke mode sleep.");
                    assistant_active = false;
                    audio_engine_stop_input_session();
                    face_set_state(FACE_SLEEP);
                    reconnect_attempts = 0;
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static bool resolve_host(const char *label, const char *host, const char *port)
{
    ESP_LOGI(TAG, "DNS [%s]: %s:%s", label, host, port);
    struct addrinfo hints = {};
    struct addrinfo *result = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    const int err = getaddrinfo(host, port, &hints, &result);
    if (err != 0 || !result) {
        ESP_LOGE(TAG, "DNS [%s]: FAILED err=%d errno=%d", label, err, errno);
        return false;
    }
    freeaddrinfo(result);
    ESP_LOGI(TAG, "DNS [%s]: OK", label);
    return true;
}

static void debug_network_path(void)
{
    const bool google_ok = resolve_host("google.com", "google.com", "443");
    const bool gemini_ok = resolve_host("Gemini", "generativelanguage.googleapis.com", "443");
    ESP_LOGI(TAG, "NETWORK BASIC: google=%s Gemini=%s",
             google_ok ? "OK" : "FAILED", gemini_ok ? "OK" : "FAILED");
}

static void sync_sntp_time(void)
{
    ESP_LOGI(TAG, "Mencari server NTP...");
    display_status("Sync Jam Network..");
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "time.google.com");
    esp_sntp_setservername(1, "id.pool.ntp.org");
    esp_sntp_setservername(2, "pool.ntp.org");
    esp_sntp_init();

    int retry = 0;
    time_t now = 0;
    struct tm timeinfo = {};
    while (retry++ < 10) {
        time(&now);
        localtime_r(&now, &timeinfo);
        if (timeinfo.tm_year >= (2024 - 1900)) {
            ESP_LOGI(TAG, "Waktu cocok! Tahun: %d", timeinfo.tm_year + 1900);
            display_status("Jam Cocok!");
            vTaskDelay(pdMS_TO_TICKS(1000));
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    ESP_LOGW(TAG, "NTP gagal. Menggunakan waktu fallback.");
    struct timeval tv = { .tv_sec = 1770000000, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    display_status("Jam Set Fallback");
}

extern "C" void app_main()
{
    ESP_LOGI(TAG, "Total PSRAM: %d bytes", esp_psram_get_size());
    ESP_LOGI(TAG, "Free Heap: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "Free PSRAM: %d bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "ESP32-S3 Asisten Kamar Dimulai...");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    if (web_config_is_needed()) {
        display_status("Config Mode");
        web_config_start();
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    oled_init();
    face_animation_start();
    face_set_state(FACE_SLEEP);
    display_status("Booting...");

    audio_hal_init();
    audio_i2s_test_tone();

    if (!audio_engine_init()) {
        ESP_LOGE(TAG, "AudioEngine init gagal");
        display_status("Audio Engine Gagal!");
        face_set_state(FACE_ERROR);
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    const bool wake_ready = wakeword_init();
    if (!wake_ready) {
        ESP_LOGE(TAG, "WakeNet init gagal. Sistem tetap bisa dimulai dengan tombol BOOT.");
        display_status("WakeNet gagal!");
    } else {
        display_status("Katakan: Hi, ESP");
    }

    gpio_set_direction(BOOT_BUTTON_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOOT_BUTTON_GPIO, GPIO_PULLUP_ONLY);
    uart_control_init();

    display_status("Menghubungkan WiFi...");
    wifi_init_sta();
    if (!wifi_wait_for_connection(15000)) {
        ESP_LOGE(TAG, "Wi-Fi tidak mendapatkan IP.");
        display_status("WiFi Gagal!");
        face_set_state(FACE_ERROR);
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "WiFi power save dimatikan");
    sync_sntp_time();
    debug_network_path();

    audio_engine_set_mic_listener(wake_ready ? wakeword_frame_cb : nullptr, nullptr);
    if (!audio_engine_start_capture()) {
        ESP_LOGE(TAG, "AudioEngine capture gagal");
        display_status("Mic Engine Gagal!");
        face_set_state(FACE_ERROR);
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    face_set_state(FACE_SLEEP);
    display_status("Sistem siap. Katakan Hi, ESP...");

    BaseType_t task_result = xTaskCreate(
        app_supervisor_task, "app_supervisor", 6144, nullptr, 5, nullptr);
    if (task_result != pdPASS)
        ESP_LOGE(TAG, "Gagal membuat app_supervisor task!");
    else
        ESP_LOGI(TAG, "app_supervisor aktif; MIC dan framing sepenuhnya milik AudioEngine");

    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}
