#include "web_config.h"
#include "wifi_manager.h"

#include "display_engine.h"
#include "display_face.h"
#include "display_text.h"
#include "audio_engine.h"
#include "websocket.h"
#include "websocket_audio.h"
#include "websocket_event.h"

#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "MAIN";
static constexpr gpio_num_t BOOT_BUTTON_GPIO = GPIO_NUM_0;
static bool s_conversation_starting = false;

static bool init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS perlu di-erase lalu init ulang");
        err = nvs_flash_erase();
        if (err != ESP_OK) return false;
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return false;
    ESP_LOGI(TAG, "NVS READY");
    return true;
}

static void init_display(void)
{
    display_face_init();
    display_text_init();
    display_text_set_status("Memulai...");
    display_engine_init();
    display_engine_start();
    ESP_LOGI(TAG, "DISPLAY READY");
}

static bool init_boot_button(void)
{
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    const esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) return false;
    ESP_LOGI(TAG, "BOOT button READY: GPIO0, tekan untuk memulai sesi Gemini");
    return true;
}

static bool init_wakeword(void)
{
    if (!audio_engine_init()) return false;
    if (!audio_engine_start_wakeword()) {
        audio_engine_stop();
        return false;
    }
    display_face_set_state(FACE_IDLE);
    display_text_set_status("Siap - ucap HI ESP");
    ESP_LOGI(TAG, "WAKEWORD READY - menunggu HI, ESP / BOOT");
    return true;
}

static bool init_websocket(void)
{
    const esp_err_t err = websocket_init();
    if (err != ESP_OK) return false;
    ESP_LOGI(TAG, "WEBSOCKET READY - menunggu WakeWord / BOOT");
    return true;
}

static bool restart_wakeword_after_conversation_failure(void)
{
    websocket_disconnect();
    audio_engine_stop_conversation();
    if (!audio_engine_start_wakeword()) {
        display_face_set_state(FACE_ERROR);
        display_text_set_status("WakeWord gagal");
        return false;
    }
    display_face_set_state(FACE_IDLE);
    display_text_set_status("Siap - ucap HI ESP");
    ESP_LOGI(TAG, "MODE: WAKEWORD ON - kembali menunggu HI ESP");
    return true;
}

static bool start_conversation(void)
{
    if (s_conversation_starting || audio_engine_conversation_active()) return false;
    s_conversation_starting = true;

    display_face_set_state(FACE_LISTENING);
    display_text_set_status("WakeWord OFF...");

    // Exclusive MIC ownership: WakeWord MUST stop and release MIC first.
    audio_engine_clear_wakeword();
    audio_engine_stop_wakeword();
    if (audio_engine_conversation_active()) {
        ESP_LOGE(TAG, "Invariant gagal: conversation MIC aktif saat transisi");
        s_conversation_starting = false;
        return false;
    }

    ESP_LOGI(TAG, "MODE TRANSITION: WAKEWORD STOP -> MIC RELEASED");
    display_text_set_status("Menghubungkan Gemini...");

    const esp_err_t ws_err = websocket_connect();
    if (ws_err != ESP_OK) {
        display_face_set_state(FACE_ERROR);
        display_text_set_status("Gemini gagal");
        s_conversation_starting = false;
        restart_wakeword_after_conversation_failure();
        return false;
    }

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(15000);
    while (!websocket_event_gemini_ready()) {
        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) {
            display_face_set_state(FACE_ERROR);
            display_text_set_status("Gemini timeout");
            s_conversation_starting = false;
            restart_wakeword_after_conversation_failure();
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    ESP_LOGI(TAG, "MODE: GEMINI setupComplete / READY");
    display_text_set_status("Mendengarkan...");

    // Gemini owns the MIC only after setupComplete.
    if (!audio_engine_start_conversation()) {
        display_face_set_state(FACE_ERROR);
        display_text_set_status("Audio gagal");
        s_conversation_starting = false;
        restart_wakeword_after_conversation_failure();
        return false;
    }

    if (!websocket_audio_start()) {
        audio_engine_stop_conversation();
        display_face_set_state(FACE_ERROR);
        display_text_set_status("Uplink gagal");
        s_conversation_starting = false;
        restart_wakeword_after_conversation_failure();
        return false;
    }

    s_conversation_starting = false;
    ESP_LOGI(TAG, "MODE: GEMINI ACTIVE - MIC owner=GEMINI");
    return true;
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "ESP32-S3 application start");
    ESP_LOGI(TAG, "========================================");

    if (!init_nvs()) {
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (web_config_is_needed()) {
        ESP_LOGW(TAG, "Konfigurasi belum siap - masuk WebConfig");
        web_config_start();
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "Konfigurasi ditemukan - lanjut Wi-Fi");
    init_display();
    display_text_set_status("WiFi...");
    wifi_init_sta();

    if (!wifi_wait_for_connection(30000)) {
        display_face_set_state(FACE_ERROR);
        display_text_set_status("WiFi gagal");
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    display_face_set_state(FACE_IDLE);
    display_text_set_status("WiFi OK");
    ESP_LOGI(TAG, "WIFI READY");

    if (!init_websocket()) {
        display_face_set_state(FACE_ERROR);
        display_text_set_status("WebSocket gagal");
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (!init_boot_button()) {
        display_face_set_state(FACE_ERROR);
        display_text_set_status("BOOT gagal");
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (!init_wakeword()) {
        display_face_set_state(FACE_ERROR);
        display_text_set_status("WakeWord gagal");
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    bool boot_button_down = (gpio_get_level(BOOT_BUTTON_GPIO) == 0);
    while (true) {
        const bool boot_pressed = (gpio_get_level(BOOT_BUTTON_GPIO) == 0);
        if (boot_pressed && !boot_button_down) {
            vTaskDelay(pdMS_TO_TICKS(30));
            if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                if (start_conversation()) ESP_LOGI(TAG, "MAIN: conversation mode ACTIVE (BOOT)");
                boot_button_down = true;
            }
        } else if (!boot_pressed) {
            boot_button_down = false;
        }

        if (!s_conversation_starting && !audio_engine_conversation_active() &&
            audio_engine_wakeword_detected()) {
            ESP_LOGI(TAG, "MAIN: WakeWord event");
            audio_engine_clear_wakeword();
            if (start_conversation()) ESP_LOGI(TAG, "MAIN: conversation mode ACTIVE (WakeWord)");
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
