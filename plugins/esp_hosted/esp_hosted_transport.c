/* ESP-Hosted V1 SPI transport. Core 1 owns this file's hardware path. */
#include "driver.h"

#if ESP_HOSTED_ENABLE

#include <string.h>
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"
#include "pico/util/queue.h"
#include "esp_hosted_config.h"
#include "esp_hosted_transport.h"

#if ESP_HOSTED_SPI_PORT == 0
#define HOSTED_SPI spi0
#else
#define HOSTED_SPI spi1
#endif

#define EH_IF_STA       1u
#define EH_IF_SERIAL    3u
#define EH_IF_HCI       4u
#define EH_IF_PRIV      5u
#define EH_IF_DUMMY     8u
#define EH_EVENT_INIT   0x22u
#define EH_CAP_CHECKSUM 0x80u
#define EH_EXT_WLAN     (1u << 4)
#define EH_CHIP_C5      0x17u
#define EH_TAG_CAP      0x11u
#define EH_TAG_CHIP     0x12u
#define EH_TAG_EXT_CAP  0x16u
#define EH_TAG_VERSION  0x17u
#define EH_TAG_RPC_VER  0x1Au

typedef struct {
    uint8_t if_type;
    uint8_t metadata;
    uint16_t length;
    uint8_t data[ESP_HOSTED_PAYLOAD_MAX];
} tx_packet_t;

typedef enum {
    Transport_Idle = 0,
    Transport_StartRequested,
    Transport_Running,
    Transport_Ready,
    Transport_Fault
} transport_state_t;

static queue_t tx_control_queue, tx_hci_queue, tx_data_queue;
static queue_t rx_data_queue, rx_rpc_queue, rx_hci_queue;
static uint32_t state = Transport_Idle, generation, hci_tx_completed;
static bool irq_pending, queues_ready, checksum_enabled = true;
static uint8_t chip_id = 0xFFu, capabilities;
static uint32_t ext_caps, firmware_version;
static uint16_t tx_sequence;
static uint8_t tx_buffer[ESP_HOSTED_FRAME_MAX];
static uint8_t rx_buffer[ESP_HOSTED_FRAME_MAX];

static inline uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void put_le16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static uint16_t checksum(const uint8_t *data, uint16_t length, bool zero_field)
{
    uint16_t sum = 0;

    for(uint16_t i = 0; i < length; i++) {
        if(!zero_field || (i != 6u && i != 7u))
            sum = (uint16_t)(sum + data[i]);
    }

    return sum;
}

static bool gpio_valid(uint pin)
{
    return pin < NUM_BANK0_GPIOS;
}

static bool pins_valid(void)
{
    return gpio_valid(ESP_HOSTED_SPI_SCK_PIN) &&
           gpio_valid(ESP_HOSTED_SPI_MOSI_PIN) &&
           gpio_valid(ESP_HOSTED_SPI_MISO_PIN) &&
           gpio_valid(ESP_HOSTED_SPI_CS_PIN) &&
           gpio_valid(ESP_HOSTED_HANDSHAKE_PIN) &&
           gpio_valid(ESP_HOSTED_DATA_READY_PIN) &&
           gpio_valid(ESP_HOSTED_RESET_PIN);
}

static bool signal_active(uint pin, uint active_level)
{
    return gpio_get(pin) == active_level;
}

static void gpio_irq(uint gpio, uint32_t events)
{
    (void)gpio;
    (void)events;
    __atomic_store_n(&irq_pending, true, __ATOMIC_RELEASE);
}

static uint16_t encode_frame(uint8_t *frame, const tx_packet_t *packet)
{
    uint16_t total = (uint16_t)(ESP_HOSTED_HEADER_SIZE + packet->length);

    memset(frame, 0, ESP_HOSTED_FRAME_MAX);
    frame[0] = packet->if_type & 0x0Fu;
    put_le16(frame + 2, packet->length);
    put_le16(frame + 4, ESP_HOSTED_HEADER_SIZE);
    put_le16(frame + 8, tx_sequence++);
    frame[11] = packet->metadata;
    if(packet->length)
        memcpy(frame + ESP_HOSTED_HEADER_SIZE, packet->data, packet->length);
    if(checksum_enabled)
        put_le16(frame + 6, checksum(frame, total, false));

    return total;
}

static void encode_dummy(uint8_t *frame)
{
    memset(frame, 0, ESP_HOSTED_FRAME_MAX);
    frame[0] = (uint8_t)(EH_IF_DUMMY | 0xF0u);
}

static bool decode_frame(const uint8_t *frame, uint8_t *if_type, uint8_t *metadata,
                         const uint8_t **payload, uint16_t *length)
{
    uint16_t len = get_le16(frame + 2);
    uint16_t offset = get_le16(frame + 4);

    if(len == 0u || (frame[0] & 0x0Fu) == EH_IF_DUMMY)
        return false;
    if(offset != ESP_HOSTED_HEADER_SIZE || (uint32_t)offset + len > ESP_HOSTED_FRAME_MAX)
        return false;

    uint16_t stored = get_le16(frame + 6);
    if(stored && checksum(frame, (uint16_t)(offset + len), true) != stored)
        return false;

    *if_type = frame[0] & 0x0Fu;
    *metadata = frame[11];
    *payload = frame + offset;
    *length = len;

    return true;
}

static bool enqueue_tx(queue_t *queue, uint8_t if_type, uint8_t metadata,
                       const uint8_t *payload, uint16_t length)
{
    if(!payload || !length || length > ESP_HOSTED_PAYLOAD_MAX)
        return false;

    tx_packet_t packet = { .if_type = if_type, .metadata = metadata, .length = length };
    memcpy(packet.data, payload, length);
    bool queued = queue_try_add(queue, &packet);
    if(queued)
        __atomic_store_n(&irq_pending, true, __ATOMIC_RELEASE);

    return queued;
}

static bool parse_init(const uint8_t *payload, uint16_t length)
{
    bool rpc_v2 = false;
    uint8_t new_chip = 0xFFu, new_capabilities = 0;
    uint32_t new_ext_caps = 0, new_version = 0;

    if(length < 2u || payload[0] != EH_EVENT_INIT || payload[1] > length - 2u)
        return false;

    const uint8_t *p = payload + 2;
    uint16_t remaining = payload[1];

    while(remaining >= 2u) {
        uint8_t tag = p[0], len = p[1];
        if((uint16_t)len + 2u > remaining)
            return false;

        switch(tag) {
            case EH_TAG_CAP:
                if(len)
                    new_capabilities = p[2];
                break;
            case EH_TAG_CHIP:
                if(len)
                    new_chip = p[2];
                break;
            case EH_TAG_EXT_CAP:
                if(len >= 4u)
                    new_ext_caps = get_le32(p + 2);
                break;
            case EH_TAG_VERSION:
                if(len >= 4u)
                    new_version = get_le32(p + 2);
                break;
            case EH_TAG_RPC_VER:
                rpc_v2 = len && p[2] == 2u;
                break;
            default:
                break;
        }

        p += len + 2u;
        remaining -= len + 2u;
    }

    uint8_t major = (uint8_t)(new_version >> 16);
    uint8_t minor = (uint8_t)(new_version >> 8);
    if(new_chip != EH_CHIP_C5 || !(new_ext_caps & EH_EXT_WLAN) ||
       major != ESP_HOSTED_PROTOCOL_MAJOR || minor != ESP_HOSTED_PROTOCOL_MINOR)
        return false;

    chip_id = new_chip;
    capabilities = new_capabilities;
    ext_caps = new_ext_caps;
    firmware_version = new_version;
    checksum_enabled = (capabilities & EH_CAP_CHECKSUM) != 0;

    uint8_t reply[20] = {
        EH_EVENT_INIT, 15,
        0x44, 1, 0,
        0x45, 1, EH_CHIP_C5,
        0x46, 1, 0,
        0x47, 1, 0,
        0x48, 1, 0
    };
    uint16_t reply_len = 17u;

    if(rpc_v2) {
        reply[1] = 18;
        reply[17] = EH_TAG_RPC_VER;
        reply[18] = 1;
        reply[19] = 2;
        reply_len = 20u;
    }

    if(!enqueue_tx(&tx_control_queue, EH_IF_PRIV, 0, reply, reply_len))
        return false;

    __atomic_add_fetch(&generation, 1u, __ATOMIC_RELEASE);
    return true;
}

static bool spi_init_core1(void)
{
    if(!pins_valid())
        return false;

    gpio_init(ESP_HOSTED_SPI_CS_PIN);
    gpio_set_dir(ESP_HOSTED_SPI_CS_PIN, GPIO_OUT);
    gpio_put(ESP_HOSTED_SPI_CS_PIN, 1);

    spi_init(HOSTED_SPI, ESP_HOSTED_SPI_CLOCK_HZ);
    spi_set_format(HOSTED_SPI, 8,
                   ESP_HOSTED_SPI_MODE & 2 ? SPI_CPOL_1 : SPI_CPOL_0,
                   ESP_HOSTED_SPI_MODE & 1 ? SPI_CPHA_1 : SPI_CPHA_0,
                   SPI_MSB_FIRST);
    gpio_set_function(ESP_HOSTED_SPI_SCK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(ESP_HOSTED_SPI_MOSI_PIN, GPIO_FUNC_SPI);
    gpio_set_function(ESP_HOSTED_SPI_MISO_PIN, GPIO_FUNC_SPI);

    gpio_init(ESP_HOSTED_HANDSHAKE_PIN);
    gpio_set_dir(ESP_HOSTED_HANDSHAKE_PIN, GPIO_IN);
    gpio_pull_down(ESP_HOSTED_HANDSHAKE_PIN);
    gpio_init(ESP_HOSTED_DATA_READY_PIN);
    gpio_set_dir(ESP_HOSTED_DATA_READY_PIN, GPIO_IN);
    gpio_pull_down(ESP_HOSTED_DATA_READY_PIN);

    uint32_t edge = ESP_HOSTED_HANDSHAKE_ACTIVE ? GPIO_IRQ_EDGE_RISE : GPIO_IRQ_EDGE_FALL;
    gpio_set_irq_enabled_with_callback(ESP_HOSTED_HANDSHAKE_PIN, edge, true, gpio_irq);
    edge = ESP_HOSTED_DATA_READY_ACTIVE ? GPIO_IRQ_EDGE_RISE : GPIO_IRQ_EDGE_FALL;
    gpio_set_irq_enabled(ESP_HOSTED_DATA_READY_PIN, edge, true);

    gpio_init(ESP_HOSTED_RESET_PIN);
    gpio_set_dir(ESP_HOSTED_RESET_PIN, GPIO_OUT);
    gpio_put(ESP_HOSTED_RESET_PIN, ESP_HOSTED_RESET_ACTIVE);
    sleep_ms(10);
    gpio_put(ESP_HOSTED_RESET_PIN, !ESP_HOSTED_RESET_ACTIVE);

    __atomic_store_n(&irq_pending, true, __ATOMIC_RELEASE);
    return true;
}

static bool next_tx(tx_packet_t *packet)
{
    if(queue_try_remove(&tx_control_queue, packet))
        return true;
    if(queue_try_remove(&tx_hci_queue, packet)) {
        __atomic_add_fetch(&hci_tx_completed, 1u, __ATOMIC_RELEASE);
        return true;
    }
    return queue_try_remove(&tx_data_queue, packet);
}

static void route_rx(uint8_t if_type, uint8_t metadata,
                     const uint8_t *payload, uint16_t length)
{
    if(if_type == EH_IF_PRIV) {
        if(parse_init(payload, length))
            __atomic_store_n(&state, Transport_Ready, __ATOMIC_RELEASE);
        else
            __atomic_store_n(&state, Transport_Fault, __ATOMIC_RELEASE);
    } else if(if_type == EH_IF_STA && length <= ESP_HOSTED_ETH_MAX) {
        esp_hosted_packet_t packet = { .length = length };
        memcpy(packet.data, payload, length);
        queue_try_add(&rx_data_queue, &packet);
    } else if(if_type == EH_IF_SERIAL && length <= ESP_HOSTED_RPC_MAX) {
        esp_hosted_rpc_packet_t packet = { .length = length };
        memcpy(packet.data, payload, length);
        queue_try_add(&rx_rpc_queue, &packet);
    } else if(if_type == EH_IF_HCI && length <= ESP_HOSTED_HCI_MAX) {
        esp_hosted_hci_packet_t packet = { .packet_type = metadata, .length = length };
        memcpy(packet.data, payload, length);
        queue_try_add(&rx_hci_queue, &packet);
    }
}

static void transact(void)
{
    tx_packet_t packet;
    if(next_tx(&packet))
        encode_frame(tx_buffer, &packet);
    else
        encode_dummy(tx_buffer);

    gpio_put(ESP_HOSTED_SPI_CS_PIN, 0);
    spi_write_read_blocking(HOSTED_SPI, tx_buffer, rx_buffer, ESP_HOSTED_FRAME_MAX);
    gpio_put(ESP_HOSTED_SPI_CS_PIN, 1);

    uint8_t if_type, metadata;
    uint16_t length;
    const uint8_t *payload;
    if(decode_frame(rx_buffer, &if_type, &metadata, &payload, &length))
        route_rx(if_type, metadata, payload, length);
}

bool esp_hosted_transport_prepare(void)
{
    if(!queues_ready) {
        queue_init(&tx_control_queue, sizeof(tx_packet_t), ESP_HOSTED_RPC_QUEUE_DEPTH);
        queue_init(&tx_hci_queue, sizeof(tx_packet_t), ESP_HOSTED_HCI_QUEUE_DEPTH);
        queue_init(&tx_data_queue, sizeof(tx_packet_t), ESP_HOSTED_TX_QUEUE_DEPTH);
        queue_init(&rx_data_queue, sizeof(esp_hosted_packet_t), ESP_HOSTED_RX_QUEUE_DEPTH);
        queue_init(&rx_rpc_queue, sizeof(esp_hosted_rpc_packet_t), ESP_HOSTED_RPC_QUEUE_DEPTH);
        queue_init(&rx_hci_queue, sizeof(esp_hosted_hci_packet_t), ESP_HOSTED_HCI_QUEUE_DEPTH);
        queues_ready = true;
    }

    return true;
}

bool esp_hosted_transport_request_start(void)
{
    if(!queues_ready)
        return false;

    uint32_t expected = Transport_Idle;
    if(__atomic_compare_exchange_n(&state, &expected, Transport_StartRequested,
                                   false, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
        return true;

    return expected == Transport_StartRequested || expected == Transport_Running ||
           expected == Transport_Ready;
}

void esp_hosted_transport_core1_poll(void)
{
    uint32_t current = __atomic_load_n(&state, __ATOMIC_ACQUIRE);

    if(current == Transport_StartRequested) {
        if(spi_init_core1())
            __atomic_store_n(&state, Transport_Running, __ATOMIC_RELEASE);
        else
            __atomic_store_n(&state, Transport_Fault, __ATOMIC_RELEASE);
        return;
    }

    if(current != Transport_Running && current != Transport_Ready)
        return;

    bool tx_pending = !queue_is_empty(&tx_control_queue) ||
                      !queue_is_empty(&tx_hci_queue) || !queue_is_empty(&tx_data_queue);
    bool pending = __atomic_load_n(&irq_pending, __ATOMIC_ACQUIRE) || tx_pending ||
                   signal_active(ESP_HOSTED_DATA_READY_PIN, ESP_HOSTED_DATA_READY_ACTIVE);

    if(pending && signal_active(ESP_HOSTED_HANDSHAKE_PIN, ESP_HOSTED_HANDSHAKE_ACTIVE)) {
        __atomic_exchange_n(&irq_pending, false, __ATOMIC_ACQ_REL);
        transact();
        if(!queue_is_empty(&tx_control_queue) || !queue_is_empty(&tx_hci_queue) ||
           !queue_is_empty(&tx_data_queue))
            __atomic_store_n(&irq_pending, true, __ATOMIC_RELEASE);
    }
}

bool esp_hosted_transport_tx(const uint8_t *data, uint16_t length)
{
    return length <= ESP_HOSTED_ETH_MAX &&
           enqueue_tx(&tx_data_queue, EH_IF_STA, 0, data, length);
}

bool esp_hosted_transport_rx(esp_hosted_packet_t *packet)
{
    return packet && queue_try_remove(&rx_data_queue, packet);
}

bool esp_hosted_transport_rpc_tx(const uint8_t *data, uint16_t length)
{
    return length <= ESP_HOSTED_RPC_MAX &&
           enqueue_tx(&tx_control_queue, EH_IF_SERIAL, 0, data, length);
}

bool esp_hosted_transport_rpc_rx(esp_hosted_rpc_packet_t *packet)
{
    return packet && queue_try_remove(&rx_rpc_queue, packet);
}

bool esp_hosted_transport_hci_tx(uint8_t packet_type, const uint8_t *data, uint16_t length)
{
    return length <= ESP_HOSTED_HCI_MAX &&
           enqueue_tx(&tx_hci_queue, EH_IF_HCI, packet_type, data, length);
}

bool esp_hosted_transport_hci_rx(esp_hosted_hci_packet_t *packet)
{
    return packet && queue_try_remove(&rx_hci_queue, packet);
}

bool esp_hosted_transport_hci_can_tx(void)
{
    return queues_ready && !queue_is_full(&tx_hci_queue);
}

uint32_t esp_hosted_transport_hci_take_completed(void)
{
    return __atomic_exchange_n(&hci_tx_completed, 0u, __ATOMIC_ACQ_REL);
}

void esp_hosted_transport_hci_reset(void)
{
    if(!queues_ready)
        return;

    esp_hosted_hci_packet_t packet;
    while(queue_try_remove(&rx_hci_queue, &packet))
        ;
    tx_packet_t tx;
    while(queue_try_remove(&tx_hci_queue, &tx))
        ;
    __atomic_store_n(&hci_tx_completed, 0u, __ATOMIC_RELEASE);
}

bool esp_hosted_transport_ready(void)
{
    return __atomic_load_n(&state, __ATOMIC_ACQUIRE) == Transport_Ready;
}

bool esp_hosted_transport_faulted(void)
{
    return __atomic_load_n(&state, __ATOMIC_ACQUIRE) == Transport_Fault;
}

uint8_t esp_hosted_transport_chip_id(void) { return chip_id; }
uint32_t esp_hosted_transport_ext_caps(void) { return ext_caps; }
uint32_t esp_hosted_transport_firmware_version(void) { return firmware_version; }
uint32_t esp_hosted_transport_generation(void)
{
    return __atomic_load_n(&generation, __ATOMIC_ACQUIRE);
}

#endif /* ESP_HOSTED_ENABLE */
