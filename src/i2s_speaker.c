#include "i2s_speaker.h"
#include "esp_log.h"

static const char *TAG = "i2s_spk";

esp_err_t i2s_speaker_init_rate(i2s_chan_handle_t *tx_handle, uint32_t sample_rate)
{
    // Channel config — use I2S_NUM_1 for speaker
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 480;

    esp_err_t ret = i2s_new_channel(&chan_cfg, tx_handle, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S TX channel: %s", esp_err_to_name(ret));
        return ret;
    }

    // Standard mode config for MAX98357A
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_SPK_SAMPLE_BITS, I2S_SPK_SLOT_MODE),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)I2S_SPK_SCK_PIN,
            .ws   = (gpio_num_t)I2S_SPK_WS_PIN,
            .dout = (gpio_num_t)I2S_SPK_SD_PIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ret = i2s_channel_init_std_mode(*tx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2S STD TX: %s", esp_err_to_name(ret));
        i2s_del_channel(*tx_handle);
        return ret;
    }

    ret = i2s_channel_enable(*tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S TX: %s", esp_err_to_name(ret));
        i2s_del_channel(*tx_handle);
        return ret;
    }

    ESP_LOGI(TAG, "MAX98357A speaker initialized (SR=%lu, %dbit)", (unsigned long)sample_rate, 16);
    return ESP_OK;
}

esp_err_t i2s_speaker_init(i2s_chan_handle_t *tx_handle)
{
    return i2s_speaker_init_rate(tx_handle, I2S_SPK_SAMPLE_RATE);
}

esp_err_t i2s_speaker_write(i2s_chan_handle_t tx_handle, const void *src, size_t size, size_t *bytes_written, uint32_t timeout_ms)
{
    return i2s_channel_write(tx_handle, src, size, bytes_written, timeout_ms);
}

esp_err_t i2s_speaker_deinit(i2s_chan_handle_t tx_handle)
{
    i2s_channel_disable(tx_handle);
    return i2s_del_channel(tx_handle);
}
