#ifndef TTS_CLIENT_H
#define TTS_CLIENT_H

#include "driver/i2s_std.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Speak text via Google Cloud Text-to-Speech API.
 *
 * Sends text to Cloud TTS (uk-UA-Wavenet-A), receives LINEAR16 PCM 24 kHz,
 * streams directly to I2S speaker via ring buffer.
 *
 * The speaker must be initialised at 24 kHz before calling this function.
 *
 * @param text       UTF-8 text to speak
 * @param tx_handle  I2S TX channel handle (speaker at 24 kHz)
 * @return ESP_OK on success
 */
esp_err_t tts_speak(const char *text, i2s_chan_handle_t tx_handle);

#ifdef __cplusplus
}
#endif

#endif // TTS_CLIENT_H
