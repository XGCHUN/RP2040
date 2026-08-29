/* grblHAL ESP32-C5 ESP-Hosted WiFi/BLE plugin. */
#pragma once

#include <stdbool.h>

void esp_hosted_init(void);
bool esp_hosted_set_credentials(const char *ssid, const char *password);
const char *esp_hosted_connection_status(void);
