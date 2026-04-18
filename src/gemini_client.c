#include "gemini_client.h"

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "mbedtls/base64.h"
#include "cJSON.h"

static const char *TAG = "gemini";

/* ---------- Configuration ---------- */
#define GEMINI_MODEL  "gemini-2.5-flash"
#define GEMINI_URL    "https://generativelanguage.googleapis.com/v1beta/models/" \
                      GEMINI_MODEL ":generateContent?key="

static const char *PROMPT_TEMPLATE =
    "Ти голосовий асистент на ESP32. Користувач говорить тобі в мікрофон. "
    "Розпізнай мовлення і ВІДПОВІДАЙ на запитання. "
    "Поточний час: %s. Місто: %s, %s. "
    "Погода зараз: %s. "
    "Не вигадуй факти. "
    "ВАЖЛИВО: Всі числа пиши СЛОВАМИ з правильними відмінками "
    "(наприклад: 'двадцять один градус', 'п'ять градусів', "
    "'о сьомій тридцять', 'дванадцята година'). "
    "Став наголоси (символ \\u0301) на словах де наголос може бути неочевидним "
    "(Бе\\u0301регове, а НЕ Берегове\\u0301). "
    "Формат:\\nQ: <транскрипція>\\nA: <повна відповідь українською>";

/* city/country/weather from main.c */
extern char g_city[64];
extern char g_country[64];
extern char g_weather[128];

/* Base64 chunk size: 768 bytes in → 1024 chars out (768 is multiple of 3) */
#define B64_IN   768
#define B64_OUT  1024

/* ---------- WAV header ---------- */
static void build_wav_header(uint8_t hdr[44], uint32_t pcm_bytes)
{
    uint32_t fsize = 36 + pcm_bytes;
    uint32_t sr = 16000, br = 32000, fmtsz = 16;
    uint16_t fmt = 1, ch = 1, bps = 16, ba = 2;

    memcpy(hdr,      "RIFF", 4);
    memcpy(hdr + 4,  &fsize, 4);
    memcpy(hdr + 8,  "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    memcpy(hdr + 16, &fmtsz, 4);
    memcpy(hdr + 20, &fmt, 2);
    memcpy(hdr + 22, &ch, 2);
    memcpy(hdr + 24, &sr, 4);
    memcpy(hdr + 28, &br, 4);
    memcpy(hdr + 32, &ba, 2);
    memcpy(hdr + 34, &bps, 2);
    memcpy(hdr + 36, "data", 4);
    memcpy(hdr + 40, &pcm_bytes, 4);
}

/* ---------- Helpers ---------- */
static inline size_t b64_enc_len(size_t n) { return ((n + 2) / 3) * 4; }

/* Read from a virtual stream: [wav_header (44 bytes)] + [pcm_data] */
static size_t wav_stream_read(const uint8_t *hdr, const uint8_t *pcm,
                              size_t wav_total, size_t offset,
                              uint8_t *dst, size_t max_len)
{
    size_t avail = wav_total - offset;
    size_t n = (avail < max_len) ? avail : max_len;
    size_t done = 0;

    /* From header (first 44 bytes) */
    if (offset < 44 && done < n) {
        size_t hdr_n = 44 - offset;
        if (hdr_n > n) hdr_n = n;
        memcpy(dst, hdr + offset, hdr_n);
        done = hdr_n;
    }
    /* From PCM data */
    if (done < n) {
        memcpy(dst + done, pcm + (offset + done - 44), n - done);
    }
    return n;
}

/* ---------- Main API ---------- */
esp_err_t gemini_ask(const int16_t *pcm_data, size_t num_samples,
                     char *response_text, size_t max_response_len)
{
    esp_err_t ret = ESP_FAIL;
    esp_http_client_handle_t client = NULL;
    char *resp_buf = NULL;

    uint32_t pcm_bytes = num_samples * 2;
    uint32_t wav_total = 44 + pcm_bytes;
    size_t   b64_total = b64_enc_len(wav_total);

    /* ---- Build URL ---- */
    char url[256];
    snprintf(url, sizeof(url), "%s%s", GEMINI_URL, CONFIG_GEMINI_API_KEY);

    /* ---- JSON wrapper parts ---- */
    static const char json_pre[] =
        "{\"contents\":[{\"parts\":["
        "{\"inline_data\":{\"mime_type\":\"audio/wav\",\"data\":\"";

    /* ---- Build dynamic prompt with current time ---- */
    char time_str[64] = "невідомо";
    time_t now = time(NULL);
    if (now > 1000000000) {  /* time is synced (after ~2001) */
        struct tm ti;
        localtime_r(&now, &ti);
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M, %A", &ti);
    }
    char prompt[1280];
    snprintf(prompt, sizeof(prompt), PROMPT_TEMPLATE,
             time_str, g_city, g_country,
             g_weather[0] ? g_weather : "невідомо");

    char json_suf[1536];
    snprintf(json_suf, sizeof(json_suf),
             "\"}},{\"text\":\"%s\"}]}],"
             "\"generationConfig\":{\"maxOutputTokens\":8192,"
             "\"thinkingConfig\":{\"thinkingBudget\":256}}}",
             prompt);

    size_t pre_len = sizeof(json_pre) - 1;   /* strlen without \0 */
    size_t suf_len = strlen(json_suf);
    int    content_len = (int)(pre_len + b64_total + suf_len);

    ESP_LOGI(TAG, "Sending %lu PCM bytes (%u samples) to Gemini…",
             (unsigned long)pcm_bytes, (unsigned)num_samples);
    ESP_LOGI(TAG, "HTTP body: %d bytes (base64: %u)", content_len, (unsigned)b64_total);

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

    ret = esp_http_client_open(client, content_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open: %s", esp_err_to_name(ret));
        goto done;
    }

    /* ---- 1) Write JSON prefix ---- */
    if (esp_http_client_write(client, json_pre, pre_len) < 0) {
        ESP_LOGE(TAG, "write prefix failed");
        ret = ESP_FAIL; goto done;
    }

    /* ---- 2) Stream base64-encoded WAV ---- */
    {
        uint8_t wav_hdr[44];
        build_wav_header(wav_hdr, pcm_bytes);

        const uint8_t *pcm_raw = (const uint8_t *)pcm_data;
        uint8_t  in_buf[B64_IN];
        unsigned char out_buf[B64_OUT + 4];
        size_t offset = 0;

        while (offset < wav_total) {
            size_t remain = wav_total - offset;
            size_t chunk  = (remain < B64_IN) ? remain : B64_IN;
            /* Align to 3 bytes (except last chunk) */
            if (remain > chunk && chunk % 3 != 0)
                chunk -= (chunk % 3);

            wav_stream_read(wav_hdr, pcm_raw, wav_total, offset, in_buf, chunk);

            size_t olen = 0;
            mbedtls_base64_encode(out_buf, sizeof(out_buf), &olen, in_buf, chunk);

            if (esp_http_client_write(client, (const char *)out_buf, olen) < 0) {
                ESP_LOGE(TAG, "write b64 chunk failed at offset %u", (unsigned)offset);
                ret = ESP_FAIL; goto done;
            }
            offset += chunk;
        }
    }

    /* ---- 3) Write JSON suffix ---- */
    if (esp_http_client_write(client, json_suf, suf_len) < 0) {
        ESP_LOGE(TAG, "write suffix failed");
        ret = ESP_FAIL; goto done;
    }

    /* ---- 4) Read response ---- */
    {
        int hdr_len = esp_http_client_fetch_headers(client);
        int status  = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP %d  content-length: %d", status, hdr_len);

        int buf_sz = 8192;
        resp_buf = malloc(buf_sz);
        if (!resp_buf) { ret = ESP_ERR_NO_MEM; goto done; }

        int total_rd = 0;
        while (total_rd < buf_sz - 1) {
            int rd = esp_http_client_read(client, resp_buf + total_rd,
                                          buf_sz - total_rd - 1);
            if (rd > 0) {
                total_rd += rd;
            } else if (rd == 0) {
                if (esp_http_client_is_complete_data_received(client)) break;
            } else {
                break;
            }
        }
        resp_buf[total_rd] = '\0';
        ESP_LOGI(TAG, "Read %d bytes of response", total_rd);

        if (status != 200) {
            ESP_LOGE(TAG, "API error (%d): %.500s", status, resp_buf);
            ret = ESP_FAIL; goto done;
        }

        /* ---- 5) Parse JSON — find the last "text" part (skip thinking) ---- */
        cJSON *root = cJSON_Parse(resp_buf);
        if (!root) {
            ESP_LOGE(TAG, "JSON parse failed. First 200 chars: %.200s", resp_buf);
            ret = ESP_FAIL; goto done;
        }

        cJSON *cands = cJSON_GetObjectItem(root, "candidates");
        cJSON *c0    = cJSON_GetArrayItem(cands, 0);
        cJSON *cont  = c0   ? cJSON_GetObjectItem(c0,   "content") : NULL;
        cJSON *parts = cont ? cJSON_GetObjectItem(cont,  "parts")  : NULL;

        /* Iterate parts in reverse — last text part is the actual answer */
        const char *found_text = NULL;
        int n_parts = cJSON_GetArraySize(parts);
        for (int i = n_parts - 1; i >= 0; i--) {
            cJSON *p = cJSON_GetArrayItem(parts, i);
            cJSON *t = p ? cJSON_GetObjectItem(p, "text") : NULL;
            if (t && cJSON_IsString(t) && t->valuestring && t->valuestring[0]) {
                found_text = t->valuestring;
                break;
            }
        }

        if (found_text) {
            strncpy(response_text, found_text, max_response_len - 1);
            response_text[max_response_len - 1] = '\0';
            ESP_LOGI(TAG, "Response (%d parts): %s", n_parts, response_text);
            ret = ESP_OK;
        } else {
            ESP_LOGE(TAG, "No text in response (%d parts)", n_parts);
            ret = ESP_FAIL;
        }
        cJSON_Delete(root);
    }

done:
    free(resp_buf);
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    return ret;
}
