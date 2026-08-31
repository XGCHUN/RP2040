/*
  xy2_100.c - XY2-100 galvo protocol driver for grblHAL

  Architecture:
  - Single PIO SM outputs both DATA channels simultaneously (bit-level sync)
  - CLK and SYNC driven by side-set (perfect timing, no glitch)
  - Stepper ISR calls xy2_100_update() which builds interleaved frame
    and writes 2 words directly to PIO TX FIFO (non-blocking)
  - PIO FIFO depth (8 words joined = 4 frames) absorbs timing jitter

  Pin requirements:
    DATA_0_PIN and DATA_1_PIN must be consecutive GPIOs
    CLK_PIN and SYNC_PIN must be consecutive GPIOs (SYNC = CLK + 1)

  Part of grblHAL
*/

#include "driver.h"

#if XY2_100_ENABLE

#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "xy2_100.pio.h"
#include "xy2_100.h"
#include "grbl/settings.h"

// Provide defaults from legacy pin names
#ifndef XY2_DATA_0_PIN
  #if defined(XY2_DATA_U_PIN)
    #define XY2_DATA_0_PIN  XY2_DATA_U_PIN
    #define XY2_DATA_1_PIN  XY2_DATA_V_PIN
  #elif defined(XY2_DATA_X_PIN)
    #define XY2_DATA_0_PIN  XY2_DATA_X_PIN
    #define XY2_DATA_1_PIN  XY2_DATA_Y_PIN
  #endif
#endif

#ifndef XY2_AXIS_0
  #if defined(XY2_DATA_U_PIN)
    #define XY2_AXIS_0  U_AXIS
    #define XY2_AXIS_1  V_AXIS
  #else
    #define XY2_AXIS_0  X_AXIS
    #define XY2_AXIS_1  Y_AXIS
  #endif
#endif

// Verify pin layout
_Static_assert(XY2_DATA_1_PIN == XY2_DATA_0_PIN + 1,
               "XY2_DATA_0_PIN and XY2_DATA_1_PIN must be consecutive GPIOs");
_Static_assert(XY2_SYNC_PIN == XY2_CLK_PIN + 1,
               "XY2_SYNC_PIN must be XY2_CLK_PIN + 1");

#define XY2_POS_MAX     65535
#define XY2_POS_CENTER  32768

// Parity mode: false = even (XY2-100 standard), true = odd
// Can be overridden in board map: #define XY2_PARITY_ODD 1
#ifndef XY2_PARITY_ODD
#define XY2_PARITY_ODD  0
#endif

// XY2-100 shares the PIO instance used by step_pulse / stepper_timer so that
// all grblHAL PIO users stay on one PIO, leaving the other PIO blocks free for
// other consumers (PIO USB, PIO SDIO). Change this macro to relocate it.
#ifndef XY2_PIO
#define XY2_PIO         pio0
#endif

// PIO resources
static PIO xy2_pio;
static uint xy2_sm;
static uint xy2_offset;

// Scale factors
static float xy2_scale_0 = 1.0f;
static float xy2_scale_1 = 1.0f;

static float compute_scale(uint_fast8_t axis_idx)
{
    float travel = settings.axis[axis_idx].max_travel;
    float steps_per_mm = settings.axis[axis_idx].steps_per_mm;

    if (travel <= 0.0f || steps_per_mm <= 0.0f)
        return 1.0f;

    return (float)XY2_POS_MAX / (travel * steps_per_mm);
}

void xy2_100_init(void)
{
    xy2_pio = XY2_PIO;
    xy2_sm = pio_claim_unused_sm(xy2_pio, true);
    uint xy2_offset = pio_add_program(xy2_pio, &xy2_100_program);
    xy2_100_program_init(xy2_pio, xy2_sm, xy2_offset, XY2_DATA_0_PIN, XY2_CLK_PIN);

    // Send initial center position
    uint32_t w0, w1;
    xy2_100_build_pair(XY2_POS_CENTER, XY2_POS_CENTER, XY2_PARITY_ODD, &w0, &w1);
    pio_sm_put_blocking(xy2_pio, xy2_sm, w0);
    pio_sm_put_blocking(xy2_pio, xy2_sm, w1);

    xy2_scale_0 = compute_scale(XY2_AXIS_0);
    xy2_scale_1 = compute_scale(XY2_AXIS_1);
}

// Called from stepper ISR — builds frame and writes to FIFO (non-blocking)
void __not_in_flash_func(xy2_100_update)(int32_t axis0_steps, int32_t axis1_steps)
{
    // Convert steps to 16-bit galvo position
    int32_t pos0 = (int32_t)(axis0_steps * xy2_scale_0) + XY2_POS_CENTER;
    int32_t pos1 = (int32_t)(axis1_steps * xy2_scale_1) + XY2_POS_CENTER;

    if (pos0 < 0) pos0 = 0;
    else if (pos0 > XY2_POS_MAX) pos0 = XY2_POS_MAX;
    if (pos1 < 0) pos1 = 0;
    else if (pos1 > XY2_POS_MAX) pos1 = XY2_POS_MAX;

    uint32_t w0, w1;
    xy2_100_build_pair((uint16_t)pos0, (uint16_t)pos1, XY2_PARITY_ODD, &w0, &w1);

    // Non-blocking write: only send if FIFO has space for both words
    if (pio_sm_get_tx_fifo_level(xy2_pio, xy2_sm) <= 6) {
        pio_sm_put(xy2_pio, xy2_sm, w0);
        pio_sm_put(xy2_pio, xy2_sm, w1);
    }
}

void xy2_100_settings_changed(void)
{
    xy2_scale_0 = compute_scale(XY2_AXIS_0);
    xy2_scale_1 = compute_scale(XY2_AXIS_1);
}

#endif // XY2_100_ENABLE
