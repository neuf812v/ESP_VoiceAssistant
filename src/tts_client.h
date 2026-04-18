#ifndef TTS_CLIENT_H
#define TTS_CLIENT_H

#include "driver/i2s_std.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Speak text via Gemini TTS API.
 *
 * Sends text to gemini-3.1-flash-tts-preview, streams the 24 kHz PCM
 * response directly to the I2S speaker (no large RAM buffer needed).
 *
 * The speaker must be initialised at 24 kHz before calling this function.
 *
 * @param text       UTF-8 text to speak (plain text, no JSON escaping needed)
 * @param tx_handle  I2S TX channel handle (speaker at 24 kHz)
 * @return ESP_OK on success
 */
esp_err_t tts_speak(const char *text, i2s_chan_handle_t tx_handle);

#ifdef __cplusplus
}
#endif

#endif // TTS_CLIENT_H
