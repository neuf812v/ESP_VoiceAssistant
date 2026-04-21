# ESP32 Voice Assistant

Голосовий асистент на ESP32: говориш у мікрофон — отримуєш голосову відповідь від Google Gemini.

```
[INMP441 Mic] → ESP32 → WiFi → [Gemini 2.5 Flash] → [Cloud TTS] → ESP32 → [MAX98357A Speaker]
```

## Можливості

- Розпізнавання мови напряму через Gemini API (без окремого STT)
- Генерація мовлення через Google Cloud TTS (uk-UA-Chirp3-HD-Kore, 24 kHz)
- **Природне звучання української мови**: коректні наголоси, природні паузи після пунктуації
- Стрімінг аудіо через ring buffer (два FreeRTOS таски) — без заїкань
- **BOOT кнопка (GPIO0) як Push-to-Talk**: натиснув → говориш, відпустив → відповідь
- **Контекст реального часу**: NTP час (Київ), геолокація (ip-api.com), погода (Open-Meteo)
- Gemini знає поточний час, місто та погоду — може відповідати на "Яка година?" і "Яка погода?"
- Чиста українська мова без Markdown символів в озвученні
- Статична буферизація для надійної роботи без OOM
- **Graceful fallback**: природна відповідь при таймауті API (замість мовчання)
- Fade-in + регулювання гучності для запобігання brownout

## Апаратне забезпечення

| Компонент | Модель | Підключення |
|-----------|--------|-------------|
| MCU | ESP32-WROOM-32 (4MB flash, без PSRAM) | — |
| Мікрофон | INMP441 (I2S) | WS→GPIO26, SCK→GPIO25, SD→GPIO33 |
| Підсилювач + динамік | MAX98357A (I2S) | WS→GPIO22, BCLK→GPIO14, DIN→GPIO27 |
| Плата | HW-463 NodeMCU-ESP32 | USB 3.0 порт (для стабільного живлення) |

## Швидкий старт

### Вимоги

- [PlatformIO](https://platformio.org/) (VS Code extension або CLI)
- ESP32 плата з підключеними INMP441 + MAX98357A

### Налаштування

1. Клонуй репозиторій:
   ```bash
   git clone https://github.com/neuf812v/ESP_VoiceAssistant.git
   cd ESP_VoiceAssistant
   ```

2. Створи файл секретів з шаблону:
   ```bash
   cp secrets.defaults.example secrets.defaults
   ```

3. Заповни `secrets.defaults` своїми даними:
   ```
   CONFIG_WIFI_SSID="YourSSID"
   CONFIG_WIFI_PASSWORD="YourPassword"
   CONFIG_GEMINI_API_KEY="YourGeminiAPIKey"
   CONFIG_CLOUD_TTS_API_KEY="YourCloudTTSKey"
   CONFIG_CLOUD_TTS_VOICE="uk-UA-Chirp3-HD-Kore"  # або інший доступний голос
   ```
   
   Доступні голоси для Google Cloud TTS (українська):
   - `uk-UA-Chirp3-HD-Kore` (рекомендований, сучасний)
   - Інші можна перевірити в [Google Cloud TTS voice list](https://cloud.google.com/text-to-speech/docs/voices)

4. Збери та проший:
   ```bash
   pio run -t upload
   ```

5. Відкрий монітор:
   ```bash
   pio device monitor
   ```

7. Тримай **BOOT кнопку** → говори → відпусти → слухай відповідь.

### Отримання API ключів

**Gemini API Key** (для STT + LLM):
1. Відкрий https://aistudio.google.com/apikey
2. Створи API key
3. Потрібен [Prepay billing](https://aistudio.google.com/billing)

**Cloud TTS API Key** (для озвучення):
1. Увімкни [Cloud Text-to-Speech API](https://console.cloud.google.com/apis/library/texttospeech.googleapis.com)
2. Перейди в [Credentials](https://console.cloud.google.com/apis/credentials)
3. Створи API key, обмеж до Cloud Text-to-Speech API

## Останні поліпшення (v1.1)

✅ **Коректна українська вимова**
- Видалено ненадійну ін'єкцію наголосів (U+0301) від моделі
- Наголоси тепер обробляються локально з гарантією

✅ **Природне звучання**
- SSML `<break>` паузи після пунктуації:
  - Кома: 180ms
  - Крапка з комою: 240ms
  - Двокрапка: 260ms

✅ **Чистий текст**
- Видалено Markdown символи (`*`, `_`, `#`) з озвучення
- Gemini тепер явно заборонено генерувати Markdown

✅ **Надійність**
- Перейменована на `uk-UA-Chirp3-HD-Kore` (сучасний голос, завжди доступний)
- Статична буферизація ring buffer → ніколи не буває OOM
- Graceful fallback при таймауті API (користувач чує зрозумілу відповідь)

## Структура проєкту

```
partitions.csv          — кастомна таблиця розділів (factory ~3.9MB)
src/
├── main.c              — точка входу, тестові режими, пайплайн, NTP, геолокація, погода
├── i2s_mic.c/.h        — драйвер INMP441 (I2S RX, 16 kHz, 32-bit)
├── i2s_speaker.c/.h    — драйвер MAX98357A (I2S TX, 16/24 kHz)
├── wifi_manager.c/.h   — WiFi STA підключення з auto-reconnect
├── gemini_client.c/.h  — Gemini 2.5 Flash API (аудіо → текст, динамічний промпт)
├── tts_client.c/.h     — Google Cloud TTS (текст → мовлення, SSML паузи, статична буферизація)
└── Kconfig.projbuild   — menuconfig опції (WiFi, API keys, TTS голос)
```

## Архітектура

Див. [ARCHITECTURE.md](ARCHITECTURE.md).

## Латентність

Типовий час від відпускання кнопки до першого звуку: **~7-8 секунд**.

| Етап | Час |
|------|-----|
| Gemini STT+LLM | ~4-5с |
| TLS connect (TTS) | ~2.2с |
| Cloud TTS TTFB | ~0.3-0.4с |

Головне вузьке місце — Gemini STT+LLM (~5с) і TLS handshake (~2.2с).

## Ліцензія

MIT
