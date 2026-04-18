#ifndef I2S_MIC_H
#define I2S_MIC_H

#include "driver/i2s_std.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * INMP441 I2S Microphone
 *
 * Wiring (ESP32 DEVKIT1 -> INMP441):
 *   GPIO26 -> WS   (Word Select / LRCLK)
 *   GPIO25 -> SCK  (Serial Clock / BCLK)
 *   GPIO33 -> SD   (Serial Data)
 *   GND    -> L/R  (Left channel when LOW)
 *   3.3V   -> VDD
 *   GND    -> GND
 */

#define I2S_MIC_WS_PIN     26
#define I2S_MIC_SCK_PIN    25
#define I2S_MIC_SD_PIN     33

#define I2S_MIC_SAMPLE_RATE   16000
#define I2S_MIC_SAMPLE_BITS   I2S_DATA_BIT_WIDTH_32BIT
#define I2S_MIC_SLOT_MODE     I2S_SLOT_MODE_MONO

/** Initialize INMP441 microphone on I2S_NUM_0 */
esp_err_t i2s_mic_init(i2s_chan_handle_t *rx_handle);

/** Read samples from microphone. Returns bytes read. */
esp_err_t i2s_mic_read(i2s_chan_handle_t rx_handle, void *dest, size_t size, size_t *bytes_read, uint32_t timeout_ms);

/** Deinitialize microphone */
esp_err_t i2s_mic_deinit(i2s_chan_handle_t rx_handle);

#ifdef __cplusplus
}
#endif

#endif // I2S_MIC_H
