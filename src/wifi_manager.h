#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize WiFi in STA mode and connect to the configured AP.
 * Blocks until connected or max retries exceeded.
 *
 * WiFi credentials are set via menuconfig:
 *   CONFIG_WIFI_SSID / CONFIG_WIFI_PASSWORD
 *
 * Returns ESP_OK on successful connection.
 */
esp_err_t wifi_manager_init(void);

/**
 * Check if WiFi is currently connected.
 */
bool wifi_manager_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_MANAGER_H
