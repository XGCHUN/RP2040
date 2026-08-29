/* BTstack HCI transport using ESP-Hosted interface 4 and header metadata. */
#include "driver.h"

#if ESP_HOSTED_ENABLE && ESP_HOSTED_BLE_ENABLE

#include <stddef.h>
#include "hci.h"
#include "esp_hosted_transport.h"
#include "esp_hosted_hci_transport.h"

static void (*packet_handler)(uint8_t packet_type, uint8_t *packet, uint16_t size);
static bool opened;

static void transport_init(const void *config)
{
    (void)config;
}

static int transport_open(void)
{
    opened = true;
    return 0;
}

static int transport_close(void)
{
    opened = false;
    esp_hosted_transport_hci_reset();
    return 0;
}

static void transport_register_packet_handler(
    void (*handler)(uint8_t packet_type, uint8_t *packet, uint16_t size))
{
    packet_handler = handler;
}

static int transport_can_send_packet_now(uint8_t packet_type)
{
    (void)packet_type;
    return opened && esp_hosted_transport_ready() && esp_hosted_transport_hci_can_tx();
}

static int transport_send_packet(uint8_t packet_type, uint8_t *packet, int size)
{
    if(!opened || !packet || size <= 0 || size > (int)ESP_HOSTED_HCI_MAX)
        return -1;

    if(packet_type != HCI_COMMAND_DATA_PACKET && packet_type != HCI_ACL_DATA_PACKET)
        return -1;

    return esp_hosted_transport_hci_tx(packet_type, packet, (uint16_t)size) ? 0 : -1;
}

static void transport_set_baudrate(uint32_t baudrate)
{
    (void)baudrate;
}

static void transport_reset_link(void)
{
}

static void transport_set_sco_config(uint16_t voice_setting, int num_connections)
{
    (void)voice_setting;
    (void)num_connections;
}

static const hci_transport_t transport = {
    .name = "esp_hosted_hci",
    .init = transport_init,
    .open = transport_open,
    .close = transport_close,
    .register_packet_handler = transport_register_packet_handler,
    .can_send_packet_now = transport_can_send_packet_now,
    .send_packet = transport_send_packet,
    .set_baudrate = NULL,
    .reset_link = NULL,
    .set_sco_config = NULL
};

const hci_transport_t *esp_hosted_hci_transport_get_instance(void)
{
    return &transport;
}

void esp_hosted_hci_transport_poll(void)
{
    if(!opened || !packet_handler)
        return;

    /* Signal completion of previously accepted TX packets to BTstack. */
    uint32_t completed = esp_hosted_transport_hci_take_completed();
    while(completed--) {
        static uint8_t sent_event[] = { HCI_EVENT_TRANSPORT_PACKET_SENT, 0 };
        packet_handler(HCI_EVENT_PACKET, sent_event, sizeof(sent_event));
    }

    esp_hosted_hci_packet_t packet;
    while(esp_hosted_transport_hci_rx(&packet)) {
        uint8_t type = packet.packet_type;
        if(type == HCI_EVENT_PACKET || type == HCI_ACL_DATA_PACKET || type == HCI_SCO_DATA_PACKET)
            packet_handler(type, packet.data, packet.length);
    }
}

void esp_hosted_hci_transport_controller_reset(void)
{
    esp_hosted_transport_hci_reset();
}

#endif /* ESP_HOSTED_ENABLE && ESP_HOSTED_BLE_ENABLE */
