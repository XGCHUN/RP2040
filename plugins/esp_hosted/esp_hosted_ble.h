/* RP2350-side BLE WiFi provisioning over ESP-Hosted HCI. */
#pragma once

#include <stdbool.h>

bool esp_hosted_ble_start(void);
void esp_hosted_ble_poll(void);
void esp_hosted_ble_status_changed(const char *status);
void esp_hosted_ble_controller_reset(void);
