#include "audio_hal.h"

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "AUDIO_HAL";

static constexpr gpio_num_t MIC_I2S_SCK = GPIO_NUM_5;
static constexpr gpio_num_t MIC_I2S_WS  = GPIO_NUM_4;
static constexpr gpio_num_t MIC_I2S_SD  = GPIO_NUM_6;
static constexpr gpio_num_t SPK_I2S_BCLK = GPIO_NUM_15;
static constexpr gpio_num_t SPK_I2S_LRCK = GPIO_NUM_16;
static constexpr gpio_num_t SPK_I2S_DOUT = GPIO_NUM_7;

static constexpr uint32_t MIC_SAMPLE_RATE = 16000;
static constexpr uint32_t SPK_SAMPLE_RATE = 24000;

static i2s_chan_handle_t s_rx = nullptr;
static i2s_chan_handle_t s_tx = nullptr;
static bool s_initialized = false;
static bool s_capture_started = false;
static bool s_playback_started = false;

static int32_t s_rx_raw[1024];
static int32_t s_tx_raw[512];

void audio_hal_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Audio HAL sudah diinisialisasi");
        return;
    }

    ESP_LOGI(TAG, "Audio HAL init: MIC=16k PCM16, SPK=24k PCM16");

    i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    tx_chan_cfg.dma_desc_num = 6;
    tx_chan_cfg.dma_frame_num = 240;
    tx_chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &s_tx, nullptr));

    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    rx_chan_cfg.dma_desc_num = 6;
    rx_chan_cfg.dma_frame_num = 240;
    rx_chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&rx_chan_cfg, nullptr, &s_rx));

    i2s_std_config_t rx_cfg = {};
    rx_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE);
    rx_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
    rx_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    rx_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    rx_cfg.gpio_cfg.bclk = MIC_I2S_SCK;
    rx_cfg.gpio_cfg.ws = MIC_I2S_WS;
    rx_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    rx_cfg.gpio_cfg.din = MIC_I2S_SD;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &rx_cfg));

    i2s_std_config_t tx_cfg = {};
    tx_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SPK_SAMPLE_RATE);
    tx_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
    tx_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
    tx_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    tx_cfg.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_32BIT;
    tx_cfg.slot_cfg.ws_pol = false;
    tx_cfg.slot_cfg.bit_shift = true;
    tx_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    tx_cfg.gpio_cfg.bclk = SPK_I2S_BCLK;
    tx_cfg.gpio_cfg.ws = SPK_I2S_LRCK;
    tx_cfg.gpio_cfg.dout = SPK_I2S_DOUT;
    tx_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &tx_cfg));

    s_initialized = true;
    ESP_LOGI(TAG, "Audio HAL hardware siap; I2S RX/TX belum dimulai");
}

esp_err_t audio_hal_start_capture(void)
{
    if (!s_initialized || !s_rx) return ESP_ERR_INVALID_STATE;
    if (s_capture_started) return ESP_OK;
    esp_err_t err = i2s_channel_enable(s_rx);
    if (err == ESP_OK) {
        s_capture_started = true;
        ESP_LOGI(TAG, "MIC capture START");
    }
    return err;
}

esp_err_t audio_hal_stop_capture(void)
{
    if (!s_initialized || !s_rx) return ESP_ERR_INVALID_STATE;
    if (!s_capture_started) return ESP_OK;
    esp_err_t err = i2s_channel_disable(s_rx);
    if (err == ESP_OK) {
        s_capture_started = false;
        ESP_LOGI(TAG, "MIC capture STOP");
    }
    return err;
}

esp_err_t audio_hal_read_pcm(int16_t *buffer, size_t samples, size_t *samples_read)
{
    if (samples_read) *samples_read = 0;
    if (!buffer || samples == 0 || !samples_read) return ESP_ERR_INVALID_ARG;
    if (!s_capture_started || !s_rx) return ESP_ERR_INVALID_STATE;
    if (samples > 1024) return ESP_ERR_INVALID_SIZE;

    size_t bytes_read = 0;
    const size_t input_bytes = samples * sizeof(int32_t);
    esp_err_t err = i2s_channel_read(s_rx, s_rx_raw, input_bytes, &bytes_read, portMAX_DELAY);
    if (err != ESP_OK) return err;

    const size_t count = bytes_read / sizeof(int32_t);
    for (size_t i = 0; i < count; ++i) {
        buffer[i] = (int16_t)(s_rx_raw[i] >> 16);
    }
    *samples_read = count;
    return ESP_OK;
}

esp_err_t audio_hal_start_playback(void)
{
    if (!s_initialized || !s_tx) return ESP_ERR_INVALID_STATE;
    if (s_playback_started) return ESP_OK;
    esp_err_t err = i2s_channel_enable(s_tx);
    if (err == ESP_OK) {
        s_playback_started = true;
        ESP_LOGI(TAG, "SPK playback START");
    }
    return err;
}

esp_err_t audio_hal_stop_playback(void)
{
    if (!s_initialized || !s_tx) return ESP_ERR_INVALID_STATE;
    if (!s_playback_started) return ESP_OK;
    esp_err_t err = i2s_channel_disable(s_tx);
    if (err == ESP_OK) {
        s_playback_started = false;
        ESP_LOGI(TAG, "SPK playback STOP");
    }
    return err;
}

esp_err_t audio_hal_write_pcm(const int16_t *buffer, size_t samples, size_t *samples_written)
{
    if (samples_written) *samples_written = 0;
    if (!buffer || samples == 0 || !samples_written) return ESP_ERR_INVALID_ARG;
    if (!s_playback_started || !s_tx) return ESP_ERR_INVALID_STATE;
    if (samples > 1024) return ESP_ERR_INVALID_SIZE;

    constexpr size_t I2S_WRITE_SAMPLES = 512;
    constexpr uint32_t I2S_WRITE_TIMEOUT_MS = 50;
    constexpr uint32_t MAX_NO_PROGRESS_RETRIES = 5;

    size_t total_written = 0;
    uint32_t no_progress_retries = 0;

    while (total_written < samples) {
        const size_t n = (samples - total_written > I2S_WRITE_SAMPLES)
                       ? I2S_WRITE_SAMPLES
                       : (samples - total_written);

        for (size_t i = 0; i < n; ++i) {
            s_tx_raw[i] = static_cast<int32_t>(buffer[total_written + i]) << 16;
        }

        size_t bytes_written = 0;
        const esp_err_t err = i2s_channel_write(
            s_tx,
            s_tx_raw,
            n * sizeof(int32_t),
            &bytes_written,
            I2S_WRITE_TIMEOUT_MS);

        size_t written = bytes_written / sizeof(int32_t);
        if (written > n) written = n;
        total_written += written;

        if (written > 0) {
            no_progress_retries = 0;
            vTaskDelay(1);
            continue;
        }

        if (err == ESP_OK) {
            ESP_LOGW(TAG, "I2S speaker write made no progress; retrying");
        } else {
            ESP_LOGW(TAG,
                     "I2S speaker write timeout/fail: err=%s written=%u/%u timeout=%ums",
                     esp_err_to_name(err),
                     (unsigned)bytes_written,
                     (unsigned)(n * sizeof(int32_t)),
                     (unsigned)I2S_WRITE_TIMEOUT_MS);
        }

        if (++no_progress_retries >= MAX_NO_PROGRESS_RETRIES) {
            *samples_written = total_written;
            vTaskDelay(1);
            return err != ESP_OK ? err : ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }

    *samples_written = total_written;
    return ESP_OK;
}
