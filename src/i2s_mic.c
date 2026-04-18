#include "i2s_mic.h"
#include "esp_log.h"

static const char *TAG = "i2s_mic";

esp_err_t i2s_mic_init(i2s_chan_handle_t *rx_handle)
{
    // Channel config — use I2S_NUM_0 for mic
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 240;

    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S RX channel: %s", esp_err_to_name(ret));
        return ret;
    }

    // Standard mode config for INMP441
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_MIC_SAMPLE_BITS, I2S_MIC_SLOT_MODE),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)I2S_MIC_SCK_PIN,
            .ws   = (gpio_num_t)I2S_MIC_WS_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din  = (gpio_num_t)I2S_MIC_SD_PIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    // INMP441 outputs on left channel when L/R pin is LOW
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ret = i2s_channel_init_std_mode(*rx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2S STD RX: %s", esp_err_to_name(ret));
        i2s_del_channel(*rx_handle);
        return ret;
    }

    ret = i2s_channel_enable(*rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S RX: %s", esp_err_to_name(ret));
        i2s_del_channel(*rx_handle);
        return ret;
    }

    ESP_LOGI(TAG, "INMP441 mic initialized (SR=%d, %dbit)", I2S_MIC_SAMPLE_RATE, 32);
    return ESP_OK;
}

esp_err_t i2s_mic_read(i2s_chan_handle_t rx_handle, void *dest, size_t size, size_t *bytes_read, uint32_t timeout_ms)
{
    return i2s_channel_read(rx_handle, dest, size, bytes_read, timeout_ms);
}

esp_err_t i2s_mic_deinit(i2s_chan_handle_t rx_handle)
{
    i2s_channel_disable(rx_handle);
    return i2s_del_channel(rx_handle);
}
