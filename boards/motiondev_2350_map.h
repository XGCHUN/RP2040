/*
  motiondev_2350_map.h - MotionDev 2350 board map for RP2350B

  8-axis capable motion controller with:
    - 4 stepper motor axes (X, Y, Z, A) driven via PIO step + GPIO dir
    - 2 galvo axes (U, V) driven via PIO step + GPIO dir (e.g. XY2-100 / DAC head)
    - SDIO SD card
    - ESP32-C6 Wi-Fi/BT coprocessor over SPI1 (ESP-Hosted)
    - MPU (IMU) over I2C0
    - UART1 device, PWM outputs, WS2812 RGB LED, USB / PIO USB
    - 8 MB QMI PSRAM (CS on GPIO47)

  GPIO map (from board schematic):
    GPIO0  UART0_TX   DEBUG            GPIO24 V_AXIS_DIR  GALVO
    GPIO1  UART0_RX   DEBUG            GPIO25 U_AXIS_DIR  GALVO
    GPIO2  SDIO_CMD   SD CARD          GPIO26 A_AXIS_DIR  MOTOR
    GPIO3  SDIO_CLK   SD CARD          GPIO27 Z_AXIS_DIR  MOTOR
    GPIO4  SDIO_D0    SD CARD          GPIO28 Y_AXIS_DIR  MOTOR
    GPIO5  SDIO_D1    SD CARD          GPIO29 X_AXIS_DIR  MOTOR
    GPIO6  SDIO_D2    SD CARD          GPIO30 X_AXIS_LIMIT MOTOR
    GPIO7  SDIO_D3    SD CARD          GPIO31 Y_AXIS_LIMIT MOTOR
    GPIO8  SPI1_MISO  ESP32C6          GPIO32 Z_AXIS_LIMIT MOTOR
    GPIO9  SPI1_CS0   ESP32C6          GPIO33 A_AXIS_LIMIT MOTOR
    GPIO10 SPI1_CLK   ESP32C6          GPIO34 STEPPER_EN   MOTOR
    GPIO11 SPI1_MOSI  ESP32C6          GPIO35 LASER_POWER_EN LASER
    GPIO12 WIFI_HANDSHAKE ESP32C6      GPIO36 LASER_PWM    LASER
    GPIO13 WIFI_DATA_INT  ESP32C6      GPIO37 FEED_HOLD    INPUT
    GPIO14 WiFi_RESET     ESP32C6      GPIO38 FAN_PWM      OUTPUT
    GPIO15 I2C0_INT   MPU              GPIO39 RGB_LED      WS2812
    GPIO16 I2C0_SDA   MPU              GPIO40 UART1_TX     UART DEVICE
    GPIO17 I2C0_SCL   MPU              GPIO41 UART1_RX     UART DEVICE
    GPIO18 X_AXIS_STEP MOTOR           GPIO42 LIGHT_PWM    OUTPUT
    GPIO19 Y_AXIS_STEP MOTOR           GPIO43 KEY_INT0     INPUT
    GPIO20 Z_AXIS_STEP MOTOR           GPIO44 USB_OTG      USB
    GPIO21 A_AXIS_STEP MOTOR           GPIO45 PIO_USB_D-   PIO USB
    GPIO22 U_AXIS_STEP GALVO           GPIO46 PIO_USB_D+   PIO USB
    GPIO23 V_AXIS_STEP GALVO           GPIO47 PSRAM_SS     QMI PSRAM CS

  Usage in my_machine.h:
    #define BOARD_MOTIONDEV_2350

  Part of grblHAL
*/

#if TRINAMIC_ENABLE
#error Trinamic plugin not supported!
#endif

#define BOARD_NAME "MotionDev 2350"
#define BOARD_URL  ""

#define STEP_PIO          pio0
#define SDIO_PIO          pio1
#define PIO_USB_HOST_PIO  2 // pio2
#define XY2_PIO STEP_PIO

// === PSRAM (8 MB) ===
#undef PICO_PSRAM_CS_PIN
#define PICO_PSRAM_CS_PIN 47

// === Axis count ===
// 4 steppers (X, Y, Z, A) + 2 galvo (U, V) = 6 axes.
#if N_ABC_MOTORS > 3
#error "Axis configuration is not supported!"
#endif

// === Step pins (PIO) ===
// 6 consecutive pins from GPIO18:
//   X=18, Y=19, Z=20, A=21, U=22, V=23
#define STEP_PORT           GPIO_PIO
#define STEP_PINS_BASE      18
#define STEP_PIO_PIN_COUNT  6

// === Direction pins ===
#define DIRECTION_PORT      GPIO_OUTPUT
#define X_DIRECTION_PIN     29
#define Y_DIRECTION_PIN     28
#define Z_DIRECTION_PIN     27
#define DIRECTION_OUTMODE   GPIO_MAP

// === Additional axes ===
// A axis (M3): stepper
#if N_ABC_MOTORS > 0
#define M3_AVAILABLE
#define M3_STEP_PIN         (STEP_PINS_BASE + 3)   // GPIO21
#define M3_DIRECTION_PIN    26
#define M3_LIMIT_PIN        33
#endif
// U axis (M4): galvo
#if N_ABC_MOTORS > 1
#define M4_AVAILABLE
#define M4_STEP_PIN         (STEP_PINS_BASE + 4)   // GPIO22
#define M4_DIRECTION_PIN    25
#endif
// V axis (M5): galvo
#if N_ABC_MOTORS > 2
#define M5_AVAILABLE
#define M5_STEP_PIN         (STEP_PINS_BASE + 5)   // GPIO23
#define M5_DIRECTION_PIN    24
#endif

// === Stepper enable (shared) ===
#define ENABLE_PORT         GPIO_OUTPUT
#define STEPPERS_ENABLE_PIN 34

// === Limit switches (X, Y, Z; A via M3_LIMIT_PIN) ===
#define X_LIMIT_PIN         30
#define Y_LIMIT_PIN         31
#define Z_LIMIT_PIN         32
#define LIMIT_INMODE        GPIO_MAP

// === Auxiliary outputs ===
#define AUXOUTPUT0_PORT     GPIO_OUTPUT
#define AUXOUTPUT0_PIN      35          // Laser power enable
#define AUXOUTPUT1_PORT     GPIO_OUTPUT
#define AUXOUTPUT1_PIN      36          // Laser PWM
#define AUXOUTPUT2_PORT     GPIO_OUTPUT
#define AUXOUTPUT2_PIN      38          // Fan PWM
#define AUXOUTPUT3_PORT     GPIO_OUTPUT
#define AUXOUTPUT3_PIN      42          // Light PWM

// === Auxiliary inputs ===
#define AUXINPUT0_PIN       37          // Feed hold
#define AUXINPUT1_PIN       43          // Key / user button (KEY_INT0)

// === Driver spindle / laser ===
#if DRIVER_SPINDLE_ENABLE
#define SPINDLE_PORT        GPIO_OUTPUT
#endif
#if DRIVER_SPINDLE_ENABLE & SPINDLE_ENA
#define SPINDLE_ENABLE_PIN  AUXOUTPUT0_PIN
#endif
#if DRIVER_SPINDLE_ENABLE & SPINDLE_PWM
#define SPINDLE_PWM_PIN     AUXOUTPUT1_PIN
#endif

// === Control inputs ===
#if CONTROL_ENABLE & CONTROL_FEED_HOLD
#define FEED_HOLD_PIN       AUXINPUT0_PIN
#endif

#if PROBE_ENABLE
#define PROBE_PIN           AUXINPUT1_PIN
#endif

// === I2C0 (MPU / IMU) ===
#if I2C_ENABLE
#define I2C_PORT            0
#define I2C_SDA             16
#define I2C_SCL             17
#endif

// === SD card (native SDIO, 4-bit, via PIO) ===
// Implemented by the PIO SDIO backend in sdcard/sdio.c + sdio.pio.
//
// The four data lines D0..D3 must be 4 consecutive GPIOs, and the PIO also
// requires the layout CLK = D0 - 1. The driver uses SDIO_D0_PIN as the data
// base; SDIO_D1/D2/D3_PIN are listed explicitly for clarity and are checked
// at compile time to be D0+1/+2/+3.
#undef SDCARD_ENABLE
#define SDCARD_ENABLE 1
#undef SDCARD_SDIO
#define SDCARD_SDIO 1
#define SDIO_CMD_PIN        2           // command line (single wire)
#define SDIO_CLK_PIN        3           // clock (must be SDIO_D0_PIN - 1)
#define SDIO_D0_PIN         4           // data line 0 (data base)
#define SDIO_D1_PIN         5           // data line 1 (= D0 + 1)
#define SDIO_D2_PIN         6           // data line 2 (= D0 + 2)
#define SDIO_D3_PIN         7           // data line 3 (= D0 + 3)

// Mount the SD card automatically at power-up (grbl default is off). This is
// what makes the TF card come up on first boot without a manual $FM/mount.
#ifndef DEFAULT_FS_SD_AUTOMOUNT
#define DEFAULT_FS_SD_AUTOMOUNT 1
#endif

// === ESP32-C6 Wi-Fi/BT coprocessor over SPI1 (ESP-Hosted) ===
#if ESP_HOSTED_ENABLE
// Network service ports. The grbl defaults (driver_opts.h) are only defined
// when ETHERNET_ENABLE/WIFI_ENABLE is set, which ESP-Hosted does not use, so
// define the ones referenced by the ESP-Hosted plugin here.
#ifndef NETWORK_HTTP_PORT
#define NETWORK_HTTP_PORT           80
#endif
#ifndef NETWORK_TELNET_PORT
#define NETWORK_TELNET_PORT         23
#endif
#ifndef NETWORK_WEBSOCKET_PORT
#define NETWORK_WEBSOCKET_PORT      81
#endif
#ifndef NETWORK_FTP_PORT
#define NETWORK_FTP_PORT            21
#endif

#define ESP_HOSTED_SPI_PORT         1
#define ESP_HOSTED_SPI_MISO_PIN     8
#define ESP_HOSTED_SPI_CS_PIN       9
#define ESP_HOSTED_SPI_SCK_PIN      10
#define ESP_HOSTED_SPI_MOSI_PIN     11
#define ESP_HOSTED_HANDSHAKE_PIN    12
#define ESP_HOSTED_DATA_READY_PIN   13
#define ESP_HOSTED_RESET_PIN        14
#endif

// === PIO USB host (GPIO45 = D-, GPIO46 = D+) ===
// Full-speed USB host via Pico-PIO-USB on a dedicated PIO block (defaults to
// pio1; pio0 = step/timer/xy2_100, pio2 = SDIO). D- must be D+ - 1.
// Enabled with the ADD_PIO_USB_HOST CMake option (defines PIO_USB_HOST_ENABLE).
#define PIO_USB_DP_PIN      46          // D+ (D- = 45)

// === WS2812 RGB LED (GPIO39) ===
// Data line for the WS2812/NeoPixel status LED. Wired to the neopixels /
// status-light plugin via an aux output when that plugin is enabled.
#define AUXOUTPUT4_PORT     GPIO_OUTPUT
#define AUXOUTPUT4_PIN      39          // WS2812 RGB LED data

// === UART1 device (GPIO40 TX / GPIO41 RX) ===
// Secondary UART for an external device; enable/route via the driver's
// serial/modbus configuration when required.

// === Buffers / planner (large PSRAM available) ===
#undef RX_BUFFER_SIZE
#define RX_BUFFER_SIZE 64 * 1024  // must be a power of 2

#undef TX_BUFFER_SIZE
#define TX_BUFFER_SIZE 16 * 1024  // must be a power of 2

#undef  SEGMENT_BUFFER_SIZE
#define SEGMENT_BUFFER_SIZE 512

#undef DEFAULT_PLANNER_BUFFER_BLOCKS
#define DEFAULT_PLANNER_BUFFER_BLOCKS 512
