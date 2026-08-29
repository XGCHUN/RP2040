/* BLE-only WiFi provisioning hosted by RP2350 with the C5 as controller. */
#include "driver.h"

#if ESP_HOSTED_ENABLE && ESP_HOSTED_BLE_ENABLE

#include <string.h>
#include "btstack.h"
#include "pico/async_context_poll.h"
#include "pico/btstack_run_loop_async_context.h"
#include "esp_hosted.h"
#include "esp_hosted_ble.h"
#include "esp_hosted_hci_transport.h"
#include "esp_hosted_provisioning.h"

#define PROVISIONING_SSID_MAX     64u
#define PROVISIONING_PASSWORD_MAX 32u
#define PROVISIONING_STATUS_MAX   63u

static async_context_poll_t bt_context;
static btstack_packet_callback_registration_t hci_event_callback;
static btstack_context_callback_registration_t notify_callback;
static hci_con_handle_t connection_handle = HCI_CON_HANDLE_INVALID;
static bool initialized, stack_working, notifications_enabled;
static bool notification_pending, wifi_connected;
static char staged_ssid[PROVISIONING_SSID_MAX + 1u];
static char staged_password[PROVISIONING_PASSWORD_MAX + 1u];
static char prepared_ssid[PROVISIONING_SSID_MAX + 1u];
static char prepared_password[PROVISIONING_PASSWORD_MAX + 1u];
static uint16_t staged_ssid_length, staged_password_length;
static uint16_t prepared_ssid_length, prepared_password_length;
static char status_value[PROVISIONING_STATUS_MAX + 1u] = "waiting for provisioning";

static const uint8_t advertising_data[] = {
    2, BLUETOOTH_DATA_TYPE_FLAGS, 0x06,
    17, BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_128_BIT_SERVICE_CLASS_UUIDS,
    0x90, 0x8a, 0x7b, 0x6c, 0x5d, 0x4e, 0xd8, 0xb7,
    0x32, 0x4a, 0x45, 0x5f, 0x01, 0x00, 0x7a, 0x9e,
    8, BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME,
    'g', 'r', 'b', 'l', 'H', 'A', 'L'
};

static void update_advertising(void)
{
    if(stack_working)
        gap_advertisements_enable(!wifi_connected && connection_handle == HCI_CON_HANDLE_INVALID);
}

static void notify_now(void *context)
{
    (void)context;
    notification_pending = false;
    if(connection_handle != HCI_CON_HANDLE_INVALID && notifications_enabled)
        att_server_notify(connection_handle,
                          ATT_CHARACTERISTIC_9e7a0005_5f45_4a32_b7d8_4e5d6c7b8a90_01_VALUE_HANDLE,
                          (const uint8_t *)status_value, (uint16_t)strlen(status_value));
}

static void request_status_notify(void)
{
    if(connection_handle == HCI_CON_HANDLE_INVALID || !notifications_enabled || notification_pending)
        return;
    notification_pending = true;
    notify_callback.callback = notify_now;
    notify_callback.context = NULL;
    att_server_request_to_send_notification(&notify_callback, connection_handle);
}

static uint16_t att_read_callback(hci_con_handle_t con_handle, uint16_t attribute_handle,
                                  uint16_t offset, uint8_t *buffer, uint16_t buffer_size)
{
    (void)con_handle;
    if(attribute_handle == ATT_CHARACTERISTIC_9e7a0005_5f45_4a32_b7d8_4e5d6c7b8a90_01_VALUE_HANDLE)
        return att_read_callback_handle_blob((const uint8_t *)status_value,
                                             (uint16_t)strlen(status_value), offset, buffer, buffer_size);
    return 0;
}

static int copy_write(char *dest, uint16_t dest_max, uint16_t *dest_length,
                      uint16_t offset, const uint8_t *buffer, uint16_t buffer_size)
{
    if(offset > dest_max)
        return ATT_ERROR_INVALID_OFFSET;
    if((uint32_t)offset + buffer_size > dest_max)
        return ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LENGTH;
    if(buffer_size)
        memcpy(dest + offset, buffer, buffer_size);
    *dest_length = (uint16_t)(offset + buffer_size);
    dest[*dest_length] = '\0';
    return 0;
}

static int att_write_callback(hci_con_handle_t con_handle, uint16_t attribute_handle,
                              uint16_t transaction_mode, uint16_t offset,
                              uint8_t *buffer, uint16_t buffer_size)
{
    (void)con_handle;
    (void)transaction_mode;

    if(attribute_handle == ATT_CHARACTERISTIC_9e7a0002_5f45_4a32_b7d8_4e5d6c7b8a90_01_VALUE_HANDLE)
        return copy_write(staged_ssid, PROVISIONING_SSID_MAX, &staged_ssid_length,
                          offset, buffer, buffer_size);

    if(attribute_handle == ATT_CHARACTERISTIC_9e7a0003_5f45_4a32_b7d8_4e5d6c7b8a90_01_VALUE_HANDLE)
        return copy_write(staged_password, PROVISIONING_PASSWORD_MAX, &staged_password_length,
                          offset, buffer, buffer_size);

    if(attribute_handle == ATT_CHARACTERISTIC_9e7a0005_5f45_4a32_b7d8_4e5d6c7b8a90_01_CLIENT_CONFIGURATION_HANDLE) {
        notifications_enabled = little_endian_read_16(buffer, 0) == GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION;
        return 0;
    }

    if(attribute_handle == ATT_CHARACTERISTIC_9e7a0004_5f45_4a32_b7d8_4e5d6c7b8a90_01_VALUE_HANDLE) {
        /* Apply: commit staged credentials atomically. */
        memcpy(prepared_ssid, staged_ssid, sizeof(prepared_ssid));
        memcpy(prepared_password, staged_password, sizeof(prepared_password));
        prepared_ssid_length = staged_ssid_length;
        prepared_password_length = staged_password_length;
        if(prepared_ssid_length == 0u)
            esp_hosted_ble_status_changed("ssid required");
        else if(esp_hosted_set_credentials(prepared_ssid, prepared_password))
            esp_hosted_ble_status_changed("applying");
        else
            esp_hosted_ble_status_changed("invalid credentials");
        return 0;
    }

    return 0;
}

static void hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    (void)channel;
    (void)size;
    if(packet_type != HCI_EVENT_PACKET)
        return;

    switch(hci_event_packet_get_type(packet)) {
        case BTSTACK_EVENT_STATE:
            stack_working = btstack_event_state_get_state(packet) == HCI_STATE_WORKING;
            update_advertising();
            break;

        case HCI_EVENT_DISCONNECTION_COMPLETE:
            connection_handle = HCI_CON_HANDLE_INVALID;
            notifications_enabled = false;
            notification_pending = false;
            update_advertising();
            break;

        case HCI_EVENT_LE_META:
            if(hci_event_le_meta_get_subevent_code(packet) == HCI_SUBEVENT_LE_CONNECTION_COMPLETE) {
                connection_handle = hci_subevent_le_connection_complete_get_connection_handle(packet);
                update_advertising();
            }
            break;

        default:
            break;
    }
}

static void setup_advertising(void)
{
    uint16_t interval_min = 0x0030, interval_max = 0x0060;
    bd_addr_t null_addr = {0};
    gap_advertisements_set_params(interval_min, interval_max, 0, 0, null_addr, 0x07, 0x00);
    gap_advertisements_set_data(sizeof(advertising_data), (uint8_t *)advertising_data);
}

bool esp_hosted_ble_start(void)
{
    if(initialized) {
        /* Controller was re-initialized (e.g. C5 reset): power cycle the host. */
        hci_power_control(HCI_POWER_ON);
        return true;
    }

    if(!async_context_poll_init_with_defaults(&bt_context))
        return false;

    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_async_context_get_instance(&bt_context.core));

    hci_init(esp_hosted_hci_transport_get_instance(), NULL);

    l2cap_init();
    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    sm_set_authentication_requirements(SM_AUTHREQ_BONDING);

    att_server_init(profile_data, att_read_callback, att_write_callback);

    hci_event_callback.callback = hci_packet_handler;
    hci_add_event_handler(&hci_event_callback);
    att_server_register_packet_handler(hci_packet_handler);

    setup_advertising();

    initialized = true;
    hci_power_control(HCI_POWER_ON);
    return true;
}

void esp_hosted_ble_poll(void)
{
    if(!initialized)
        return;

    esp_hosted_hci_transport_poll();
    async_context_poll(&bt_context.core);
}

void esp_hosted_ble_status_changed(const char *status)
{
    if(!status)
        return;

    strncpy(status_value, status, PROVISIONING_STATUS_MAX);
    status_value[PROVISIONING_STATUS_MAX] = '\0';

    bool connected = !strcmp(status, "connected");
    if(connected != wifi_connected) {
        wifi_connected = connected;
        update_advertising();
    }
    request_status_notify();
}

void esp_hosted_ble_controller_reset(void)
{
    esp_hosted_hci_transport_controller_reset();
    connection_handle = HCI_CON_HANDLE_INVALID;
    notifications_enabled = false;
    notification_pending = false;
    stack_working = false;
}

#endif /* ESP_HOSTED_ENABLE && ESP_HOSTED_BLE_ENABLE */
