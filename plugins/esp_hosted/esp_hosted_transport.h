/* ESP-Hosted SPI transport for Pico SDK, no RTOS. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ESP_HOSTED_FRAME_MAX   1600u
#define ESP_HOSTED_HEADER_SIZE 12u
#define ESP_HOSTED_PAYLOAD_MAX (ESP_HOSTED_FRAME_MAX - ESP_HOSTED_HEADER_SIZE)
#define ESP_HOSTED_ETH_MTU     1500u
#define ESP_HOSTED_ETH_MAX     (ESP_HOSTED_ETH_MTU + 18u)
#define ESP_HOSTED_RPC_MAX     768u
#define ESP_HOSTED_HCI_MAX     1100u

typedef struct {
    uint16_t length;
    uint8_t data[ESP_HOSTED_ETH_MAX];
} esp_hosted_packet_t;

typedef struct {
    uint16_t length;
    uint8_t data[ESP_HOSTED_RPC_MAX];
} esp_hosted_rpc_packet_t;

typedef struct {
    uint8_t packet_type;
    uint16_t length;
    uint8_t data[ESP_HOSTED_HCI_MAX];
} esp_hosted_hci_packet_t;

bool esp_hosted_transport_prepare(void);
bool esp_hosted_transport_request_start(void);
void esp_hosted_transport_core1_poll(void);

bool esp_hosted_transport_tx(const uint8_t *data, uint16_t length);
bool esp_hosted_transport_rx(esp_hosted_packet_t *packet);
bool esp_hosted_transport_rpc_tx(const uint8_t *data, uint16_t length);
bool esp_hosted_transport_rpc_rx(esp_hosted_rpc_packet_t *packet);
bool esp_hosted_transport_hci_tx(uint8_t packet_type, const uint8_t *data, uint16_t length);
bool esp_hosted_transport_hci_rx(esp_hosted_hci_packet_t *packet);
bool esp_hosted_transport_hci_can_tx(void);
uint32_t esp_hosted_transport_hci_take_completed(void);
void esp_hosted_transport_hci_reset(void);

bool esp_hosted_transport_ready(void);
bool esp_hosted_transport_faulted(void);
uint8_t esp_hosted_transport_chip_id(void);
uint32_t esp_hosted_transport_ext_caps(void);
uint32_t esp_hosted_transport_firmware_version(void);
uint32_t esp_hosted_transport_generation(void);
