/* BTstack HCI transport over the ESP-Hosted HCI interface. */
#pragma once

#include "hci_transport.h"

const hci_transport_t *esp_hosted_hci_transport_get_instance(void);
void esp_hosted_hci_transport_poll(void);
void esp_hosted_hci_transport_controller_reset(void);
