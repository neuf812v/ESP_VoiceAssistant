# ESP32 Voice Assistant

Голосовий асистент на ESP32: говориш у мікрофон — отримуєш голосову відповідь від Google Gemini.

```
[INMP441 Mic] → ESP32 → WiFi → [Gemini 2.5 Flash] → [Gemini TTS] → ESP32 → [MAX98357A Speaker]
```

## Можливості

- Розпізнавання мови напряму через Gemini API (без окремого STT)
- Генерація мовлення через Gemini 3.1 Flash TTS Preview (24 kHz)
- Стрімінг аудіо через ring buffer (два FreeRTOS таски) — без заїкань
- **BOOT кнопка (GPIO0) як Push-to-Talk**: натиснув → говориш, відпустив → відповідь
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

7. Тримай **BOOT кнопку** → говори → відпусти → слухай відповідь.

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

## Латентність

Типовий час від відпускання кнопки до першого звуку: **~15-18 секунд**.

| Етап | Час |
|------|-----|
| Gemini STT+LLM | ~5-6с |
| TLS connect (TTS) | ~2.2с |
| TTS синтез на сервері | ~8-13с |

Gemini TTS синтезує весь аудіофайл цілком перед відправкою — це головне вузьке місце.

## Перехід на Google Cloud TTS

Для значного зменшення латентності TTS (з ~10с до ~0.5-1.5с TTFB) можна перейти на [Google Cloud Text-to-Speech API](https://cloud.google.com/text-to-speech).

**Почему не зроблено зараз**: Cloud TTS не підтримує аутентифікацію через API ключ — вимагає **OAuth2 access token**. На відміну від Gemini API, який працює з простим `?key=`, Cloud TTS повертає `401 UNAUTHENTICATED` для API ключів.

**Що потрібно для реалізації**:

1. **Service Account** в Google Cloud Console (з роллю `Cloud Text-to-Speech User`)
2. **JWT підпис** на ESP32 через mbedtls (RS256 або ES256)
3. **Token refresh** кожні 60 хвилин
4. **~5-10KB додаткового flash** (при поточних 94.6% це критично)

**Алгоритм OAuth2 на ESP32**:
```
1. Зберігаємо Service Account JSON (private key) в NVS або файлі
2. Створюємо JWT: {"iss": sa_email, "scope": "...", "aud": "...", "exp": now+3600}
3. Підписуємо RS256 через mbedtls_pk_sign()
4. POST https://oauth2.googleapis.com/token (обмін JWT на access_token)
5. Використовуємо: Authorization: Bearer <access_token>
6. Оновлюємо за 50 хв до закінчення
```

**Cloud TTS API формат**:
```json
POST https://texttospeech.googleapis.com/v1/text:synthesize
{
  "input": {"text": "Текст для озвучення"},
  "voice": {"languageCode": "uk-UA", "name": "uk-UA-Wavenet-A"},
  "audioConfig": {"audioEncoding": "LINEAR16", "sampleRateHertz": 24000}
}
→ {"audioContent": "<base64 WAV з 44-byte заголовком>"}
```

Відповідь містить WAV з заголовком (44 байти), який треба пропустити перед подачею на I2S.

## Ліцензія

MIT
