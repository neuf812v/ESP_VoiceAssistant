#include "tts_client.h"

#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
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
#define TTS_URL       "https://texttospeech.googleapis.com/v1/text:synthesize?key="
#define TTS_VOICE     "uk-UA-Wavenet-A"
#define TTS_LANG      "uk-UA"
#define TTS_RATE      24000

#define READ_BUF_SZ   4096
#define B64_BUF_SZ    1024          /* must be multiple of 4 */
#define PCM_BUF_SZ    (B64_BUF_SZ * 3 / 4)   /* 768 bytes = 384 samples */
#define FADE_SAMPLES  1200          /* 50 ms at 24 kHz */
#define TTS_VOLUME    75            /* percent of full volume (reduces brownout) */
#define RING_SZ       (12 * 1024)   /* ~250 ms ring buffer at 24 kHz 16-bit */
#define PREFILL_SZ    (6 * 1024)    /* start playback after ~125 ms buffered */
#define PLAY_CHUNK    768           /* consumer I2S write chunk */
#define PRODUCER_STACK 6144
#define WAV_HEADER_SZ 44            /* WAV header to skip for LINEAR16 */

typedef struct {
    const char *from;
    const char *to;
} stress_fix_t;

static const stress_fix_t STRESS_FIXES[] = {
    {"зараз", "за\xCC\x81раз"},
    {"Зараз", "За\xCC\x81раз"},
};

static bool is_word_boundary_byte(char ch)
{
    unsigned char uch = (unsigned char)ch;

    if (ch == '\0') {
        return true;
    }

    return uch < 0x80 && !isalnum(uch) && ch != '_';
}

static size_t count_stress_fix_matches(const char *text, const char *from)
{
    size_t matches = 0;
    size_t from_len = strlen(from);
    const char *p = text;

    while ((p = strstr(p, from)) != NULL) {
        char prev = (p == text) ? '\0' : p[-1];
        char next = p[from_len];

        if (is_word_boundary_byte(prev) && is_word_boundary_byte(next)) {
            matches++;
        }
        p += from_len;
    }

    return matches;
}

static char *apply_stress_fixes(const char *text)
{
    size_t out_len = strlen(text);

    for (size_t i = 0; i < sizeof(STRESS_FIXES) / sizeof(STRESS_FIXES[0]); i++) {
        size_t count = count_stress_fix_matches(text, STRESS_FIXES[i].from);
        if (count > 0) {
            out_len += count * (strlen(STRESS_FIXES[i].to) - strlen(STRESS_FIXES[i].from));
        }
    }

    char *out = malloc(out_len + 1);
    if (!out) {
        return NULL;
    }

    const char *src = text;
    char *dst = out;
    while (*src) {
        bool replaced = false;

        for (size_t i = 0; i < sizeof(STRESS_FIXES) / sizeof(STRESS_FIXES[0]); i++) {
            const char *from = STRESS_FIXES[i].from;
            const char *to = STRESS_FIXES[i].to;
            size_t from_len = strlen(from);

            if (strncmp(src, from, from_len) == 0) {
                char prev = (src == text) ? '\0' : src[-1];
                char next = src[from_len];

                if (is_word_boundary_byte(prev) && is_word_boundary_byte(next)) {
                    size_t to_len = strlen(to);
                    memcpy(dst, to, to_len);
                    dst += to_len;
                    src += from_len;
                    replaced = true;
                    break;
                }
            }
        }

        if (!replaced) {
            *dst++ = *src++;
        }
    }

    *dst = '\0';
    return out;
}

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
    int64_t                  t_start;
} producer_ctx_t;

static bool stream_buffer_send_all(StreamBufferHandle_t sbuf, const uint8_t *data,
                                   size_t len, TickType_t chunk_timeout)
{
    size_t sent_total = 0;

    while (sent_total < len) {
        size_t sent = xStreamBufferSend(sbuf, data + sent_total, len - sent_total,
                                        chunk_timeout);
        if (sent == 0) {
            return false;
        }
        sent_total += sent;
    }

    return true;
}

static esp_err_t i2s_write_all(i2s_chan_handle_t tx_handle, const void *src,
                               size_t size, uint32_t timeout_ms)
{
    const uint8_t *ptr = (const uint8_t *)src;
    size_t written_total = 0;

    while (written_total < size) {
        size_t written = 0;
        esp_err_t err = i2s_speaker_write(tx_handle, ptr + written_total,
                                          size - written_total, &written,
                                          timeout_ms);
        if (err != ESP_OK) {
            return err;
        }
        if (written == 0) {
            return ESP_ERR_TIMEOUT;
        }
        written_total += written;
    }

    return ESP_OK;
}

static void tts_producer_task(void *arg)
{
    producer_ctx_t *ctx = (producer_ctx_t *)arg;
    char *read_buf = malloc(READ_BUF_SZ);
    if (!read_buf) { ctx->done = true; vTaskDelete(NULL); return; }

    /* State machine: find "audioContent":" then stream base64 */
    enum { FIND_AUDIO, STREAM_B64 } state = FIND_AUDIO;
    char search_buf[32];
    int search_len = 0;
    static const char *PATTERN = "\"audioContent\":\"";
    static const char *PATTERN2 = "\"audioContent\": \"";
    int pat_len  = 16;
    int pat2_len = 17;

    unsigned char b64_acc[B64_BUF_SZ];
    int b64_len = 0;
    uint8_t pcm_out[PCM_BUF_SZ];
    size_t total_pcm = 0;
    size_t total_decoded = 0;  /* includes WAV header bytes */

    bool stream_done = false;
    while (!stream_done) {
        int rd = esp_http_client_read(ctx->client, read_buf, READ_BUF_SZ);
        if (rd > 0) {
            for (int i = 0; i < rd; i++) {
                char c = read_buf[i];

                if (state == FIND_AUDIO) {
                    if (search_len < (int)sizeof(search_buf) - 1) {
                        search_buf[search_len++] = c;
                    } else {
                        memmove(search_buf, search_buf + 1, search_len - 1);
                        search_buf[search_len - 1] = c;
                    }
                    search_buf[search_len] = '\0';

                    if ((search_len >= pat_len &&
                         memcmp(search_buf + search_len - pat_len,
                                PATTERN, pat_len) == 0) ||
                        (search_len >= pat2_len &&
                         memcmp(search_buf + search_len - pat2_len,
                                PATTERN2, pat2_len) == 0))
                    {
                        state = STREAM_B64;
                        b64_len = 0;
                        ESP_LOGI(TAG, "Found audioContent, streaming...");
                        ESP_LOGI(TAG, "[TIMING] Audio data found: %lld ms from start",
                                 (esp_timer_get_time() - ctx->t_start) / 1000);
                    }
                } else if (state == STREAM_B64) {
                    if (c == '"') {
                        /* End of base64 data — flush remainder */
                        if (b64_len > 0) {
                            while (b64_len % 4 != 0)
                                b64_acc[b64_len++] = '=';
                            size_t olen = 0;
                            mbedtls_base64_decode(pcm_out, sizeof(pcm_out),
                                                  &olen, b64_acc, b64_len);
                            if (olen > 0) {
                                /* Skip WAV header bytes */
                                size_t skip = 0;
                                if (total_decoded < WAV_HEADER_SZ) {
                                    skip = WAV_HEADER_SZ - total_decoded;
                                    if (skip > olen) skip = olen;
                                }
                                total_decoded += olen;
                                if (olen > skip) {
                                    uint8_t *pcm_start = pcm_out + skip;
                                    size_t pcm_len = olen - skip;
                                    int16_t *samples = (int16_t *)pcm_start;
                                    for (size_t s = 0; s < pcm_len / 2; s++) {
                                        size_t g = (total_pcm / 2) + s;
                                        int32_t v = (int32_t)samples[s] * TTS_VOLUME / 100;
                                        if (g < FADE_SAMPLES)
                                            v = v * (int32_t)g / FADE_SAMPLES;
                                        samples[s] = (int16_t)v;
                                    }
                                    if (!stream_buffer_send_all(ctx->sbuf, pcm_start,
                                                                pcm_len,
                                                                pdMS_TO_TICKS(5000))) {
                                        ESP_LOGW(TAG, "Ring buffer stalled while streaming audio");
                                        stream_done = true;
                                        break;
                                    }
                                    total_pcm += pcm_len;
                                }
                            }
                        }
                        stream_done = true;
                        break;
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
                                /* Skip WAV header bytes */
                                size_t skip = 0;
                                if (total_decoded < WAV_HEADER_SZ) {
                                    skip = WAV_HEADER_SZ - total_decoded;
                                    if (skip > olen) skip = olen;
                                }
                                total_decoded += olen;
                                if (olen > skip) {
                                    uint8_t *pcm_start = pcm_out + skip;
                                    size_t pcm_len = olen - skip;
                                    int16_t *samples = (int16_t *)pcm_start;
                                    for (size_t s = 0; s < pcm_len / 2; s++) {
                                        size_t g = (total_pcm / 2) + s;
                                        int32_t v = (int32_t)samples[s] * TTS_VOLUME / 100;
                                        if (g < FADE_SAMPLES)
                                            v = v * (int32_t)g / FADE_SAMPLES;
                                        samples[s] = (int16_t)v;
                                    }
                                    if (!stream_buffer_send_all(ctx->sbuf, pcm_start,
                                                                pcm_len,
                                                                pdMS_TO_TICKS(5000))) {
                                        ESP_LOGW(TAG, "Ring buffer stalled while streaming audio");
                                        stream_done = true;
                                        break;
                                    }
                                    total_pcm += pcm_len;
                                }
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
    char *normalized = NULL;
    StreamBufferHandle_t sbuf = NULL;

    ESP_LOGI(TAG, "TTS: \"%s\"", text);

    int64_t t_start = esp_timer_get_time();

    /* ---- Build URL ---- */
    char url[256];
    snprintf(url, sizeof(url), "%s%s", TTS_URL, CONFIG_CLOUD_TTS_API_KEY);

    /* ---- Build JSON body ---- */
    normalized = apply_stress_fixes(text);
    if (!normalized) { ret = ESP_ERR_NO_MEM; goto done; }

    size_t text_len = strlen(normalized);
    size_t body_sz = text_len * 2 + 512;
    body = malloc(body_sz);
    if (!body) { ret = ESP_ERR_NO_MEM; goto done; }

    char *escaped = malloc(text_len * 2 + 1);
    if (!escaped) { ret = ESP_ERR_NO_MEM; goto done; }
    json_escape(normalized, escaped, text_len * 2 + 1);

    int body_len = snprintf(body, body_sz,
        "{\"input\":{\"text\":\"%s\"},"
        "\"voice\":{\"languageCode\":\"%s\",\"name\":\"%s\"},"
        "\"audioConfig\":{\"audioEncoding\":\"LINEAR16\","
        "\"sampleRateHertz\":%d}}",
        escaped, TTS_LANG, TTS_VOICE, TTS_RATE);
    free(escaped);
    escaped = NULL;

    ESP_LOGI(TAG, "Request body: %d bytes", body_len);

    /* ---- HTTP client ---- */
    esp_http_client_config_t cfg = {
        .url              = url,
        .method           = HTTP_METHOD_POST,
        .timeout_ms       = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size      = 2048,
        .buffer_size_tx   = 2048,
    };
    client = esp_http_client_init(&cfg);
    if (!client) { ret = ESP_ERR_NO_MEM; goto done; }

    esp_http_client_set_header(client, "Content-Type", "application/json");

    /* Disable WiFi power save */
    esp_wifi_set_ps(WIFI_PS_NONE);

    int64_t t_connect = esp_timer_get_time();
    ret = esp_http_client_open(client, body_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open: %s", esp_err_to_name(ret));
        goto done;
    }
    ESP_LOGI(TAG, "[TIMING] TLS connect: %lld ms",
             (esp_timer_get_time() - t_connect) / 1000);

    /* ---- Write request body ---- */
    if (esp_http_client_write(client, body, body_len) < 0) {
        ESP_LOGE(TAG, "HTTP write failed");
        ret = ESP_FAIL; goto done;
    }
    free(body); body = NULL;

    /* ---- Read response headers ---- */
    int64_t t_wait = esp_timer_get_time();
    int hdr_len = esp_http_client_fetch_headers(client);
    int status  = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP %d  content-length: %d", status, hdr_len);
    ESP_LOGI(TAG, "[TIMING] Server response (TTFB): %lld ms",
             (esp_timer_get_time() - t_wait) / 1000);

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
        .t_start   = t_start,
    };

    TaskHandle_t prod_handle = NULL;
    BaseType_t created = xTaskCreatePinnedToCore(
        tts_producer_task, "tts_prod", PRODUCER_STACK,
        &pctx, 5, &prod_handle, 0);
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
            ret = i2s_write_all(tx_handle, play_buf, PLAY_CHUNK, 100);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to prefill I2S DMA: %s", esp_err_to_name(ret));
                goto done;
            }
        }

        /* Wait for ring buffer to reach pre-fill threshold */
        while (!pctx.done && xStreamBufferBytesAvailable(sbuf) < PREFILL_SZ)
            vTaskDelay(pdMS_TO_TICKS(5));

        ESP_LOGI(TAG, "Pre-fill done (%u bytes), starting playback",
                 (unsigned)xStreamBufferBytesAvailable(sbuf));
        ESP_LOGI(TAG, "[TIMING] First audio out: %lld ms from start",
                 (esp_timer_get_time() - t_start) / 1000);

        /* Drain ring buffer → I2S */
        while (!pctx.done || xStreamBufferBytesAvailable(sbuf) > 0) {
            size_t got = xStreamBufferReceive(sbuf, play_buf, PLAY_CHUNK,
                                              pdMS_TO_TICKS(100));
            if (got > 0) {
                ret = i2s_write_all(tx_handle, play_buf, got, 1000);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(ret));
                    goto done;
                }
            }
        }

        size_t total_pcm = pctx.total_pcm;
        ESP_LOGI(TAG, "TTS done: %u PCM bytes (%.1f sec at 24 kHz)",
                 (unsigned)total_pcm, (float)total_pcm / 2 / TTS_RATE);
        ESP_LOGI(TAG, "[TIMING] Total TTS: %lld ms",
                 (esp_timer_get_time() - t_start) / 1000);

        /* Flush silence */
        memset(play_buf, 0, PLAY_CHUNK);
        for (int i = 0; i < 8; i++) {
            ret = i2s_write_all(tx_handle, play_buf, PLAY_CHUNK, 1000);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to flush trailing silence: %s", esp_err_to_name(ret));
                break;
            }
        }

        ret = (total_pcm > 0) ? ESP_OK : ESP_FAIL;
        if (total_pcm == 0) ESP_LOGE(TAG, "No audio data found in response");
    }

    vStreamBufferDelete(sbuf);
    sbuf = NULL;

done:
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    free(normalized);
    free(body);
    if (sbuf) vStreamBufferDelete(sbuf);
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    return ret;
}
