# ESP-Hosted RP2350 WiFi/BLE plugin

This plugin turns an ESP32-C5 running the official Espressif ESP-Hosted co-processor
firmware into a WiFi network card and BLE controller for a bare-metal RP2350 host
(no RTOS). The C5 runs only stock ESP-Hosted firmware; all networking, HTTP/WebSocket/
FTP/WebUI, SD/FatFS and BLE provisioning logic runs on the RP2350.

Responsibility split:

- Core 1 owns the ESP-Hosted SPI full-duplex transport.
- Core 0 owns lwIP (`NO_SYS=1`), the ESP-Hosted RPC client, the network services and
  the BTstack BLE host.
- The C5 supplies the WiFi MAC/PHY and the BLE controller only. It does not run any
  custom provisioning or GATT logic.

```text
Phone (BLE)
   -> C5 BLE controller
   -> Hosted HCI over SPI
   -> RP2350 BTstack BLE host
   -> BLE provisioning GATT
   -> $74/$75 credentials -> Hosted RPC -> C5 WiFi connect
```

## Builds

WiFi only:

```sh
cmake -S . -B build-hosted -DADD_ESP_HOSTED=ON -DADD_WIFI=OFF -DADD_ETHERNET=OFF -DADD_BLUETOOTH=OFF
cmake --build build-hosted -j4
```

WiFi + BLE provisioning:

```sh
cmake -S . -B build-hosted-ble -DADD_ESP_HOSTED=ON -DADD_BLUETOOTH=ON -DADD_WIFI=OFF -DADD_ETHERNET=OFF
cmake --build build-hosted-ble -j4
```

`ADD_WIFI` (CYW43), `ADD_ETHERNET` (W5x00) and `ADD_ESP_HOSTED` are mutually exclusive.
Hosted builds enable HTTP, WebUI, WebSocket, FTP and SD card support. Telnet is disabled
by default to save RP2350 RAM. When `ADD_BLUETOOTH=ON` together with `ADD_ESP_HOSTED=ON`
the build links BLE-only BTstack (`pico_btstack_ble`) over the Hosted HCI transport and
never links the native CYW43/Classic Bluetooth transports.

## Settings ($ commands)

- `$73` WiFi mode: Off or Station.
- `$74` Station SSID. Sent to the C5 over ESP-Hosted RPC, not just stored.
- `$75` Station password. Sent to the C5 over ESP-Hosted RPC.
- Hostname, IP mode (Static/DHCP), static IP/gateway/netmask, network services and
  service ports use the standard grblHAL networking settings.

Consecutive `$74`/`$75` writes are coalesced (500 ms) so a single reconnect happens.

## RP2350 board macros

Define these in the selected board map; the `0xFF` defaults intentionally make transport
startup fail safely until pins are assigned:

- `ESP_HOSTED_SPI_PORT`, `ESP_HOSTED_SPI_CLOCK_HZ`, `ESP_HOSTED_SPI_MODE`
- `ESP_HOSTED_SPI_SCK_PIN`, `ESP_HOSTED_SPI_MOSI_PIN`, `ESP_HOSTED_SPI_MISO_PIN`, `ESP_HOSTED_SPI_CS_PIN`
- `ESP_HOSTED_HANDSHAKE_PIN`, `ESP_HOSTED_DATA_READY_PIN`, `ESP_HOSTED_RESET_PIN`
- Optional active-level overrides

Start with SPI mode 3 at 10 MHz and connect `ESP_HOSTED_RESET_PIN` to the C5 active-low
reset/enable input. The STA MAC is read from the C5 over RPC (`Req_GetMACAddress`) and
used for the `wh0` lwIP netif, so no MAC needs to be hard-coded.

## ESP32-C5 co-processor firmware

Build the Espressif unified ESP-Hosted firmware (pinned to the 2.12.x wire protocol this
host targets) for target `esp32c5` and enable:

- SPI Full Duplex transport, mode 3, matching GPIOs, with checksum enabled
- WiFi and Network Split (default network stack on the host, DHCP on the host)
- Hosted HCI / Bluetooth so the BLE controller is reachable over the same SPI link

The GPIO pins configured in the C5 firmware Kconfig must match the RP2350 board macros.

## BLE provisioning

Provisioning is a BLE-only GATT service running on the RP2350 (see
`esp_hosted_provisioning.gatt`), advertised as `grblHAL`:

- SSID write characteristic (`9e7a0002-...`), encrypted write
- Password write characteristic (`9e7a0003-...`), encrypted write, never readable
- Apply/commit write characteristic (`9e7a0004-...`)
- Status read/notify characteristic (`9e7a0005-...`)

On Apply the staged SSID/password are committed atomically and pushed to the C5 via the
same RPC path as `$74`/`$75`. Status changes (`applying`, `connected`, `disconnected`,
etc.) are pushed as notifications. Advertising stops while a central is connected or once
WiFi is connected, and resumes on disconnect/link loss. If the C5 is reset, the transport
re-initializes, the BTstack host power-cycles and provisioning restarts.

## HTTP interfaces

The WebUI handlers provide `POST /upload` and `POST /sdfiles` for SD uploads,
`GET /sdfiles` for SD listing, `GET /sd/*` for downloads and `GET /command` for machine
operations/status. Hardware validation is still required after assigning real SPI pins
and flashing matching C5 firmware; the placeholder pins compile but fault at runtime by
design.
