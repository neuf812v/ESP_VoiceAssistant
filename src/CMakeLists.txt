idf_component_register(
    SRCS "main.c" "i2s_mic.c" "i2s_speaker.c" "wifi_manager.c" "gemini_client.c" "tts_client.c"
    INCLUDE_DIRS "."
    REQUIRES driver nvs_flash esp_wifi esp_netif esp_event esp_http_client json mbedtls
)
