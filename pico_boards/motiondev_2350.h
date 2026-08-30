/*
 * motiondev_2350.h - Pico SDK board header for MotionDev 2350 (RP2350B)
 *
 * Custom 8-axis motion controller board based on RP2350B with:
 *   - 16 MB QSPI flash
 *   - 8 MB QMI PSRAM (CS on GPIO47)
 *   - ESP32-C6 Wi-Fi/BT coprocessor over SPI1 (ESP-Hosted, not CYW43)
 *
 * Since networking is provided by the ESP32-C6 via ESP-Hosted, the on-chip
 * CYW43 support is NOT enabled here.
 *
 * Based on rp2350_plus_w.h.
 */

#ifndef _BOARDS_MOTIONDEV_2350_H
#define _BOARDS_MOTIONDEV_2350_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

// --- Flash ---
#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 4
#endif

#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#endif

// --- PSRAM (8 MB, CS on GPIO47) ---
#ifndef PICO_PSRAM_CS_PIN
#define PICO_PSRAM_CS_PIN 47
#endif

#ifndef PICO_PSRAM_SIZE_BYTES
#define PICO_PSRAM_SIZE_BYTES (8 * 1024 * 1024)
#endif

// Auto-detect PSRAM size at runtime (overrides PICO_PSRAM_SIZE_BYTES if actual is smaller)
#ifndef PICO_AUTO_DETECT_PSRAM_SIZE
#define PICO_AUTO_DETECT_PSRAM_SIZE 1
#endif

// --- Platform identification (RP2350B, not RP2040) ---
#ifndef PICO_PLATFORM
#define PICO_PLATFORM "rp2350-arm-s"
#endif

// --- Board identification (RP2350B: larger package with GPIO0..47) ---
#ifndef PICO_RP2350A
#define PICO_RP2350A 0
#endif

#ifndef PICO_RP2350B
#define PICO_RP2350B 1
#endif

// --- Networking via ESP32-C6 over SPI (ESP-Hosted). CYW43 not present. ---
#ifndef CYW43_SUPPORTED
#define CYW43_SUPPORTED 0
#endif

#ifndef PICO_CYW43_SUPPORTED
#define PICO_CYW43_SUPPORTED 0
#endif

// --- UART (default stdio / debug on GPIO0/GPIO1) ---
#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 0
#endif

#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 0
#endif

#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 1
#endif

// --- Boot stage 2 ---
#ifndef PICO_BOOT_STAGE2_CHOOSE_W25Q080
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1
#endif

#ifndef PICO_RP2350_B2_SUPPORTED
#define PICO_RP2350_B2_SUPPORTED 1
#endif

#endif // _BOARDS_MOTIONDEV_2350_H
