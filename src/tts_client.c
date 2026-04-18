#include "tts_client.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "mbedtls/base64.h"
#include "i2s_speaker.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"

static const char *TAG = "tts";

/* ---------- Configuration ---------- */
#define TTS_MODEL     "gemini-3.1-flash-tts-preview"
#define TTS_URL       "https://generativelanguage.googleapis.com/v1beta/models/" \
                      TTS_MODEL ":generateContent?key="
#define TTS_VOICE     "Kore"

#define READ_BUF_SZ   4096
#define B64_BUF_SZ    1024          /* must be multiple of 4 */
#define PCM_BUF_SZ    (B64_BUF_SZ * 3 / 4)   /* 768 bytes = 384 samples */
#define FADE_SAMPLES  1200          /* 50 ms at 24 kHz */
#define TTS_VOLUME    75            /* percent of full volume (reduces brownout) */
#define RING_SZ       (12 * 1024)   /* ~250 ms ring buffer at 24 kHz 16-bit */
#define PREFILL_SZ    (6 * 1024)    /* start playback after ~125 ms buffered */
#define PLAY_CHUNK    768           /* consumer I2S write chunk */
#define PRODUCER_STACK 6144

/* ---------- JSON text escaping ---------- */
static size_t json_escape(const char *src, char *dst, size_t dst_sz)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dst_sz - 2; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') {
            dst[j++] = '\\';
            if (j < dst_sz - 1) dst[j++] = c;
        } else if (c == '\n') {
            dst[j++] = '\\';
            if (j < dst_sz - 1) dst[j++] = 'n';
        } else if (c == '\r') {
            /* skip */
        } else if (c == '\t') {
            dst[j++] = '\\';
            if (j < dst_sz - 1) dst[j++] = 't';
        } else {
            dst[j++] = c;
        }
    }
    dst[j] = '\0';
    return j;
}

/* ---------- Producer context (HTTP → decode → ring buffer) ---------- */
typedef struct {
    esp_http_client_handle_t client;
    StreamBufferHandle_t     sbuf;
    volatile bool            done;
    volatile size_t          total_pcm;
} producer_ctx_t;

static void tts_producer_task(void *arg)
{
    producer_ctx_t *ctx = (producer_ctx_t *)arg;
    char *read_buf = malloc(READ_BUF_SZ);
    if (!read_buf) { ctx->done = true; vTaskDelete(NULL); return; }

    enum { FIND_DATA, STREAM_B64, PARSE_DONE } state = FIND_DATA;
    char search_buf[32];
    int search_len = 0;
    static const char *PATTERN1 = "\"data\":\"";
    static const char *PATTERN2 = "\"data\": \"";
    int pat1_len = 8;
    int pat2_len = 9;

    unsigned char b64_acc[B64_BUF_SZ];
    int b64_len = 0;
    uint8_t pcm_out[PCM_BUF_SZ];
    size_t total_pcm = 0;

    while (state != PARSE_DONE) {
        int rd = esp_http_client_read(ctx->client, read_buf, READ_BUF_SZ);
        if (rd > 0) {
            for (int i = 0; i < rd && state != PARSE_DONE; i++) {
                char c = read_buf[i];

                if (state == FIND_DATA) {
                    if (search_len < (int)sizeof(search_buf) - 1) {
                        search_buf[search_len++] = c;
                    } else {
                        memmove(search_buf, search_buf + 1, search_len - 1);
                        search_buf[search_len - 1] = c;
                    }
                    search_buf[search_len] = '\0';

                    if ((search_len >= pat1_len &&
                         memcmp(search_buf + search_len - pat1_len,
                                PATTERN1, pat1_len) == 0) ||
                        (search_len >= pat2_len &&
                         memcmp(search_buf + search_len - pat2_len,
                                PATTERN2, pat2_len) == 0))
                    {
                        state = STREAM_B64;
                        b64_len = 0;
                        ESP_LOGI(TAG, "Found audio data, streaming...");
                    }
                }
                else if (state == STREAM_B64) {
                    if (c == '"') {
                        if (b64_len > 0) {
                            while (b64_len % 4 != 0)
                                b64_acc[b64_len++] = '=';
                            size_t olen = 0;
                            mbedtls_base64_decode(pcm_out, sizeof(pcm_out),
                                                  &olen, b64_acc, b64_len);
                            if (olen > 0) {
                                int16_t *samples = (int16_t *)pcm_out;
                                for (size_t s = 0; s < olen / 2; s++) {
                                    size_t g = (total_pcm / 2) + s;
                                    int32_t v = (int32_t)samples[s] * TTS_VOLUME / 100;
                                    if (g < FADE_SAMPLES)
                                        v = v * (int32_t)g / FADE_SAMPLES;
                                    samples[s] = (int16_t)v;
                                }
                                xStreamBufferSend(ctx->sbuf, pcm_out, olen,
                                                  pdMS_TO_TICKS(5000));
                                total_pcm += olen;
                            }
                        }
                        state = PARSE_DONE;
                    }
                    else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                             (c >= '0' && c <= '9') || c == '+' || c == '/' ||
                             c == '=')
                    {
                        b64_acc[b64_len++] = (unsigned char)c;
                        if (b64_len >= B64_BUF_SZ) {
                            size_t olen = 0;
                            mbedtls_base64_decode(pcm_out, sizeof(pcm_out),
                                                  &olen, b64_acc, B64_BUF_SZ);
                            if (olen > 0) {
                                int16_t *samples = (int16_t *)pcm_out;
                                for (size_t s = 0; s < olen / 2; s++) {
                                    size_t g = (total_pcm / 2) + s;
                                    int32_t v = (int32_t)samples[s] * TTS_VOLUME / 100;
                                    if (g < FADE_SAMPLES)
                                        v = v * (int32_t)g / FADE_SAMPLES;
                                    samples[s] = (int16_t)v;
                                }
                                xStreamBufferSend(ctx->sbuf, pcm_out, olen,
                                                  pdMS_TO_TICKS(5000));
                                total_pcm += olen;
                            }
                            b64_len = 0;
                        }
                    }
                }
            }
        } else if (rd == 0) {
            if (esp_http_client_is_complete_data_received(ctx->client)) break;
        } else {
            break;
        }
    }

    ctx->total_pcm = total_pcm;
    free(read_buf);
    ctx->done = true;
    vTaskDelete(NULL);
}

/* ---------- Main API ---------- */
esp_err_t tts_speak(const char *text, i2s_chan_handle_t tx_handle)
{
    esp_err_t ret = ESP_FAIL;
    esp_http_client_handle_t client = NULL;
    char *body = NULL;
    StreamBufferHandle_t sbuf = NULL;

    ESP_LOGI(TAG, "TTS: \"%s\"", text);

    /* ---- Build URL ---- */
    char url[256];
    snprintf(url, sizeof(url), "%s%s", TTS_URL, CONFIG_GEMINI_API_KEY);

    /* ---- Build JSON body ---- */
    size_t text_len = strlen(text);
    size_t body_sz = text_len * 2 + 512;   /* worst-case escaped + JSON wrapper */
    body = malloc(body_sz);
    if (!body) { ret = ESP_ERR_NO_MEM; goto done; }

    /* Escape text for JSON string */
    char *escaped = malloc(text_len * 2 + 1);
    if (!escaped) { ret = ESP_ERR_NO_MEM; goto done; }
    json_escape(text, escaped, text_len * 2 + 1);

    int body_len = snprintf(body, body_sz,
        "{\"contents\":[{\"parts\":[{\"text\":\"%s\"}]}],"
        "\"generationConfig\":{"
        "\"responseModalities\":[\"AUDIO\"],"
        "\"speechConfig\":{\"voiceConfig\":{\"prebuiltVoiceConfig\":"
        "{\"voiceName\":\"%s\"}}}}}",
        escaped, TTS_VOICE);
    free(escaped);
    escaped = NULL;

    ESP_LOGI(TAG, "Request body: %d bytes", body_len);

    /* ---- HTTP client ---- */
    esp_http_client_config_t cfg = {
        .url              = url,
        .method           = HTTP_METHOD_POST,
        .timeout_ms       = 60000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size      = 2048,
        .buffer_size_tx   = 2048,
    };
    client = esp_http_client_init(&cfg);
    if (!client) { ret = ESP_ERR_NO_MEM; goto done; }

    esp_http_client_set_header(client, "Content-Type", "application/json");

    /* Disable WiFi power save: modem sleep adds >100 ms packet latency */
    esp_wifi_set_ps(WIFI_PS_NONE);

    ret = esp_http_client_open(client, body_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open: %s", esp_err_to_name(ret));
        goto done;
    }

    /* ---- Write request body ---- */
    if (esp_http_client_write(client, body, body_len) < 0) {
        ESP_LOGE(TAG, "HTTP write failed");
        ret = ESP_FAIL; goto done;
    }
    free(body); body = NULL;

    /* ---- Read response headers ---- */
    int hdr_len = esp_http_client_fetch_headers(client);
    int status  = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP %d  content-length: %d", status, hdr_len);

    if (status != 200) {
        char err_buf[512];
        int rd = esp_http_client_read(client, err_buf, sizeof(err_buf) - 1);
        if (rd > 0) { err_buf[rd] = '\0'; ESP_LOGE(TAG, "Error: %.500s", err_buf); }
        ret = ESP_FAIL; goto done;
    }

    /* ---- Ring buffer + producer/consumer ---- */
    sbuf = xStreamBufferCreate(RING_SZ, 1);
    if (!sbuf) { ret = ESP_ERR_NO_MEM; goto done; }

    producer_ctx_t pctx = {
        .client    = client,
        .sbuf      = sbuf,
        .done      = false,
        .total_pcm = 0,
    };

    TaskHandle_t prod_handle = NULL;
    BaseType_t created = xTaskCreatePinnedToCore(
        tts_producer_task, "tts_prod", PRODUCER_STACK,
        &pctx, 5, &prod_handle, 0);   /* Core 0: WiFi/HTTP */
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create producer task");
        ret = ESP_ERR_NO_MEM; goto done;
    }

    /* ---- Consumer: pre-fill DMA, wait for buffer, stream to I2S ---- */
    {
        uint8_t play_buf[PLAY_CHUNK];
        /* Pre-fill DMA with silence */
        memset(play_buf, 0, PLAY_CHUNK);
        for (int k = 0; k < 4; k++) {
            size_t w;
            i2s_speaker_write(tx_handle, play_buf, PLAY_CHUNK, &w, 100);
        }

        /* Wait for ring buffer to reach pre-fill threshold */
        while (!pctx.done && xStreamBufferBytesAvailable(sbuf) < PREFILL_SZ)
            vTaskDelay(pdMS_TO_TICKS(5));

        ESP_LOGI(TAG, "Pre-fill done (%u bytes), starting playback",
                 (unsigned)xStreamBufferBytesAvailable(sbuf));

        /* Drain ring buffer → I2S */
        while (!pctx.done || xStreamBufferBytesAvailable(sbuf) > 0) {
            size_t got = xStreamBufferReceive(sbuf, play_buf, PLAY_CHUNK,
                                              pdMS_TO_TICKS(100));
            if (got > 0) {
                size_t w;
                i2s_speaker_write(tx_handle, play_buf, got, &w, 1000);
            }
        }

        size_t total_pcm = pctx.total_pcm;
        ESP_LOGI(TAG, "TTS done: %u PCM bytes (%.1f sec at 24 kHz)",
                 (unsigned)total_pcm, (float)total_pcm / 2 / 24000);

        /* Flush silence so speaker doesn't hold last sample */
        memset(play_buf, 0, PLAY_CHUNK);
        for (int i = 0; i < 8; i++) {
            size_t w;
            i2s_speaker_write(tx_handle, play_buf, PLAY_CHUNK, &w, 1000);
        }

        ret = (total_pcm > 0) ? ESP_OK : ESP_FAIL;
        if (total_pcm == 0) ESP_LOGE(TAG, "No audio data found in response");
    }

    vStreamBufferDelete(sbuf);
    sbuf = NULL;

done:
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    free(body);
    if (sbuf) vStreamBufferDelete(sbuf);
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    return ret;
}
