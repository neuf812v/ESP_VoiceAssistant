# ESP32 Voice Assistant

Голосовий асистент на ESP32: говориш у мікрофон — отримуєш голосову відповідь від Google Gemini.

```
[INMP441 Mic] → ESP32 → WiFi → [Gemini 2.5 Flash] → [Gemini TTS] → ESP32 → [MAX98357A Speaker]
```

## Можливості

- Розпізнавання мови напряму через Gemini API (без окремого STT)
- Генерація мовлення через Gemini 3.1 Flash TTS Preview (24 kHz)
- Стрімінг аудіо через ring buffer (два FreeRTOS таски) — без заїкань
- Підтримка української мови
- Fade-in + регулювання гучності для запобігання brownout

## Апаратне забезпечення

| Компонент | Модель | Підключення |
|-----------|--------|-------------|
| MCU | ESP32-WROOM-32 (2MB flash, без PSRAM) | — |
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
   ```

4. Збери та проший:
   ```bash
   pio run -t upload
   ```

5. Відкрий монітор:
   ```bash
   pio device monitor
   ```

6. Натисни **ENTER** → говори 3 секунди → слухай відповідь.

### Отримання Gemini API Key

1. Відкрий https://aistudio.google.com/apikey
2. Створи API key
3. Для TTS моделі потрібен [Prepay billing](https://aistudio.google.com/billing)

## Структура проєкту

```
src/
├── main.c              — точка входу, тестові режими, пайплайн
├── i2s_mic.c/.h        — драйвер INMP441 (I2S RX, 16 kHz, 32-bit)
├── i2s_speaker.c/.h    — драйвер MAX98357A (I2S TX, 16/24 kHz)
├── wifi_manager.c/.h   — WiFi STA підключення з auto-reconnect
├── gemini_client.c/.h  — Gemini 2.5 Flash API (аудіо → текст)
├── tts_client.c/.h     — Gemini 3.1 Flash TTS (текст → мовлення)
└── Kconfig.projbuild   — menuconfig опції (WiFi, API key)
```

## Архітектура

Див. [ARCHITECTURE.md](ARCHITECTURE.md).

## Ліцензія

MIT
