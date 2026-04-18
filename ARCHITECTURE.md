# ESP32 Voice Assistant — Architecture

## Overview

Голосовий асистент на ESP32: користувач говорить у мікрофон, отримує голосову відповідь від Gemini через динамік.

```
[INMP441 Mic] → I2S RX → PCM 16kHz/16bit → base64 WAV
    → HTTPS POST → Gemini 2.5 Flash (аудіо → текст)
    → текст відповіді → HTTPS POST → Gemini 3.1 Flash TTS
    → base64 PCM 24kHz → ring buffer → I2S TX → [MAX98357A Speaker]
```

## Чому Gemini?

**Gemini 2.5 Flash** — приймає аудіо напряму (multimodal), не потрібен окремий STT.
**Gemini 3.1 Flash TTS Preview** — генерує мовлення з тексту, підтримує українську.

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
│  │ Gemini 2.5    │              │  Gemini 3.1 TTS      │  │
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
| **tts_client** | `tts_client.h/.c` | ✅ | Ring buffer (12KB), producer/consumer, fade-in, SSE streaming |
| **main** | `main.c` | ✅ | 7 тестових режимів + assistant pipeline |

## Data Flow (потік даних)

### 1. Запис голосу
```
INMP441 → I2S0 RX (32-bit, 16kHz, mono) → зсув >>16 → 16-bit PCM → буфер в RAM
```
- Тривалість: до 3 сек (96KB при 16kHz/16bit)
- Тригер: **BOOT кнопка** (GPIO0) — натиснув→запис, відпустив→відправка
- DC offset removal після запису

### 2. Відправка до Gemini (gemini_client)
```
PCM буфер → стрімінг base64 WAV (768-byte чанки) → HTTP POST → Gemini 2.5 Flash
```

- Модель: `gemini-2.5-flash`
- thinkingBudget: 256 (всередині generationConfig)
- maxOutputTokens: 8192 (повні відповіді без обмеження довжини)
- Промпт: Q/A формат, повна відповідь українською
- Парсинг: знаходить останній `text` (пропускає thinking)

### 3. Text-to-Speech (tts_client)
```
Текст "A:" частини → HTTP POST → Gemini 3.1 Flash TTS Preview
    → стрімінг base64 → decode → PCM 24kHz/16bit → ring buffer
    → consumer task → I2S TX → MAX98357A → динамік
```

- Модель: `gemini-3.1-flash-tts-preview`
- Ендпоінт: `streamGenerateContent?alt=sse` (SSE streaming)
- Голос: Kore
- Ring buffer: 12KB (~250мс) — FreeRTOS StreamBuffer
- Producer task (Core 0): HTTP read → base64 decode → ring buffer
- Consumer (Core 1): ring buffer → I2S write
- Pre-fill: 6KB в ring buffer перед першим I2S write
- Volume: 75% (запобігає brownout)
- Fade-in: 1200 семплів (50мс)

### 4. Повний пайплайн (assistant_task, Core 1)
```
BOOT натиснуто → запис (до 3с) → BOOT відпущено
    → DC removal → gemini_ask()
    → витягнути "A:" → вимкнути мік I2S
    → init speaker 24kHz → 50мс delay
    → tts_speak() → deinit speaker → увімкнути мік
```
Мікрофон вимикається під час TTS для зменшення споживання.

## Латентність та таймінги

Типовий цикл запит-відповідь (виміряно через `esp_timer`):

| Етап | Час |
|------|-----|
| Gemini STT+LLM (текст) | ~5-6 сек |
| TLS connect (TTS) | ~2.2 сек |
| **TTS TTFB (серверний синтез)** | **~8-13 сек** |
| Перший звук від відпускання кнопки | ~15-18 сек |

**Головне обмеження**: Gemini TTS синтезує весь аудіофайл цілком на сервері перед відправкою. Навіть з `streamGenerateContent` (SSE) аудіо приходить одним блоком. TTFB пропорційний довжині тексту:
- Коротка відповідь (~1-2 речення): TTFB ~8 сек
- Довга відповідь (~5-6 речень): TTFB ~13 сек

## Обмеження та рішення

| Обмеження | Рішення |
|-----------|---------|
| RAM 320KB, без PSRAM | Стрімінг: base64 encode/decode чанками, ring buffer 12KB |
| Flash 2MB (92-94% зайнято) | Мінімум бібліотек, mbedtls cert bundle |
| Brownout при TTS + WiFi | Volume 75%, fade-in, мік вимикається під час TTS |
| I2S DMA underrun | Ring buffer + producer/consumer на різних ядрах |
| WiFi modem sleep latency | WiFi PS_NONE під час TTS |
| Brownout threshold | CONFIG_ESP_BROWNOUT_DET_LVL_SEL_5 (2.27V), USB 3.0 порт |

## Секрети та конфігурація

Секрети зберігаються окремо від основної конфігурації:

| Файл | В git | Зміст |
|------|-------|-------|
| `sdkconfig.defaults` | ✅ | Flash size, brownout, mbedtls |
| `secrets.defaults` | ❌ | WiFi SSID/password, API key |
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
8. LED індикатор стану
9. Google Cloud TTS (швидший TTFB, потребує OAuth2)
