/* ESP32-C5 ESP-Hosted WiFi plugin for grblHAL, bare-metal RP2350 host. */
#include "driver.h"

#if ESP_HOSTED_ENABLE

#include <stdio.h>
#include <string.h>
#include "pico/multicore.h"
#include "lwip/dhcp.h"
#include "lwip/etharp.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/timeouts.h"
#include "grbl/nvs_buffer.h"
#include "grbl/task.h"
#include "networking/networking.h"
#include "esp_hosted.h"
#include "esp_hosted_config.h"
#include "esp_hosted_rpc.h"
#include "esp_hosted_transport.h"

#if ESP_HOSTED_BLE_ENABLE
#include "esp_hosted_ble.h"
#endif

typedef struct {
    grbl_wifi_mode_t mode;
    wifi_sta_settings_t sta;
} hosted_settings_t;

typedef enum {
    RpcPhase_Idle = 0,
    RpcPhase_GetMac,
    RpcPhase_SetMode,
    RpcPhase_SetConfig,
    RpcPhase_Connect,
    RpcPhase_Disconnect,
    RpcPhase_BTInit,
    RpcPhase_BTEnable,
    RpcPhase_Ready
} rpc_phase_t;

static struct netif hosted_netif;
static hosted_settings_t wifi;
static network_services_t allowed_services, services;
static network_flags_t network_status;
static nvs_address_t nvs_address;
static networking_get_info previous_get_info;
static on_report_options_ptr previous_report_options;
static bool started, netif_created, fault_reported, apply_requested, intentional_disconnect;
#if ESP_HOSTED_BLE_ENABLE
static bool bt_controller_ready;
#endif
static uint8_t mac_address[6];
static uint8_t tx_frame[ESP_HOSTED_ETH_MAX];
static char ip_address[IP4ADDR_STRLEN_MAX], netservices[NETWORK_SERVICES_LEN];
static const char if_name[] = "wh0";
static const char *connection_status = "transport down";
static uint32_t transport_generation, reconnect_at, last_service_poll;
static rpc_phase_t rpc_phase = RpcPhase_Idle;

static inline void set_addr(char *destination, const ip4_addr_t *address)
{
    ip4addr_ntoa_r(address, destination, 16);
}

static bool get_addr(ip4_addr_t *address, const char *value)
{
    return ip4addr_aton(value, address) == 1;
}

static void status_event_out(void *data)
{
    networking.event(if_name, (network_status_t){ .value = (uint32_t)data });
}

static void status_event_publish(network_flags_t changed)
{
    task_add_immediate(status_event_out,
        (void *)((network_status_t){ .changed = changed, .flags = network_status }).value);
}

static network_info_t *get_info(const char *interface)
{
    static network_info_t info;

    if(interface == if_name || (interface && !strcmp(interface, if_name))) {
        memset(&info, 0, sizeof(info));
        info.interface = if_name;
        info.is_ethernet = false;
        info.link_up = network_status.link_up;
        info.dhcp = wifi.sta.network.ip_mode == IpMode_DHCP;
        info.wifi_mode = wifi.mode;
        memcpy(&info.status, &wifi.sta.network, sizeof(info.status));
        memcpy(info.status.ip, ip_address, sizeof(info.status.ip));
        if(info.dhcp) {
            *info.status.gateway = '\0';
            *info.status.mask = '\0';
        }
        snprintf(info.mac, sizeof(info.mac), MAC_FORMAT_STRING,
                 mac_address[0], mac_address[1], mac_address[2],
                 mac_address[3], mac_address[4], mac_address[5]);
        info.status.services = services;
#if MQTT_ENABLE
        networking_make_mqtt_clientid(info.mac, info.mqtt_client_id);
#endif
        return &info;
    }

    return previous_get_info ? previous_get_info(interface) : NULL;
}

static void report_options(bool newopt)
{
    if(previous_report_options)
        previous_report_options(newopt);

    if(newopt)
        hal.stream.write(",WIFI,ESP-HOSTED");
    else {
        network_info_t *info = get_info(if_name);
        hal.stream.write("[WIFI MAC:");
        hal.stream.write(info->mac);
        hal.stream.write("]" ASCII_EOL "[IP:");
        hal.stream.write(*info->status.ip ? info->status.ip : "-");
        hal.stream.write("]" ASCII_EOL "[WIFI STATUS:");
        hal.stream.write(connection_status);
        hal.stream.write("]" ASCII_EOL);
        report_plugin("ESP-Hosted", "2.12");
    }
}

static void poll_services(uint32_t now)
{
    if((uint32_t)(now - last_service_poll) < 10u)
        return;
    last_service_poll = now;
#if TELNET_ENABLE
    if(services.telnet)
        telnetd_poll();
#endif
#if WEBSOCKET_ENABLE
    if(services.websocket)
        websocketd_poll();
#endif
#if FTP_ENABLE
    if(services.ftp)
        ftpd_poll();
#endif
}

static void start_services(void)
{
    network_settings_t *network = &wifi.sta.network;
#if TELNET_ENABLE
    if(network->services.telnet && !services.telnet)
        services.telnet = telnetd_init(network->telnet_port);
#endif
#if WEBSOCKET_ENABLE
    if(network->services.websocket && !services.websocket)
        services.websocket = websocketd_init(network->websocket_port);
#endif
#if FTP_ENABLE
    if(network->services.ftp && !services.ftp)
        services.ftp = ftpd_init(network->ftp_port);
#endif
#if HTTP_ENABLE
    if(network->services.http && !services.http)
        services.http = httpd_init(network->http_port);
#endif
}

static void netif_status_callback(struct netif *netif)
{
    bool has_ip = !ip4_addr_isany_val(*netif_ip4_addr(netif));
    if(netif_is_up(netif) && netif_is_link_up(netif) && has_ip) {
        ip4addr_ntoa_r(netif_ip4_addr(netif), ip_address, sizeof(ip_address));
        start_services();
        if(!network_status.ip_aquired) {
            network_status.ip_aquired = On;
            status_event_publish((network_flags_t){ .ip_aquired = On });
        }
    } else if(network_status.ip_aquired) {
        *ip_address = '\0';
        network_status.ip_aquired = Off;
        status_event_publish((network_flags_t){ .ip_aquired = On });
    }
}

static err_t hosted_linkoutput(struct netif *netif, struct pbuf *p)
{
    (void)netif;
    uint16_t length = 0;

    for(struct pbuf *q = p; q; q = q->next) {
        if((uint32_t)length + q->len > sizeof(tx_frame))
            return ERR_BUF;
        memcpy(tx_frame + length, q->payload, q->len);
        length += q->len;
    }
    if(length < 60u) {
        memset(tx_frame + length, 0, 60u - length);
        length = 60u;
    }
    return esp_hosted_transport_tx(tx_frame, length) ? ERR_OK : ERR_MEM;
}

static err_t hosted_netif_init(struct netif *netif)
{
    netif->name[0] = 'w';
    netif->name[1] = 'h';
    netif->output = etharp_output;
    netif->linkoutput = hosted_linkoutput;
    netif->mtu = ESP_HOSTED_ETH_MTU;
    netif->hwaddr_len = sizeof(mac_address);
    memcpy(netif->hwaddr, mac_address, sizeof(mac_address));
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP |
                   NETIF_FLAG_ETHERNET | NETIF_FLAG_IGMP;
    return ERR_OK;
}

static bool create_netif(void)
{
    if(netif_created) {
        memcpy(hosted_netif.hwaddr, mac_address, sizeof(mac_address));
        return true;
    }

    ip4_addr_t ip, mask, gateway;
    ip4_addr_set_zero(&ip);
    ip4_addr_set_zero(&mask);
    ip4_addr_set_zero(&gateway);
    if(wifi.sta.network.ip_mode == IpMode_Static) {
        get_addr(&ip, wifi.sta.network.ip);
        get_addr(&mask, wifi.sta.network.mask);
        get_addr(&gateway, wifi.sta.network.gateway);
    }

    if(!netif_add(&hosted_netif, &ip, &mask, &gateway, NULL,
                  hosted_netif_init, netif_input))
        return false;
    netif_set_default(&hosted_netif);
    netif_set_status_callback(&hosted_netif, netif_status_callback);
#if LWIP_NETIF_HOSTNAME
    netif_set_hostname(&hosted_netif, wifi.sta.network.hostname);
#endif
    netif_set_up(&hosted_netif);
    netif_created = true;
    network_status.interface_up = On;
    status_event_publish((network_flags_t){ .interface_up = On });
    return true;
}

static void link_down(void)
{
    if(!netif_created)
        return;

    if(wifi.sta.network.ip_mode == IpMode_DHCP) {
        dhcp_stop(&hosted_netif);
        ip4_addr_t zero;
        ip4_addr_set_zero(&zero);
        netif_set_addr(&hosted_netif, &zero, &zero, &zero);
    }
    netif_set_link_down(&hosted_netif);
    *ip_address = '\0';
    network_flags_t changed = {0};
    if(network_status.link_up) {
        network_status.link_up = Off;
        changed.link_up = On;
    }
    if(network_status.ip_aquired) {
        network_status.ip_aquired = Off;
        changed.ip_aquired = On;
    }
    if(changed.value)
        status_event_publish(changed);
}

static void link_up(void)
{
    if(!netif_created || network_status.link_up)
        return;

    netif_set_link_up(&hosted_netif);
    network_status.link_up = On;
    status_event_publish((network_flags_t){ .link_up = On });
    connection_status = "connected";

    if(wifi.sta.network.ip_mode == IpMode_DHCP)
        dhcp_start(&hosted_netif);
    else
        netif_status_callback(&hosted_netif);
#if ESP_HOSTED_BLE_ENABLE
    esp_hosted_ble_status_changed("connected");
#endif
}

static void input_packets(void)
{
    esp_hosted_packet_t packet;
    while(netif_created && esp_hosted_transport_rx(&packet)) {
        struct pbuf *p = pbuf_alloc(PBUF_RAW, packet.length, PBUF_POOL);
        if(!p)
            break;
        if(pbuf_take(p, packet.data, packet.length) != ERR_OK ||
           hosted_netif.input(p, &hosted_netif) != ERR_OK)
            pbuf_free(p);
    }
}

static void begin_bt(void);

static void rpc_done(bool ok, int32_t status, const uint8_t *data,
                     uint16_t length, void *context)
{
    (void)context;
    if(!ok) {
        connection_status = "RPC failed";
        rpc_phase = RpcPhase_Ready;
        task_add_immediate(report_warning, "ESP-Hosted RPC failed; verify the pinned 2.12.x C5 firmware configuration.");
#if ESP_HOSTED_BLE_ENABLE
        esp_hosted_ble_status_changed("rpc failed");
#endif
        return;
    }

    switch(rpc_phase) {
        case RpcPhase_GetMac:
            if(length != sizeof(mac_address)) {
                rpc_done(false, -2, NULL, 0, NULL);
                return;
            }
            memcpy(mac_address, data, sizeof(mac_address));
            if(!create_netif()) {
                rpc_done(false, -3, NULL, 0, NULL);
                return;
            }
            rpc_phase = RpcPhase_SetMode;
            esp_hosted_rpc_wifi_set_mode(wifi.mode == WiFiMode_STA ? 1u : 0u, rpc_done, NULL);
            break;

        case RpcPhase_SetMode:
            if(wifi.mode == WiFiMode_STA && *wifi.sta.ssid) {
                rpc_phase = RpcPhase_SetConfig;
                connection_status = "configuring";
                esp_hosted_rpc_wifi_set_config(wifi.sta.ssid, wifi.sta.password, rpc_done, NULL);
            } else {
                connection_status = *wifi.sta.ssid ? "WiFi off" : "waiting for provisioning";
                begin_bt();
            }
            break;

        case RpcPhase_SetConfig:
            rpc_phase = RpcPhase_Connect;
            connection_status = "connecting";
            esp_hosted_rpc_wifi_connect(rpc_done, NULL);
            break;

        case RpcPhase_Connect:
            begin_bt();
            break;

        case RpcPhase_Disconnect:
            intentional_disconnect = false;
            rpc_phase = RpcPhase_SetMode;
            esp_hosted_rpc_wifi_set_mode(wifi.mode == WiFiMode_STA ? 1u : 0u, rpc_done, NULL);
            break;

#if ESP_HOSTED_BLE_ENABLE
        case RpcPhase_BTInit:
            rpc_phase = RpcPhase_BTEnable;
            esp_hosted_rpc_bt_control(3, 0, rpc_done, NULL);
            break;

        case RpcPhase_BTEnable:
            rpc_phase = RpcPhase_Ready;
            bt_controller_ready = true;
            if(!esp_hosted_ble_start())
                task_add_immediate(report_warning, "ESP-Hosted BLE provisioning failed to start.");
            break;
#endif
        default:
            rpc_phase = RpcPhase_Ready;
            break;
    }
    (void)status;
}

static void begin_bt(void)
{
#if ESP_HOSTED_BLE_ENABLE
    if(!bt_controller_ready) {
        rpc_phase = RpcPhase_BTInit;
        if(!esp_hosted_rpc_bt_control(1, 0, rpc_done, NULL))
            rpc_done(false, -4, NULL, 0, NULL);
        return;
    }
#endif
    rpc_phase = RpcPhase_Ready;
}

static void rpc_event(esp_hosted_rpc_event_t event, uint32_t detail, void *context)
{
    (void)context;
    switch(event) {
        case ESPHostedRpcEvent_StaConnected:
            reconnect_at = 0;
            link_up();
            break;

        case ESPHostedRpcEvent_StaDisconnected:
            connection_status = "disconnected";
            link_down();
#if ESP_HOSTED_BLE_ENABLE
            esp_hosted_ble_status_changed("disconnected");
#endif
            if(!intentional_disconnect && wifi.mode == WiFiMode_STA && *wifi.sta.ssid)
                reconnect_at = hal.get_elapsed_ticks() + 3000u;
            if(detail)
                task_add_immediate(report_plain, "WIFI STA DISCONNECTED");
            break;

        case ESPHostedRpcEvent_Init:
            link_down();
            break;
    }
}

static void start_configuration(void)
{
    if(esp_hosted_rpc_busy() || !esp_hosted_transport_ready())
        return;

    apply_requested = false;
    reconnect_at = 0;
    if(network_status.link_up || rpc_phase == RpcPhase_Connect || rpc_phase == RpcPhase_Ready) {
        intentional_disconnect = true;
        rpc_phase = RpcPhase_Disconnect;
        if(!esp_hosted_rpc_wifi_disconnect(rpc_done, NULL))
            rpc_done(false, -5, NULL, 0, NULL);
    } else {
        rpc_phase = RpcPhase_SetMode;
        if(!esp_hosted_rpc_wifi_set_mode(wifi.mode == WiFiMode_STA ? 1u : 0u, rpc_done, NULL))
            rpc_done(false, -6, NULL, 0, NULL);
    }
}

static void apply_settings_task(void *data)
{
    (void)data;
    apply_requested = true;
}

static void schedule_apply(void)
{
    if(started) {
        task_delete(apply_settings_task, NULL);
        task_add_delayed(apply_settings_task, NULL, 500u);
    }
}

static void core1_entry(void)
{
    while(true) {
        esp_hosted_transport_core1_poll();
        tight_loop_contents();
    }
}

static void hosted_poll(void *data)
{
    (void)data;
    if(!started)
        return;

    uint32_t now = hal.get_elapsed_ticks();
    if(esp_hosted_transport_faulted()) {
        if(!fault_reported) {
            fault_reported = true;
            connection_status = "transport failed";
            task_add_immediate(report_warning,
                "ESP-Hosted transport failed; check C5 2.12.x firmware and pin macros.");
        }
        return;
    }

    uint32_t generation = esp_hosted_transport_generation();
    if(esp_hosted_transport_ready() && generation != transport_generation) {
        transport_generation = generation;
        fault_reported = false;
#if ESP_HOSTED_BLE_ENABLE
        bt_controller_ready = false;
        esp_hosted_ble_controller_reset();
#endif
        link_down();
        esp_hosted_rpc_reset();
        rpc_phase = RpcPhase_GetMac;
        connection_status = "initializing";
        if(!esp_hosted_rpc_get_mac(rpc_done, NULL))
            rpc_done(false, -7, NULL, 0, NULL);
    }

    esp_hosted_rpc_poll(now);
    input_packets();
    sys_check_timeouts();
    poll_services(now);
#if ESP_HOSTED_BLE_ENABLE
    esp_hosted_ble_poll();
#endif

    if(apply_requested && !esp_hosted_rpc_busy())
        start_configuration();
    else if(reconnect_at && (int32_t)(now - reconnect_at) >= 0 && !esp_hosted_rpc_busy()) {
        reconnect_at = 0;
        apply_requested = true;
        start_configuration();
    }
}

static status_code_t set_ip(setting_id_t setting, char *value)
{
    ip4_addr_t address;
    if(!get_addr(&address, value))
        return Status_InvalidStatement;

    switch(setting) {
        case Setting_IpAddress3: set_addr(wifi.sta.network.ip, &address); break;
        case Setting_Gateway3: set_addr(wifi.sta.network.gateway, &address); break;
        case Setting_NetMask3: set_addr(wifi.sta.network.mask, &address); break;
        default: return Status_Unhandled;
    }
    return Status_OK;
}

static char *get_ip(setting_id_t setting)
{
    switch(setting) {
        case Setting_IpAddress3: return wifi.sta.network.ip;
        case Setting_Gateway3: return wifi.sta.network.gateway;
        case Setting_NetMask3: return wifi.sta.network.mask;
        default: return "";
    }
}

static status_code_t set_integer(setting_id_t setting, uint_fast16_t value)
{
    switch(setting) {
        case Setting_NetworkServices:
            wifi.sta.network.services.mask = (uint8_t)value & allowed_services.mask;
            break;
#if TELNET_ENABLE
        case Setting_TelnetPort3: wifi.sta.network.telnet_port = (uint16_t)value; break;
#endif
#if WEBSOCKET_ENABLE
        case Setting_WebSocketPort3: wifi.sta.network.websocket_port = (uint16_t)value; break;
#endif
#if HTTP_ENABLE
        case Setting_HttpPort3: wifi.sta.network.http_port = (uint16_t)value; break;
#endif
#if FTP_ENABLE
        case Setting_FtpPort3: wifi.sta.network.ftp_port = (uint16_t)value; break;
#endif
        default: return Status_Unhandled;
    }
    return Status_OK;
}

static uint32_t get_integer(setting_id_t setting)
{
    switch(setting) {
        case Setting_NetworkServices: return wifi.sta.network.services.mask & allowed_services.mask;
#if TELNET_ENABLE
        case Setting_TelnetPort3: return wifi.sta.network.telnet_port;
#endif
#if WEBSOCKET_ENABLE
        case Setting_WebSocketPort3: return wifi.sta.network.websocket_port;
#endif
#if HTTP_ENABLE
        case Setting_HttpPort3: return wifi.sta.network.http_port;
#endif
#if FTP_ENABLE
        case Setting_FtpPort3: return wifi.sta.network.ftp_port;
#endif
        default: return 0;
    }
}

static const setting_group_detail_t hosted_groups[] = {
    { Group_Root, Group_Networking, "Networking" },
    { Group_Networking, Group_Networking_Wifi, "WiFi" }
};

static const setting_detail_t hosted_setting_list[] = {
    { Setting_WifiMode, Group_Networking_Wifi, "WiFi Mode", NULL, Format_RadioButtons, "Off,Station", NULL, NULL, Setting_NonCore, &wifi.mode, NULL, NULL },
    { Setting_WiFi_STA_SSID, Group_Networking_Wifi, "WiFi Station (STA) SSID", NULL, Format_String, "x(64)", NULL, "64", Setting_NonCore, &wifi.sta.ssid, NULL, NULL },
    { Setting_WiFi_STA_Password, Group_Networking_Wifi, "WiFi Station (STA) Password", NULL, Format_Password, "x(32)", NULL, "32", Setting_NonCore, &wifi.sta.password, NULL, NULL, { .allow_null = On } },
    { Setting_NetworkServices, Group_Networking, "Network Services", NULL, Format_Bitfield, netservices, NULL, NULL, Setting_NonCoreFn, set_integer, get_integer, NULL },
    { Setting_Hostname3, Group_Networking, "Hostname", NULL, Format_String, "x(32)", NULL, "32", Setting_NonCore, &wifi.sta.network.hostname, NULL, NULL },
    { Setting_IpMode3, Group_Networking, "IP Mode (STA)", NULL, Format_RadioButtons, "Static,DHCP", NULL, NULL, Setting_NonCore, &wifi.sta.network.ip_mode, NULL, NULL },
    { Setting_IpAddress3, Group_Networking, "IP Address", NULL, Format_IPv4, NULL, NULL, NULL, Setting_NonCoreFn, set_ip, get_ip, NULL },
    { Setting_Gateway3, Group_Networking, "Gateway", NULL, Format_IPv4, NULL, NULL, NULL, Setting_NonCoreFn, set_ip, get_ip, NULL },
    { Setting_NetMask3, Group_Networking, "Netmask", NULL, Format_IPv4, NULL, NULL, NULL, Setting_NonCoreFn, set_ip, get_ip, NULL },
#if TELNET_ENABLE
    { Setting_TelnetPort3, Group_Networking, "Telnet port", NULL, Format_Integer, "####0", "1", "65535", Setting_NonCoreFn, set_integer, get_integer, NULL },
#endif
#if HTTP_ENABLE
    { Setting_HttpPort3, Group_Networking, "HTTP port", NULL, Format_Integer, "####0", "1", "65535", Setting_NonCoreFn, set_integer, get_integer, NULL },
#endif
#if FTP_ENABLE
    { Setting_FtpPort3, Group_Networking, "FTP port", NULL, Format_Int16, "####0", "1", "65535", Setting_NonCoreFn, set_integer, get_integer, NULL },
#endif
#if WEBSOCKET_ENABLE
    { Setting_WebSocketPort3, Group_Networking, "Websocket port", NULL, Format_Integer, "####0", "1", "65535", Setting_NonCoreFn, set_integer, get_integer, NULL },
#endif
};

#ifndef NO_SETTINGS_DESCRIPTIONS
static const setting_descr_t hosted_descriptions[] = {
    { Setting_WifiMode, "ESP32-C5 WiFi mode." },
    { Setting_WiFi_STA_SSID, "SSID sent to the C5 by ESP-Hosted RPC." },
    { Setting_WiFi_STA_Password, "Password sent to the C5 by ESP-Hosted RPC." },
    { Setting_NetworkServices, "Services exposed through the Hosted WiFi interface." },
    { Setting_Hostname3, "lwIP hostname on the RP2350." },
    { Setting_IpMode3, "Static or DHCP addressing on the RP2350." },
    { Setting_IpAddress3, "Static IPv4 address." },
    { Setting_Gateway3, "Static IPv4 gateway." },
    { Setting_NetMask3, "Static IPv4 netmask." }
};
#endif

static void wifi_settings_save(void)
{
    hal.nvs.memcpy_to_nvs(nvs_address, (uint8_t *)&wifi, sizeof(wifi), true);
}

static void wifi_settings_restore(void)
{
    memset(&wifi, 0, sizeof(wifi));
    wifi.mode = WiFiMode_STA;
    wifi.sta.network.ip_mode = (ip_mode_t)NETWORK_STA_IPMODE;
    strlcpy(wifi.sta.network.hostname, NETWORK_STA_HOSTNAME, sizeof(wifi.sta.network.hostname));
    strlcpy(wifi.sta.network.ip, NETWORK_STA_IP, sizeof(wifi.sta.network.ip));
    strlcpy(wifi.sta.network.gateway, NETWORK_STA_GATEWAY, sizeof(wifi.sta.network.gateway));
    strlcpy(wifi.sta.network.mask, NETWORK_STA_MASK, sizeof(wifi.sta.network.mask));
#if TELNET_ENABLE
    wifi.sta.network.telnet_port = NETWORK_TELNET_PORT;
#endif
#if WEBSOCKET_ENABLE
    wifi.sta.network.websocket_port = NETWORK_WEBSOCKET_PORT;
#endif
#if HTTP_ENABLE
    wifi.sta.network.http_port = NETWORK_HTTP_PORT;
#endif
#if FTP_ENABLE
    wifi.sta.network.ftp_port = NETWORK_FTP_PORT;
#endif
    wifi.sta.network.services = allowed_services;
    wifi_settings_save();
}

static void wifi_settings_load(void)
{
    if(hal.nvs.memcpy_from_nvs((uint8_t *)&wifi, nvs_address, sizeof(wifi), true) != NVS_TransferResult_OK)
        wifi_settings_restore();
    if(wifi.mode != WiFiMode_NULL && wifi.mode != WiFiMode_STA)
        wifi.mode = WiFiMode_STA;
    wifi.sta.network.services.mask &= allowed_services.mask;
}

static void wifi_settings_changed(settings_t *settings, settings_changed_flags_t changed)
{
    (void)settings;
    (void)changed;
    schedule_apply();
}

static setting_details_t setting_details = {
    .groups = hosted_groups,
    .n_groups = sizeof(hosted_groups) / sizeof(hosted_groups[0]),
    .settings = hosted_setting_list,
    .n_settings = sizeof(hosted_setting_list) / sizeof(hosted_setting_list[0]),
#ifndef NO_SETTINGS_DESCRIPTIONS
    .descriptions = hosted_descriptions,
    .n_descriptions = sizeof(hosted_descriptions) / sizeof(hosted_descriptions[0]),
#endif
    .on_changed = wifi_settings_changed,
    .save = wifi_settings_save,
    .load = wifi_settings_load,
    .restore = wifi_settings_restore
};

bool esp_hosted_set_credentials(const char *ssid, const char *password)
{
    if(!ssid || !password || strlen(ssid) >= sizeof(wifi.sta.ssid) ||
       strlen(password) >= sizeof(wifi.sta.password))
        return false;

    strlcpy(wifi.sta.ssid, ssid, sizeof(wifi.sta.ssid));
    strlcpy(wifi.sta.password, password, sizeof(wifi.sta.password));
    wifi.mode = WiFiMode_STA;
    wifi_settings_save();
    connection_status = "credentials received";
    schedule_apply();
    return true;
}

const char *esp_hosted_connection_status(void)
{
    return connection_status;
}

static void hosted_startup(void *data)
{
    (void)data;
    lwip_init();
    esp_hosted_rpc_init(rpc_event, NULL);
    if(!esp_hosted_transport_request_start()) {
        task_add_immediate(report_warning, "ESP-Hosted transport could not start.");
        return;
    }
    started = true;
    task_add_systick(hosted_poll, NULL);
}

void esp_hosted_init(void)
{
    if(!(nvs_address = nvs_alloc(sizeof(hosted_settings_t))) ||
       !esp_hosted_transport_prepare()) {
        task_run_on_startup(report_warning, "ESP-Hosted plugin failed to initialize.");
        return;
    }

    networking_init();
    allowed_services = networking_get_services_list(netservices);
    previous_get_info = networking.get_info;
    networking.get_info = get_info;
    previous_report_options = grbl.on_report_options;
    grbl.on_report_options = report_options;
    settings_register(&setting_details);

    multicore_launch_core1(core1_entry);
    task_run_on_startup(hosted_startup, NULL);
}

#endif /* ESP_HOSTED_ENABLE */
