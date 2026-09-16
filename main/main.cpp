#include "display_face.h"
#include "display_text.h"
#include "display_engine.h"
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
#include "driver/gpio.h"
#include "esp_timer.h"

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

static volatile bool assistant_active = false;
static volatile bool wake_requested = false;
static int reconnect_attempts = 0;
static int64_t connect_start_us = 0;

static bool ensure_wakeword_ready(void)
{
    if (audio_engine_start_wakeword()) {
        display_face_set_state(FACE_SLEEP);
        display_text_set_status("Sistem siap. Katakan Hi, ESP...");
        return true;
    }

    ESP_LOGE(TAG, "WakeWord start gagal");
    display_face_set_state(FACE_ERROR);
    display_text_set_status("WakeWord gagal!");
    return false;
}

static void start_assistant_session(void)
{
    if (assistant_active) return;
    assistant_active = true;
    wake_requested = false;
    reconnect_attempts = 0;
    connect_start_us = esp_timer_get_time();

    /* Repo5 ownership model: WakeWord owns MIC only while idle. Gemini gets
     * exclusive MIC ownership after setupComplete through AudioEngine. */
    display_face_set_state(FACE_HAPPY);
    ESP_LOGI(TAG, "GEMINI_SESSION_START: menunggu WebSocket + setupComplete sebelum MIC TX");
    websocket_app_start();
}

static void app_supervisor_task(void *arg)
{
    (void)arg;

    for (;;) {
        if (!assistant_active) {
            if (wake_requested) {
                ESP_LOGI(TAG, "Wake request diterima supervisor. Memulai sesi...");
                start_assistant_session();
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }

            if (!audio_engine_input_session_active()) {
                (void)ensure_wakeword_ready();
            }

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

        if (websocket_is_intentional_standby()) {
            ESP_LOGI(TAG, "Gemini STANDBY: session selesai, mengembalikan WakeWord");
            assistant_active = false;
            wake_requested = false;
            audio_engine_stop_input_session();
            reconnect_attempts = 0;
            (void)ensure_wakeword_ready();
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* Gemini gets the MIC only after WebSocket + setupComplete. */
        if (!audio_engine_input_session_active()) {
            if (websocket_is_connected()) {
                ESP_LOGI(TAG, "GEMINI_SETUP_COMPLETE: handoff MIC -> Gemini");
                audio_engine_start_input_session();
                if (audio_engine_input_session_active())
                    ESP_LOGI(TAG, "MIC_TX_READY: Gemini owns MIC");
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }

            ESP_LOGI(TAG, "AudioEngine belum aktif: menunggu Gemini setupComplete");
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
                    ESP_LOGW(TAG, "Reconnect gagal, kembali ke WakeWord.");
                    assistant_active = false;
                    audio_engine_stop_input_session();
                    reconnect_attempts = 0;
                    (void)ensure_wakeword_ready();
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
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
                    ESP_LOGW(TAG, "Reconnect gagal, kembali ke WakeWord.");
                    assistant_active = false;
                    audio_engine_stop_input_session();
                    reconnect_attempts = 0;
                    (void)ensure_wakeword_ready();
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void sync_sntp_time(void)
{
    ESP_LOGI(TAG, "Mencari server NTP...");
    display_text_set_status("Sync Jam Network..");
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
            display_text_set_status("Jam Cocok!");
            vTaskDelay(pdMS_TO_TICKS(1000));
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    ESP_LOGW(TAG, "NTP gagal. Menggunakan waktu fallback.");
    struct timeval tv = { .tv_sec = 1770000000, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    display_text_set_status("Jam Set Fallback");
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
        display_text_set_status("Config Mode");
        web_config_start();
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    display_face_init();
    display_text_init();
    display_engine_init();
    display_engine_start();
    display_face_set_state(FACE_SLEEP);
    display_text_set_status("Booting...");

    audio_hal_init();
    if (!audio_engine_init()) {
        ESP_LOGE(TAG, "AudioEngine init gagal");
        display_text_set_status("Audio Engine Gagal!");
        display_face_set_state(FACE_ERROR);
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    gpio_set_direction(BOOT_BUTTON_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOOT_BUTTON_GPIO, GPIO_PULLUP_ONLY);
    uart_control_init();

    display_text_set_status("Menghubungkan WiFi...");
    wifi_init_sta();
    if (!wifi_wait_for_connection(15000)) {
        ESP_LOGE(TAG, "Wi-Fi tidak mendapatkan IP.");
        display_text_set_status("WiFi Gagal!");
        display_face_set_state(FACE_ERROR);
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "WiFi power save dimatikan");
    sync_sntp_time();

    if (!audio_engine_start_capture()) {
        ESP_LOGE(TAG, "AudioEngine capture subsystem gagal");
        display_text_set_status("Mic Engine Gagal!");
        display_face_set_state(FACE_ERROR);
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (!ensure_wakeword_ready()) {
        ESP_LOGE(TAG, "WakeWord belum READY - sistem tetap hidup untuk diagnostic");
    }

    audio_engine_set_mic_sink(
        [](const uint8_t *pcm, size_t len, void *ctx) {
            (void)ctx;
            websocket_send_audio_data(pcm, len);
        },
        nullptr);

    BaseType_t task_result = xTaskCreate(
        app_supervisor_task, "app_supervisor", 6144, nullptr, 5, nullptr);
    if (task_result != pdPASS)
        ESP_LOGE(TAG, "Gagal membuat app_supervisor task!");
    else
        ESP_LOGI(TAG, "app_supervisor aktif; MIC ownership = WakeWord idle / Gemini active");

    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}
