/* Minimal ESP-Hosted 2.12 protobuf RPC client, no protobuf-c/RTOS. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    ESPHostedRpcEvent_Init = 769,
    ESPHostedRpcEvent_StaConnected = 775,
    ESPHostedRpcEvent_StaDisconnected = 776
} esp_hosted_rpc_event_t;

typedef void (*esp_hosted_rpc_done_ptr)(bool ok, int32_t status,
                                         const uint8_t *data, uint16_t length,
                                         void *context);
typedef void (*esp_hosted_rpc_event_ptr)(esp_hosted_rpc_event_t event,
                                          uint32_t detail, void *context);

void esp_hosted_rpc_init(esp_hosted_rpc_event_ptr event_handler, void *context);
void esp_hosted_rpc_reset(void);
void esp_hosted_rpc_poll(uint32_t now_ms);
bool esp_hosted_rpc_busy(void);

bool esp_hosted_rpc_get_mac(esp_hosted_rpc_done_ptr done, void *context);
bool esp_hosted_rpc_wifi_init(esp_hosted_rpc_done_ptr done, void *context);
bool esp_hosted_rpc_wifi_set_mode(uint8_t mode, esp_hosted_rpc_done_ptr done, void *context);
bool esp_hosted_rpc_wifi_set_config(const char *ssid, const char *password,
                                    esp_hosted_rpc_done_ptr done, void *context);
bool esp_hosted_rpc_wifi_start(esp_hosted_rpc_done_ptr done, void *context);
bool esp_hosted_rpc_wifi_stop(esp_hosted_rpc_done_ptr done, void *context);
bool esp_hosted_rpc_wifi_connect(esp_hosted_rpc_done_ptr done, void *context);
bool esp_hosted_rpc_wifi_disconnect(esp_hosted_rpc_done_ptr done, void *context);
bool esp_hosted_rpc_bt_control(uint8_t command, uint8_t option,
                               esp_hosted_rpc_done_ptr done, void *context);
