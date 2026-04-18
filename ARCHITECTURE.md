# ESP32 Voice Assistant — Architecture

## Overview

Голосовий асистент на ESP32: користувач говорить у мікрофон, отримує голосову відповідь від Gemini через динамік.

```
[INMP441 Mic] → I2S RX → PCM 16kHz/16bit → base64 WAV
    → HTTPS POST → Gemini 2.5 Flash (аудіо → текст)
    → текст відповіді → HTTPS POST → Google Cloud TTS
    → base64 PCM 24kHz → ring buffer → I2S TX → [MAX98357A Speaker]
```

## Чому Gemini?

**Gemini 2.5 Flash** — приймає аудіо напряму (multimodal), не потрібен окремий STT.
**Google Cloud TTS** — генерує мовлення з тексту, нативний голос uk-UA-Wavenet-A, TTFB ~0.3-0.4с.

| Критерій | Gemini | OpenAI GPT-4o | Whisper + LLM + TTS |
|----------|--------|---------------|----------------------|
| API-виклики на запит | 2 | 3 (STT+LLM+TTS) | 3 |
| Аудіо на вході | Нативно | Нативно | Whisper окремо |
| Латентність | Низька | Середня | Висока |
| Білінг | Prepay credits | Pay-as-you-go | Залежить |

## Архітектура системи

### Компоненти

```
┌──────────────────────────────────────────────────────────┐
│                      ESP32 Firmware                       │
│                                                           │
│  ┌──────────┐  ┌───────────┐  ┌───────────────────────┐  │
│  │ i2s_mic  │  │i2s_speaker│  │    wifi_manager       │  │
│  │(INMP441) │  │(MAX98357A)│  │ (STA, auto-reconnect) │  │
│  └────┬─────┘  └─────▲─────┘  └──────────┬────────────┘  │
│       │              │                    │               │
│  ┌────▼──────────────┴────────────────────▼────────────┐  │
│  │               main.c (assistant_task)               │  │
│  │     Core 1, пріоритет 6 — оркестрація пайплайну     │  │
│  └────┬────────────────────────────────────┬───────────┘  │
│       │                                    │              │
│  ┌────▼──────────┐              ┌─────────▼───────────┐  │
│  │ gemini_client │              │    tts_client        │  │
│  │ Gemini 2.5    │              │  Cloud TTS           │  │
│  │ (аудіо→текст) │              │  (текст→мовлення)    │  │
│  └───────────────┘              │                      │  │
│                                 │  ┌────────────────┐  │  │
│                                 │  │ Producer (C0)  │  │  │
│                                 │  │ HTTP→decode→   │  │  │
│                                 │  │ ring buffer    │  │  │
│                                 │  └───────┬────────┘  │  │
│                                 │  ┌───────▼────────┐  │  │
│                                 │  │ Consumer (C1)  │  │  │
│                                 │  │ ring buf→I2S   │  │  │
│                                 │  └────────────────┘  │  │
│                                 └──────────────────────┘  │
└──────────────────────────────────────────────────────────┘
```

### Модулі

| Модуль | Файли | Статус | Опис |
|--------|-------|--------|------|
| **i2s_mic** | `i2s_mic.h/.c` | ✅ | INMP441, I2S0 RX, 16 kHz, 32→16 bit |
| **i2s_speaker** | `i2s_speaker.h/.c` | ✅ | MAX98357A, I2S1 TX, 16/24 kHz, DMA 8×480 |
| **wifi_manager** | `wifi_manager.h/.c` | ✅ | WiFi STA, max 10 retries, auto-reconnect |
| **gemini_client** | `gemini_client.h/.c` | ✅ | Стрімінг base64 WAV, thinking budget 256, maxOutputTokens 8192 |
| **tts_client** | `tts_client.h/.c` | ✅ | Google Cloud TTS, uk-UA-Wavenet-A, ring buffer 12KB, producer/consumer, fade-in |
| **main** | `main.c` | ✅ | 7 тестових режимів + assistant pipeline, NTP, геолокація, погода |

## Data Flow (потік даних)

### 1. Запис голосу
```
INMP441 → I2S0 RX (32-bit, 16kHz, mono) → зсув >>16 → 16-bit PCM → буфер в RAM
```
- Тривалість: до 3 сек (96KB при 16kHz/16bit)
- Тригер: **BOOT кнопка** (GPIO0) — натиснув→запис, відпустив→відправка
- DC offset removal після запису

### 2. Контекст реального часу (main.c, при старті + перед кожним запитом)
```
app_main:
  NTP sync (pool.ntp.org) → TZ "EET-2EEST" (Kyiv)
  ip-api.com → місто, країна, lat/lon
  Open-Meteo → температура, weather_code → опис українською

assistant_task (перед кожним запитом):
  fetch_weather() → оновлення g_weather
```

- **NTP**: `esp_sntp`, таймзона Київ, синхронізація при старті (10с таймаут)
- **Геолокація**: `ip-api.com` (HTTP, без TLS) → `g_city`, `g_country`, `g_lat`, `g_lon`
- **Погода**: Open-Meteo API (HTTP, без API key) → температура + WMO weather code → український опис
- Погода оновлюється **перед кожним запитом** до Gemini (завжди актуальна)

### 3. Відправка до Gemini (gemini_client)
```
PCM буфер → стрімінг base64 WAV (768-byte чанки) → HTTP POST → Gemini 2.5 Flash
```

- Модель: `gemini-2.5-flash`
- thinkingBudget: 256 (всередині generationConfig)
- maxOutputTokens: 8192 (повні відповіді без обмеження довжини)
- **Динамічний промпт**: Q/A формат, українською, включає поточний час, місто, країну, погоду
- Числа словами з правильними відмінками, наголоси на неоднозначних словах
- Парсинг: знаходить останній `text` (пропускає thinking)

### 4. Text-to-Speech (tts_client)
```
Текст "A:" частини → HTTP POST → Google Cloud TTS (uk-UA-Wavenet-A)
    → base64 WAV → skip 44-byte header → PCM 24kHz/16bit → ring buffer
    → consumer task → I2S TX → MAX98357A → динамік
```

- API: Google Cloud Text-to-Speech (`texttospeech.googleapis.com/v1/text:synthesize`)
- Аутентифікація: API key (Cloud Console)
- Голос: `uk-UA-Wavenet-A` (нативна українська)
- Формат: LINEAR16, 24 kHz, WAV з 44-byte заголовком (пропускається)
- Ring buffer: 12KB (~250мс) — FreeRTOS StreamBuffer
- Producer task (Core 0): HTTP read → base64 decode → skip WAV header → ring buffer
- Consumer (Core 1): ring buffer → I2S write
- Pre-fill: 6KB в ring buffer перед першим I2S write
- Volume: 75% (запобігає brownout)
- Fade-in: 1200 семплів (50мс)

### 5. Повний пайплайн (assistant_task, Core 1)
```
BOOT натиснуто → запис (до 3с) → BOOT відпущено
    → DC removal → fetch_weather() → gemini_ask()
    → витягнути "A:" → вимкнути мік I2S
    → init speaker 24kHz → 50мс delay
    → tts_speak() → deinit speaker → увімкнути мік
```
Погода оновлюється перед кожним запитом. Мікрофон вимикається під час TTS для зменшення споживання.

## Латентність та таймінги

Типовий цикл запит-відповідь (виміряно через `esp_timer`):

| Етап | Час |
|------|-----|
| Gemini STT+LLM (текст) | ~4-5 сек |
| TLS connect (Cloud TTS) | ~2.2 сек |
| **Cloud TTS TTFB** | **~0.3-0.4 сек** |
| Перший звук від відпускання кнопки | **~7-8 сек** |

**Cloud TTS vs Gemini TTS**: Перехід на Google Cloud TTS зменшив TTFB з ~10с до ~0.4с. Загальна латентність до першого звуку впала з ~15-18с до ~7-8с. Головне вузьке місце тепер — Gemini STT+LLM (~5с) і TLS handshake (~2.2с).

## Обмеження та рішення

| Обмеження | Рішення |
|-----------|---------|
| RAM 320KB, без PSRAM | Стрімінг: base64 encode/decode чанками, ring buffer 12KB |
| Flash 4MB (custom partition, ~24.8% зайнято) | Кастомна таблиця розділів: ~3.9MB під додаток, без OTA |
| Brownout при TTS + WiFi | Volume 75%, fade-in, мік вимикається під час TTS |
| I2S DMA underrun | Ring buffer + producer/consumer на різних ядрах |
| WiFi modem sleep latency | WiFi PS_NONE під час TTS |
| Brownout threshold | CONFIG_ESP_BROWNOUT_DET_LVL_SEL_5 (2.27V), USB 3.0 порт |

## Секрети та конфігурація

Секрети зберігаються окремо від основної конфігурації:

| Файл | В git | Зміст |
|------|-------|-------|
| `sdkconfig.defaults` | ✅ | Flash size 4MB, custom partitions, brownout, mbedtls |
| `secrets.defaults` | ❌ | WiFi SSID/password, Gemini API key, Cloud TTS API key |
| `secrets.defaults.example` | ✅ | Шаблон для копіювання |
| `Kconfig.projbuild` | ✅ | Menuconfig опції |

`CMakeLists.txt` завантажує обидва файли через `SDKCONFIG_DEFAULTS`.

## Тригер

Поточний: **BOOT кнопка (GPIO0)** — Push-to-Talk. Натиснув→запис, відпустив→відправка.

Можливі покращення:
- **Wake word** — потребує ESP32-S3 з PSRAM (ESP-SR) або Picovoice Porcupine
- **VAD** (Voice Activity Detection) — детекція голосу за RMS порогом
- **Зовнішня кнопка** — окремий GPIO замість BOOT

## Порядок реалізації

1. ~~WiFi Manager~~ ✅
2. ~~Gemini Client (аудіо → текст)~~ ✅
3. ~~TTS Client (текст → мовлення)~~ ✅
4. ~~Voice Assistant pipeline~~ ✅
5. ~~Ring buffer (anti-stutter)~~ ✅
6. ~~Кнопка PTT (BOOT / GPIO0)~~ ✅
7. ~~Таймінг-логи~~ ✅
8. ~~NTP + геолокація + погода в контексті Gemini~~ ✅
9. ~~Google Cloud TTS (uk-UA-Wavenet-A, TTFB ~0.4с)~~ ✅
10. LED індикатор стану
