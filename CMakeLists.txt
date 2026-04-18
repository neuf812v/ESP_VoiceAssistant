cmake_minimum_required(VERSION 3.16.0)

# Load secrets (WiFi, API keys) from separate gitignored file
set(SDKCONFIG_DEFAULTS
    "${CMAKE_SOURCE_DIR}/sdkconfig.defaults"
    "${CMAKE_SOURCE_DIR}/secrets.defaults"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ESP_VoiceAssistant)
