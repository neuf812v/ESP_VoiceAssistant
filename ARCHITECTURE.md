# ESP32 Voice Assistant — Architecture

## Overview

Голосовий асистент на ESP32: користувач говорить у мікрофон, отримує голосову відповідь від LLM через динамік.

```
[INMP441 Mic] → ESP32 → WiFi → [Gemini API] → ESP32 → [MAX98357A Speaker]
                                     ↕
                              [Google Cloud TTS]
```

## Чому Gemini?

**Gemini 2.0 Flash** — оптимальний вибір для цього проєкту:

| Критерій | Gemini 2.0 Flash | OpenAI GPT-4o | Claude |
|----------|------------------|---------------|--------|
| Безкоштовний рівень | ✅ 15 RPM, 1M токенів/хв | ❌ Платний | ❌ Платний |
| Аудіо на вході | ✅ Нативно (multimodal) | ✅ Нативно | ❌ Тільки текст |
| Швидкість відповіді | ~1-2 сек | ~2-3 сек | ~2-3 сек |
| Якість відповідей | Відмінна | Відмінна | Відмінна |
| Простота API | REST + JSON | REST + JSON | REST + JSON |

**Ключова перевага Gemini**: приймає аудіо напряму (base64 PCM/WAV), тому **не потрібен окремий STT сервіс**. Це спрощує архітектуру і зменшує латентність.

**Альтернативи, якщо Gemini не підходить:**
- **OpenAI GPT-4o** — також multimodal, але платний з першого запиту
- **Традиційний пайплайн**: Whisper (STT) → будь-який LLM → TTS — більше API-викликів, більша затримка

## Архітектура системи

### Компоненти

```
┌─────────────────────────────────────────────────────┐
│                     ESP32 Firmware                   │
│                                                      │
│  ┌──────────┐  ┌──────────┐  ┌───────────────────┐  │
│  │ i2s_mic  │  │i2s_speaker│  │   wifi_manager    │  │
│  │ (INMP441)│  │(MAX98357A)│  │ (STA connection)  │  │
│  └────┬─────┘  └─────▲────┘  └────────┬──────────┘  │
│       │              │                │              │
│  ┌────▼──────────────┴────────────────▼──────────┐  │
│  │              voice_assistant                    │  │
│  │  (головний модуль-оркестратор)                  │  │
│  └────┬───────────────────────────────┬──────────┘  │
│       │                               │              │
│  ┌────▼─────────┐           ┌────────▼───────────┐  │
│  │ gemini_client│           │    tts_client       │  │
│  │ (HTTP POST)  │           │  (Google Cloud TTS) │  │
│  └──────────────┘           └────────────────────┘  │
│                                                      │
│  ┌──────────────────────────────────────────────┐   │
│  │              button / trigger                  │   │
│  │         (GPIO кнопка для PTT)                  │   │
│  └──────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────┘
```

### Модулі

| Модуль | Файли | Опис |
|--------|-------|------|
| **i2s_mic** | `i2s_mic.h/.c` | Драйвер INMP441 (вже готовий) |
| **i2s_speaker** | `i2s_speaker.h/.c` | Драйвер MAX98357A (вже готовий) |
| **wifi_manager** | `wifi_manager.h/.c` | WiFi STA підключення, reconnect |
| **gemini_client** | `gemini_client.h/.c` | HTTP POST до Gemini API з аудіо, отримання тексту |
| **tts_client** | `tts_client.h/.c` | HTTP POST до Google Cloud TTS, отримання PCM аудіо |
| **audio_codec** | `audio_codec.h/.c` | WAV header builder, MP3 decode (якщо потрібно) |
| **voice_assistant** | `voice_assistant.h/.c` | Оркестратор: запис → Gemini → TTS → програвання |
| **main** | `main.c` | Ініціалізація, кнопка, запуск |

## Data Flow (потік даних)

### 1. Запис голосу
```
INMP441 → I2S RX (32-bit, 16kHz) → конвертація в 16-bit PCM → буфер в RAM
```
- Макс тривалість: 4 сек (128KB при 16kHz/16bit)
- Кнопка PTT (Push-To-Talk): натиснув → запис, відпустив → зупинка

### 2. Відправка до Gemini
```
PCM буфер → base64 encode → HTTP POST JSON → Gemini API
```

**Gemini API Request:**
```json
{
  "contents": [{
    "parts": [
      {
        "inline_data": {
          "mime_type": "audio/pcm;rate=16000",
          "data": "<base64 encoded PCM>"
        }
      },
      {
        "text": "Ти голосовий асистент. Відповідай коротко і українською."
      }
    ]
  }],
  "generationConfig": {
    "maxOutputTokens": 200
  }
}
```

**Розмір запиту**: 4 сек PCM = 128KB → base64 ≈ 171KB
**Обмеження пам'яті**: Потрібно відправляти чанками (chunked HTTP) або використовувати streaming.

### 3. Отримання відповіді від Gemini
```
Gemini API → JSON response → парсинг тексту відповіді
```
- Відповідь: текст (кілька речень)
- Парсинг: мінімальний JSON-парсер або cJSON (є в ESP-IDF)

### 4. Text-to-Speech
```
Текст відповіді → HTTP POST → Google Cloud TTS API → аудіо (LINEAR16 PCM)
```

**Google Cloud TTS Request:**
```json
{
  "input": {"text": "Відповідь від Gemini"},
  "voice": {"languageCode": "uk-UA", "name": "uk-UA-Wavenet-A"},
  "audioConfig": {
    "audioEncoding": "LINEAR16",
    "sampleRateHertz": 16000
  }
}
```

**Чому LINEAR16 (PCM)**: не потрібен MP3 декодер на ESP32, можна грати напряму.

**Альтернатива TTS**: Google Cloud TTS потребує API key і має ліміти. Безкоштовні варіанти:
- **ESP-IDF TTS** (офлайн, але тільки англійська/китайська)
- **Gemini з TTS output** (якщо API підтримує audio response)

### 5. Програвання відповіді
```
PCM аудіо з TTS → streaming через I2S TX → MAX98357A → динамік
```
- Streaming: отримати чанк → одразу грати (без буферизації всієї відповіді)
- DC offset removal + auto-gain (вже реалізовано)

## Обмеження ESP32 та рішення

| Обмеження | Рішення |
|-----------|---------|
| RAM 320KB | Streaming аудіо, не буферизувати все |
| Flash 2MB | Мінімум бібліотек, без SPIFFS |
| Brownout при гучному звуку | Auto-gain, fade-in (вже реалізовано) |
| Base64 encoding великого аудіо | Chunked HTTP transfer |
| Латентність мережі | Показувати стан LED (запис/обробка/відповідь) |
| WiFi reconnect | Auto-reconnect в wifi_manager |

## Тригер (кнопка)

Замість ENTER в серійному моніторі — **фізична кнопка PTT (Push-To-Talk)**:
- GPIO пін з pull-up
- Натиснув і тримаєш → запис
- Відпустив → відправка до Gemini
- LED індикатор стану

Альтернатива: **wake word detection** (наприклад "Hey ESP") — але це потребує додаткової бібліотеки і обчислювальних ресурсів.

## API Keys

Зберігаються в `sdkconfig` через Kconfig (menuconfig):
- `CONFIG_WIFI_SSID` / `CONFIG_WIFI_PASSWORD`
- `CONFIG_GEMINI_API_KEY`
- `CONFIG_GOOGLE_TTS_API_KEY` (якщо окремий від Gemini)

**Не хардкодити в коді!**

## Порядок реалізації

1. **WiFi Manager** — підключення до мережі
2. **Gemini Client** — відправка тексту, отримання відповіді (тест без аудіо)
3. **Gemini + аудіо** — відправка PCM, отримання текстової відповіді
4. **TTS Client** — перетворення тексту у мовлення
5. **Voice Assistant** — повний пайплайн: мікрофон → Gemini → TTS → динамік
6. **Кнопка PTT** — фізичний тригер замість серійного монітора
7. **LED індикатор** — стан (idle / recording / processing / speaking)
