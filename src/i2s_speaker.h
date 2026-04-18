#ifndef I2S_SPEAKER_H
#define I2S_SPEAKER_H

#include "driver/i2s_std.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * MAX98357A I2S Amplifier/Speaker
 *
 * Wiring (ESP32 DEVKIT1 -> MAX98357A):
 *   GPIO22 -> LRC  (Word Select / LRCLK)
 *   GPIO14 -> BCLK (Bit Clock)
 *   GPIO27 -> DIN  (Data In)
 *   ---    -> GAIN (unconnected = 9dB, or GND = 12dB, or VDD = 15dB)
 *   ---    -> SD   (Shutdown: HIGH = on, pull-up internally; or tie to VDD)
 *   5V     -> VIN
 *   GND    -> GND
 */

#define I2S_SPK_WS_PIN     22
#define I2S_SPK_SCK_PIN    14
#define I2S_SPK_SD_PIN     27

#define I2S_SPK_SAMPLE_RATE   16000
#define I2S_SPK_SAMPLE_BITS   I2S_DATA_BIT_WIDTH_16BIT
#define I2S_SPK_SLOT_MODE     I2S_SLOT_MODE_MONO

/** Initialize MAX98357A speaker on I2S_NUM_1 (default 16 kHz) */
esp_err_t i2s_speaker_init(i2s_chan_handle_t *tx_handle);

/** Initialize speaker with custom sample rate (e.g. 24000 for TTS) */
esp_err_t i2s_speaker_init_rate(i2s_chan_handle_t *tx_handle, uint32_t sample_rate);

/** Write samples to speaker. Returns bytes written. */
esp_err_t i2s_speaker_write(i2s_chan_handle_t tx_handle, const void *src, size_t size, size_t *bytes_written, uint32_t timeout_ms);

/** Deinitialize speaker */
esp_err_t i2s_speaker_deinit(i2s_chan_handle_t tx_handle);

#ifdef __cplusplus
}
#endif

#endif // I2S_SPEAKER_H
