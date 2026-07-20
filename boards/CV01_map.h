/*
  CV01_map.h - 4-axis galvo+stepper laser board for RP2350B

  Axis configuration:
    X, Y — Galvo mirrors via XY2-100 (no step/dir pin, no GPIO wasted)
    Z, A — Stepper motors (step/dir via PIO)

  Pin layout:
    Step (PIO):  GPIO30~31  (Z=30, A=31)  — 2 pins for 2 motors
    Direction:   GPIO22~23  (Z=22, A=23)  — 2 pins for 2 motors
    XY2-100:     GPIO38=DATA_X, GPIO39=DATA_Y, GPIO40=CLK, GPIO41=SYNC
    Limits:      GPIO42=X, GPIO43=Y, GPIO44=Z
    Enable:      GPIO45 (shared)

    X/Y axes do NOT consume any step/dir GPIO — they exist only in grblHAL's
    coordinate system and their position is output purely via XY2-100.

  Usage in my_machine.h:
    #define BOARD_CV01

  Part of grblHAL
*/

#if TRINAMIC_ENABLE
#error Trinamic plugin not supported!
#endif

#define BOARD_NAME "CV01"

// 4 axes: X(galvo), Y(galvo), Z(stepper), A(stepper)
#undef N_AXIS
#define N_AXIS 4

// === XY2-100 configuration ===
#define XY2_100_ENABLE      1
#define XY2_AXIS_0          X_AXIS
#define XY2_AXIS_1          Y_AXIS
#define XY2_DATA_0_PIN      38
#define XY2_DATA_1_PIN      39
#define XY2_CLK_PIN         40
#define XY2_SYNC_PIN        41  // Must be CLK + 1

// === Step Pins ===
// Only 2 physical motors (Z, A), PIO allocates 2 consecutive pins.
// X/Y have NO step pin — position output is purely via XY2-100.
#define STEP_PORT           GPIO_PIO
#define STEP_PINS_BASE      30
#define STEP_PIO_PIN_COUNT  2   // Z, A only

// Override auto-generated step pins:
// PIO out bits: bit0 → GPIO30 (Z), bit1 → GPIO31 (A)
// X/Y step pins still get defined by driver.h (base+0, base+1) but
// their bits are cleared by XY2_AXES_MASK before reaching PIO output.
// Z and A must map to the correct PIO bit positions:
#define X_STEP_PIN          (STEP_PINS_BASE + 2)  // Virtual, never toggled
#define Y_STEP_PIN          (STEP_PINS_BASE + 3)  // Virtual, never toggled
#define Z_STEP_PIN          30  // PIO bit 0
#define M3_STEP_PIN         31  // PIO bit 1 (A axis)

// === Direction Pins ===
// Only Z and A have physical direction output.
// X/Y are dummies (galvo axes, never written due to XY2_AXES_MASK clearing those bits).
#define DIRECTION_PORT      GPIO_OUTPUT
#define X_DIRECTION_PIN     22  // Dummy (shared with Z, but bit never set)
#define Y_DIRECTION_PIN     23  // Dummy (shared with A, but bit never set)
#define Z_DIRECTION_PIN     22
#define DIRECTION_OUTMODE   GPIO_MAP

// === Additional Axis: A (M3) ===
#define M3_AVAILABLE
#define M3_DIRECTION_PIN    23

// Direction mask: only Z and A pins (X/Y bits already cleared by XY2_AXES_MASK)
#define DIRECTION_MASK      ((1UL << Z_DIRECTION_PIN) | (1UL << M3_DIRECTION_PIN))

// === Enable ===
#define ENABLE_PORT         GPIO_OUTPUT
#define STEPPERS_ENABLE_PIN 45

// === Limits (X, Y, Z) ===
#define X_LIMIT_PIN         42
#define Y_LIMIT_PIN         43
#define Z_LIMIT_PIN         44
#define LIMIT_INMODE        GPIO_MAP

// === Spindle / Laser ===
#define AUXOUTPUT3_PORT     GPIO_OUTPUT
#define AUXOUTPUT3_PIN      2           // Laser enable
#define AUXOUTPUT4_PORT     GPIO_OUTPUT
#define AUXOUTPUT4_PIN      3           // Laser PWM
#define AUXOUTPUT5_PORT     GPIO_OUTPUT
#define AUXOUTPUT5_PIN      4           // Laser direction

#if DRIVER_SPINDLE_ENABLE
#define SPINDLE_PORT        GPIO_OUTPUT
#endif
#if DRIVER_SPINDLE_ENABLE & SPINDLE_ENA
#define SPINDLE_ENABLE_PIN  AUXOUTPUT3_PIN
#endif
#if DRIVER_SPINDLE_ENABLE & SPINDLE_PWM
#define SPINDLE_PWM_PIN     AUXOUTPUT4_PIN
#endif
#if DRIVER_SPINDLE_ENABLE & SPINDLE_DIR
#define SPINDLE_DIRECTION_PIN AUXOUTPUT5_PIN
#endif

// === Auxiliary I/O ===
#define AUXOUTPUT0_PORT     GPIO_OUTPUT
#define AUXOUTPUT0_PIN      5
#define AUXOUTPUT1_PORT     GPIO_OUTPUT
#define AUXOUTPUT1_PIN      6

#define AUXINPUT0_PIN       7       // Probe
#define AUXINPUT1_PIN       8       // Safety door
#define AUXINPUT7_PIN       9       // Reset/EStop
#define AUXINPUT8_PIN       10      // Feed hold
#define AUXINPUT9_PIN       11      // Cycle start

// === Control ===
#if CONTROL_ENABLE & CONTROL_HALT
#define RESET_PIN           AUXINPUT7_PIN
#endif
#if CONTROL_ENABLE & CONTROL_FEED_HOLD
#define FEED_HOLD_PIN       AUXINPUT8_PIN
#endif
#if CONTROL_ENABLE & CONTROL_CYCLE_START
#define CYCLE_START_PIN     AUXINPUT9_PIN
#endif
#if PROBE_ENABLE
#define PROBE_PIN           AUXINPUT0_PIN
#endif
#if SAFETY_DOOR_ENABLE
#define SAFETY_DOOR_PIN     AUXINPUT1_PIN
#elif MOTOR_FAULT_ENABLE
#define MOTOR_FAULT_PIN     AUXINPUT1_PIN
#endif

// === I2C ===
#if I2C_ENABLE
#define I2C_PORT            1
#define I2C_SDA             14
#define I2C_SCL             15
#endif

// === SPI / SD Card ===
#if SDCARD_ENABLE
#define SPI_PORT            0
#define SPI_SCK_PIN         18
#define SPI_MOSI_PIN        19
#define SPI_MISO_PIN        16
#define SD_CS_PIN           17
#endif
