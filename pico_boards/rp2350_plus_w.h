/*
 * rp2350_plus_w.h - Pico SDK board header for custom RP2350 + CYW43 board
 *
 * This file tells the Pico SDK that the board has a CYW43 Wi-Fi/BT chip
 * and defines the SPI-over-PIO pins used to communicate with it.
 *
 * Based on pico2_w board header from the SDK.
 */

#ifndef _BOARDS_RP2350_PLUS_W_H
#define _BOARDS_RP2350_PLUS_W_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

// --- Flash ---
#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 4
#endif

#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#endif

// --- PSRAM ---
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

// --- Platform identification (tells SDK this is RP2350, not RP2040) ---
#ifndef PICO_PLATFORM
#define PICO_PLATFORM "rp2350-arm-s"
#endif

// --- Board identification ---
#ifndef PICO_RP2350A
#define PICO_RP2350A 0
#endif

#ifndef PICO_RP2350B
#define PICO_RP2350B 1
#endif

// --- CYW43 Wi-Fi/BT support ---
// This macro is parsed by the SDK's CMake system to set a CMake variable
pico_board_cmake_set(PICO_CYW43_SUPPORTED, 1)

#ifndef CYW43_SUPPORTED
#define CYW43_SUPPORTED 1
#endif

#ifndef PICO_CYW43_SUPPORTED
#define PICO_CYW43_SUPPORTED 1
#endif

// CYW43 pin definitions (directly from your custom hardware)
#ifndef CYW43_DEFAULT_PIN_WL_REG_ON
#define CYW43_DEFAULT_PIN_WL_REG_ON     36
#endif

#ifndef CYW43_DEFAULT_PIN_WL_DATA_OUT
#define CYW43_DEFAULT_PIN_WL_DATA_OUT   37
#endif

#ifndef CYW43_DEFAULT_PIN_WL_DATA_IN
#define CYW43_DEFAULT_PIN_WL_DATA_IN    37
#endif

#ifndef CYW43_DEFAULT_PIN_WL_HOST_WAKE
#define CYW43_DEFAULT_PIN_WL_HOST_WAKE  37
#endif

#ifndef CYW43_DEFAULT_PIN_WL_CLOCK
#define CYW43_DEFAULT_PIN_WL_CLOCK      39
#endif

#ifndef CYW43_DEFAULT_PIN_WL_CS
#define CYW43_DEFAULT_PIN_WL_CS         38
#endif

// CYW43 uses PIO for SPI communication
#ifndef CYW43_PIO_CLOCK_DIV_INT
#define CYW43_PIO_CLOCK_DIV_INT 2
#endif

// --- UART (default stdio) ---
#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 0
#endif

#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 0
#endif

#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 1
#endif

// --- LED (directly via CYW43 GPIO, no on-board LED pin) ---
// If your board has no direct GPIO LED, set this:
// #ifndef PICO_DEFAULT_LED_PIN
// #define PICO_DEFAULT_LED_PIN 25
// #endif

// --- Boot stage 2 ---
#ifndef PICO_BOOT_STAGE2_CHOOSE_W25Q080
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1
#endif

#ifndef PICO_RP2350_B2_SUPPORTED
#define PICO_RP2350_B2_SUPPORTED 1
#endif

#endif // _BOARDS_RP2350_PLUS_W_H
