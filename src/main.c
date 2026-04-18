/**
 * ESP32 Voice Assistant — Audio Hardware Test
 *
 * Test modes:
 *   1. Mic test:       reads INMP441, prints RMS level to serial
 *   2. Speaker test:   plays a 1kHz sine tone on MAX98357A
 *   3. Loopback:       mic -> speaker in real-time (with 32→16 bit conversion)
 *   4. Record & Play:  records 2s from mic, then plays back through speaker
 *
 * Change TEST_MODE below to select.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/uart.h"

#include "i2s_mic.h"
#include "i2s_speaker.h"
#include "wifi_manager.h"
#include "gemini_client.h"
#include "tts_client.h"

static const char *TAG = "main";

// ---------- Choose test mode ----------
#define TEST_MIC        1
#define TEST_SPEAKER    2
#define TEST_LOOPBACK   3
#define TEST_REC_PLAY   4
#define TEST_GEMINI     5
#define TEST_TTS        6
#define TEST_ASSISTANT  7

#define TEST_MODE       TEST_ASSISTANT
// --------------------------------------

#define SAMPLE_RATE   16000
#define BUF_LEN       128   // samples per read/write cycle
#define REC_SECONDS   3
#define REC_SAMPLES   (SAMPLE_RATE * REC_SECONDS)  // 48000 samples
// GAIN is now computed automatically based on recorded signal level

// ====== Mic test: read and print RMS ======
static void mic_test_task(void *arg)
{
    i2s_chan_handle_t rx_handle;
    ESP_ERROR_CHECK(i2s_mic_init(&rx_handle));

    int32_t buf[BUF_LEN];
    size_t bytes_read;

    while (1) {
        esp_err_t ret = i2s_mic_read(rx_handle, buf, sizeof(buf), &bytes_read, 1000);
        if (ret == ESP_OK && bytes_read > 0) {
            int samples = bytes_read / sizeof(int32_t);
            // Compute RMS (top 16 bits of 32-bit samples)
            int64_t sum_sq = 0;
            for (int i = 0; i < samples; i++) {
                int16_t s = (int16_t)(buf[i] >> 16);
                sum_sq += (int64_t)s * s;
            }
            double rms = sqrt((double)sum_sq / samples);
            ESP_LOGI(TAG, "Mic RMS: %.1f  (samples: %d)", rms, samples);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ====== Speaker test: 1 kHz sine wave ======
static void speaker_test_task(void *arg)
{
    i2s_chan_handle_t tx_handle;
    ESP_ERROR_CHECK(i2s_speaker_init(&tx_handle));

    int16_t buf[BUF_LEN];
    const double freq = 1000.0;
    const double amplitude = 8000.0;  // moderate volume
    uint32_t phase = 0;

    while (1) {
        for (int i = 0; i < BUF_LEN; i++) {
            double t = (double)(phase + i) / SAMPLE_RATE;
            buf[i] = (int16_t)(amplitude * sin(2.0 * M_PI * freq * t));
        }
        phase += BUF_LEN;

        size_t bytes_written;
        i2s_speaker_write(tx_handle, buf, sizeof(buf), &bytes_written, 1000);
    }
}

// ====== Loopback: mic -> speaker (32bit -> 16bit) ======
static void loopback_task(void *arg)
{
    i2s_chan_handle_t rx_handle;
    i2s_chan_handle_t tx_handle;
    ESP_ERROR_CHECK(i2s_mic_init(&rx_handle));
    ESP_ERROR_CHECK(i2s_speaker_init(&tx_handle));

    int32_t mic_buf[BUF_LEN];
    int16_t spk_buf[BUF_LEN];
    size_t bytes_read, bytes_written;

    ESP_LOGI(TAG, "Loopback running: speak into mic, hear through speaker");

    while (1) {
        esp_err_t ret = i2s_mic_read(rx_handle, mic_buf, sizeof(mic_buf), &bytes_read, 1000);
        if (ret == ESP_OK && bytes_read > 0) {
            int samples = bytes_read / sizeof(int32_t);
            // Convert 32-bit mic samples to 16-bit for speaker
            for (int i = 0; i < samples; i++) {
                spk_buf[i] = (int16_t)(mic_buf[i] >> 16);
            }
            i2s_speaker_write(tx_handle, spk_buf, samples * sizeof(int16_t), &bytes_written, 1000);
        }
    }
}

// Helper: check if ENTER was pressed on UART0
static bool uart_enter_pressed(void)
{
    uint8_t ch;
    int len = uart_read_bytes(UART_NUM_0, &ch, 1, 0);
    return (len > 0 && (ch == '\n' || ch == '\r'));
}

// ====== Record & Play: record until ENTER, then play back ======
static void rec_play_task(void *arg)
{
    // Install UART driver for reading console input
    uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);

    i2s_chan_handle_t rx_handle;
    i2s_chan_handle_t tx_handle;
    ESP_ERROR_CHECK(i2s_mic_init(&rx_handle));
    ESP_ERROR_CHECK(i2s_speaker_init(&tx_handle));

    ESP_LOGI(TAG, "Free heap: %lu bytes, largest block: %lu bytes",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    // Max recording: REC_SECONDS at 16kHz
    int16_t *rec_buf = (int16_t *)malloc(REC_SAMPLES * sizeof(int16_t));
    if (rec_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate recording buffer (%d bytes)", REC_SAMPLES * (int)sizeof(int16_t));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Allocated %d bytes for recording (max %ds)", REC_SAMPLES * (int)sizeof(int16_t), REC_SECONDS);

    int32_t mic_buf[BUF_LEN];
    int16_t spk_buf[BUF_LEN];

    while (1) {
        // --- Wait for ENTER to start recording ---
        ESP_LOGI(TAG, ">>> Press ENTER to start recording <<<");
        // Flush any pending UART bytes
        uart_flush_input(UART_NUM_0);
        while (!uart_enter_pressed()) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        // --- Recording phase: until ENTER pressed or buffer full ---
        uart_flush_input(UART_NUM_0);  // flush leftover \n from ENTER
        vTaskDelay(pdMS_TO_TICKS(100)); // small delay to settle
        uart_flush_input(UART_NUM_0);  // flush again
        ESP_LOGI(TAG, ">>> RECORDING — speak now! Press ENTER to stop <<<");
        size_t total_recorded = 0;
        size_t bytes_read;

        while (total_recorded < REC_SAMPLES) {
            // Check for ENTER to stop
            if (uart_enter_pressed()) {
                break;
            }

            size_t to_read = (REC_SAMPLES - total_recorded);
            if (to_read > BUF_LEN) to_read = BUF_LEN;

            esp_err_t ret = i2s_mic_read(rx_handle,
                                          mic_buf,
                                          to_read * sizeof(int32_t),
                                          &bytes_read, 100);
            if (ret == ESP_OK && bytes_read > 0) {
                int samples = bytes_read / sizeof(int32_t);
                for (int i = 0; i < samples; i++) {
                    rec_buf[total_recorded + i] = (int16_t)(mic_buf[i] >> 16);
                }
                total_recorded += samples;
            }
        }

        if (total_recorded == 0) {
            ESP_LOGW(TAG, "Nothing recorded, try again");
            continue;
        }

        float rec_duration = (float)total_recorded / SAMPLE_RATE;
        ESP_LOGI(TAG, "Recording done: %d samples (%.1f sec)", (int)total_recorded, rec_duration);

        // --- Remove DC offset and compute signal stats ---
        int64_t dc_sum = 0;
        int16_t sig_min = 32767, sig_max = -32768;
        for (size_t i = 0; i < total_recorded; i++) {
            dc_sum += rec_buf[i];
            if (rec_buf[i] < sig_min) sig_min = rec_buf[i];
            if (rec_buf[i] > sig_max) sig_max = rec_buf[i];
        }
        int16_t dc_offset = (int16_t)(dc_sum / (int64_t)total_recorded);
        ESP_LOGI(TAG, "DC offset: %d, signal min: %d, max: %d, range: %d",
                 (int)dc_offset, (int)sig_min, (int)sig_max, (int)(sig_max - sig_min));

        // --- Compute auto-gain to use 75% of full scale without clipping ---
        int16_t peak_pos = 0, peak_neg = 0;
        for (size_t i = 0; i < total_recorded; i++) {
            int16_t s = rec_buf[i] - dc_offset;
            if (s > peak_pos) peak_pos = s;
            if (s < peak_neg) peak_neg = s;
        }
        int16_t abs_peak = (peak_pos > -peak_neg) ? peak_pos : -peak_neg;
        int gain = (abs_peak > 0) ? (29000 / abs_peak) : 1;  // target 90% of 32767
        if (gain < 1) gain = 1;
        if (gain > 128) gain = 128;
        ESP_LOGI(TAG, "Auto gain: %d (abs_peak=%d after DC removal)", gain, (int)abs_peak);

        // --- Playback phase with fade-in to avoid current spike ---
        ESP_LOGI(TAG, ">>> PLAYING BACK (gain=%d) <<<", gain);
        size_t total_played = 0;
        size_t bytes_written;
        int32_t clip_count = 0;
        const int FADE_SAMPLES = 800;  // 50ms fade-in at 16kHz

        while (total_played < total_recorded) {
            size_t chunk = total_recorded - total_played;
            if (chunk > BUF_LEN) chunk = BUF_LEN;

            // Remove DC, apply gain with fade-in, clamp
            for (size_t i = 0; i < chunk; i++) {
                int32_t sample = ((int32_t)rec_buf[total_played + i] - dc_offset) * gain;
                // Fade-in for first 50ms
                size_t global_i = total_played + i;
                if (global_i < FADE_SAMPLES) {
                    sample = sample * (int32_t)global_i / FADE_SAMPLES;
                }
                if (sample > 32767) { sample = 32767; clip_count++; }
                if (sample < -32768) { sample = -32768; clip_count++; }
                spk_buf[i] = (int16_t)sample;
            }

            i2s_speaker_write(tx_handle, spk_buf, chunk * sizeof(int16_t), &bytes_written, 1000);
            total_played += chunk;
        }
        ESP_LOGI(TAG, "Clipped samples: %d / %d (%.1f%%)",
                 (int)clip_count, (int)total_recorded,
                 100.0f * clip_count / total_recorded);

        // Flush silence into DMA so MAX98357A DAC holds zero, then stop
        memset(spk_buf, 0, BUF_LEN * sizeof(int16_t));
        for (int i = 0; i < 16; i++) {
            i2s_speaker_write(tx_handle, spk_buf, BUF_LEN * sizeof(int16_t), &bytes_written, 1000);
        }
        i2s_channel_disable(tx_handle);
        i2s_channel_enable(tx_handle);

        ESP_LOGI(TAG, "Playback done.");
    }

    // never reached, but good practice
    free(rec_buf);
}

// ====== Gemini test: record → send to Gemini → print response ======
static void gemini_test_task(void *arg)
{
    uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);

    i2s_chan_handle_t rx_handle;
    ESP_ERROR_CHECK(i2s_mic_init(&rx_handle));

    ESP_LOGI(TAG, "Free heap: %lu bytes, largest block: %lu bytes",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    int16_t *rec_buf = (int16_t *)malloc(REC_SAMPLES * sizeof(int16_t));
    if (!rec_buf) {
        ESP_LOGE(TAG, "Failed to allocate recording buffer (%d bytes)",
                 REC_SAMPLES * (int)sizeof(int16_t));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Allocated %d bytes for recording (max %ds)",
             REC_SAMPLES * (int)sizeof(int16_t), REC_SECONDS);

    int32_t mic_buf[BUF_LEN];
    char response[1024];

    while (1) {
        ESP_LOGI(TAG, ">>> Press ENTER to record a question for Gemini <<<");
        uart_flush_input(UART_NUM_0);
        while (!uart_enter_pressed()) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        /* --- Record --- */
        uart_flush_input(UART_NUM_0);
        vTaskDelay(pdMS_TO_TICKS(100));
        uart_flush_input(UART_NUM_0);
        ESP_LOGI(TAG, ">>> RECORDING — speak now! Press ENTER to stop <<<");
        size_t total_recorded = 0;
        size_t bytes_read;

        while (total_recorded < REC_SAMPLES) {
            if (uart_enter_pressed()) break;

            size_t to_read = REC_SAMPLES - total_recorded;
            if (to_read > BUF_LEN) to_read = BUF_LEN;

            esp_err_t err = i2s_mic_read(rx_handle, mic_buf,
                                         to_read * sizeof(int32_t),
                                         &bytes_read, 100);
            if (err == ESP_OK && bytes_read > 0) {
                int samples = bytes_read / sizeof(int32_t);
                for (int i = 0; i < samples; i++) {
                    rec_buf[total_recorded + i] = (int16_t)(mic_buf[i] >> 16);
                }
                total_recorded += samples;
            }
        }

        if (total_recorded == 0) {
            ESP_LOGW(TAG, "Nothing recorded, try again");
            continue;
        }

        float dur = (float)total_recorded / SAMPLE_RATE;
        ESP_LOGI(TAG, "Recorded %u samples (%.1f sec)", (unsigned)total_recorded, dur);

        /* --- Remove DC offset --- */
        int64_t dc_sum = 0;
        for (size_t i = 0; i < total_recorded; i++) dc_sum += rec_buf[i];
        int16_t dc = (int16_t)(dc_sum / (int64_t)total_recorded);
        if (dc != 0) {
            for (size_t i = 0; i < total_recorded; i++) rec_buf[i] -= dc;
            ESP_LOGI(TAG, "Removed DC offset: %d", (int)dc);
        }

        /* --- Send to Gemini --- */
        ESP_LOGI(TAG, ">>> Sending to Gemini… <<<");
        esp_err_t err = gemini_ask(rec_buf, total_recorded,
                                   response, sizeof(response));
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "=== Gemini says: %s ===", response);
        } else {
            ESP_LOGE(TAG, "Gemini request failed: %s", esp_err_to_name(err));
        }
    }

    free(rec_buf);
}

// ====== TTS test: speak a hardcoded phrase through speaker ======
static void tts_test_task(void *arg)
{
    i2s_chan_handle_t tx_handle;
    ESP_ERROR_CHECK(i2s_speaker_init_rate(&tx_handle, 24000));

    const char *phrases[] = {
        "Привіт! Я голосовий асистент на ЕСП тридцять два. Як я можу вам допомогти?",
        "Сьогодні чудовий день для програмування мікроконтролерів.",
    };
    int n_phrases = sizeof(phrases) / sizeof(phrases[0]);

    for (int i = 0; i < n_phrases; i++) {
        ESP_LOGI(TAG, "TTS phrase %d: %s", i + 1, phrases[i]);
        esp_err_t err = tts_speak(phrases[i], tx_handle);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "TTS phrase %d done", i + 1);
        } else {
            ESP_LOGE(TAG, "TTS phrase %d failed: %s", i + 1, esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    i2s_speaker_deinit(tx_handle);
    ESP_LOGI(TAG, "TTS test complete");
    vTaskDelete(NULL);
}

// ====== Voice Assistant: mic → Gemini → TTS → speaker ======
static void assistant_task(void *arg)
{
    uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);

    /* Init mic (always needed) */
    i2s_chan_handle_t rx_handle;
    ESP_ERROR_CHECK(i2s_mic_init(&rx_handle));

    ESP_LOGI(TAG, "Free heap: %lu bytes, largest block: %lu bytes",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    int16_t *rec_buf = (int16_t *)malloc(REC_SAMPLES * sizeof(int16_t));
    if (!rec_buf) {
        ESP_LOGE(TAG, "Failed to allocate recording buffer");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Allocated %d bytes for recording (max %ds)",
             REC_SAMPLES * (int)sizeof(int16_t), REC_SECONDS);

    int32_t mic_buf[BUF_LEN];
    char response[1024];

    while (1) {
        ESP_LOGI(TAG, ">>> Press ENTER to ask a question <<<");
        uart_flush_input(UART_NUM_0);
        while (!uart_enter_pressed()) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        /* --- Record --- */
        uart_flush_input(UART_NUM_0);
        vTaskDelay(pdMS_TO_TICKS(100));
        uart_flush_input(UART_NUM_0);
        ESP_LOGI(TAG, ">>> RECORDING — speak now! <<<");
        size_t total_recorded = 0;
        size_t bytes_read;

        while (total_recorded < REC_SAMPLES) {
            if (uart_enter_pressed()) break;
            size_t to_read = REC_SAMPLES - total_recorded;
            if (to_read > BUF_LEN) to_read = BUF_LEN;
            esp_err_t err = i2s_mic_read(rx_handle, mic_buf,
                                         to_read * sizeof(int32_t),
                                         &bytes_read, 100);
            if (err == ESP_OK && bytes_read > 0) {
                int samples = bytes_read / sizeof(int32_t);
                for (int i = 0; i < samples; i++)
                    rec_buf[total_recorded + i] = (int16_t)(mic_buf[i] >> 16);
                total_recorded += samples;
            }
        }
        if (total_recorded == 0) { ESP_LOGW(TAG, "Nothing recorded"); continue; }

        float dur = (float)total_recorded / SAMPLE_RATE;
        ESP_LOGI(TAG, "Recorded %u samples (%.1f sec)", (unsigned)total_recorded, dur);

        /* --- DC offset removal --- */
        int64_t dc_sum = 0;
        for (size_t i = 0; i < total_recorded; i++) dc_sum += rec_buf[i];
        int16_t dc = (int16_t)(dc_sum / (int64_t)total_recorded);
        if (dc != 0)
            for (size_t i = 0; i < total_recorded; i++) rec_buf[i] -= dc;

        /* --- Gemini: speech → text --- */
        ESP_LOGI(TAG, ">>> Thinking… <<<");
        esp_err_t err = gemini_ask(rec_buf, total_recorded,
                                   response, sizeof(response));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Gemini failed: %s", esp_err_to_name(err));
            continue;
        }
        ESP_LOGI(TAG, "Gemini: %s", response);

        /* --- Extract answer text (after "A: ") --- */
        const char *answer = response;
        char *a_ptr = strstr(response, "A: ");
        if (a_ptr) {
            answer = a_ptr + 3;
        } else {
            a_ptr = strstr(response, "A:");
            if (a_ptr) answer = a_ptr + 2;
        }
        /* Trim leading whitespace */
        while (*answer == ' ' || *answer == '\n') answer++;

        if (answer[0] == '\0') {
            ESP_LOGW(TAG, "Empty answer from Gemini");
            continue;
        }

        /* --- TTS: text → speaker --- */
        ESP_LOGI(TAG, ">>> Speaking: %s <<<", answer);

        /* Disable mic I2S to reduce power draw during speaker + WiFi */
        i2s_channel_disable(rx_handle);

        i2s_chan_handle_t tx_handle;
        err = i2s_speaker_init_rate(&tx_handle, 24000);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Speaker init failed");
            i2s_channel_enable(rx_handle);
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(50));  /* let power settle */

        err = tts_speak(answer, tx_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "TTS failed: %s", esp_err_to_name(err));
        }

        i2s_speaker_deinit(tx_handle);
        i2s_channel_enable(rx_handle);  /* re-enable mic for next round */
    }

    free(rec_buf);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP32 Voice Assistant — Audio Test ===");

    /* Connect to WiFi (blocking, will retry up to 10 times) */
    wifi_manager_init();

#if TEST_MODE == TEST_MIC
    ESP_LOGI(TAG, "Mode: MIC TEST (check serial for RMS levels)");
    xTaskCreate(mic_test_task, "mic_test", 8192, NULL, 5, NULL);

#elif TEST_MODE == TEST_SPEAKER
    ESP_LOGI(TAG, "Mode: SPEAKER TEST (1kHz sine tone)");
    xTaskCreate(speaker_test_task, "spk_test", 8192, NULL, 5, NULL);

#elif TEST_MODE == TEST_LOOPBACK
    ESP_LOGI(TAG, "Mode: LOOPBACK (mic -> speaker)");
    xTaskCreate(loopback_task, "loopback", 8192, NULL, 5, NULL);

#elif TEST_MODE == TEST_REC_PLAY
    ESP_LOGI(TAG, "Mode: RECORD & PLAY (%d sec record -> playback)", REC_SECONDS);
    xTaskCreate(rec_play_task, "rec_play", 8192, NULL, 5, NULL);

#elif TEST_MODE == TEST_GEMINI
    ESP_LOGI(TAG, "Mode: GEMINI TEST (record -> Gemini API -> text)");
    xTaskCreate(gemini_test_task, "gemini", 12288, NULL, 5, NULL);

#elif TEST_MODE == TEST_TTS
    ESP_LOGI(TAG, "Mode: TTS TEST (speak phrases through speaker)");
    xTaskCreate(tts_test_task, "tts_test", 12288, NULL, 5, NULL);

#elif TEST_MODE == TEST_ASSISTANT
    ESP_LOGI(TAG, "Mode: VOICE ASSISTANT (mic -> Gemini -> TTS -> speaker)");
    xTaskCreatePinnedToCore(assistant_task, "assistant", 16384, NULL, 6, NULL, 1);

#endif
}
