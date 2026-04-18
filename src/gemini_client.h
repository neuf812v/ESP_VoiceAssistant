#ifndef GEMINI_CLIENT_H
#define GEMINI_CLIENT_H

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Send PCM audio to Gemini API and get a text response.
 *
 * Audio is wrapped in a WAV header and base64-encoded in chunks
 * to avoid holding the full encoded data in RAM.
 *
 * @param pcm_data   16-bit signed PCM samples, 16 kHz mono
 * @param num_samples number of samples in pcm_data
 * @param response_text output buffer for the text response
 * @param max_response_len size of response_text buffer
 * @return ESP_OK on success
 */
esp_err_t gemini_ask(const int16_t *pcm_data, size_t num_samples,
                     char *response_text, size_t max_response_len);

#ifdef __cplusplus
}
#endif

#endif // GEMINI_CLIENT_H
