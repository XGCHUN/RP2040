/*
  xy2_100.c - XY2-100 galvo protocol driver for grblHAL

  Flexible galvo output driver:
  - The board map defines which two axes are galvo axes:
      XY2_AXIS_0, XY2_AXIS_1  (e.g. X_AXIS/Y_AXIS or U_AXIS/V_AXIS)
  - Work area per axis is taken from settings.axis[n].max_travel (mm)
  - Full 16-bit range (0~65535) maps to the travel range
  - Machine position 0 maps to center of galvo range (0x8000)

  Part of grblHAL

  grblHAL is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "driver.h"

#if XY2_100_ENABLE

#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "xy2_100.pio.h"
#include "xy2_100.h"
#include "grbl/settings.h"

// ============================================================
// Board map must define:
//   XY2_AXIS_0       - first galvo axis index  (e.g. X_AXIS or U_AXIS)
//   XY2_AXIS_1       - second galvo axis index (e.g. Y_AXIS or V_AXIS)
//   XY2_DATA_0_PIN   - PIO data pin for axis 0
//   XY2_DATA_1_PIN   - PIO data pin for axis 1
//   XY2_CLK_PIN      - clock pin (shared)
//   XY2_SYNC_PIN     - sync pin (shared, must be CLK+1)
// ============================================================

// Provide defaults from legacy pin names if new names not defined
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

// XY2-100 position is always 16-bit
#define XY2_POS_MAX     65535
#define XY2_POS_CENTER  32768

// PIO instance and state machines
static PIO xy2_pio;
static uint xy2_sm_0;
static uint xy2_sm_1;
static uint xy2_offset;

// Pre-computed: counts per step for each galvo axis
// scale = (65535.0 / travel_mm) / steps_per_mm = 65535.0 / (travel_mm * steps_per_mm)
// But since travel_mm * steps_per_mm = total_steps_in_travel:
// scale = 65535.0 / total_steps_in_travel
static float xy2_scale_0 = 1.0f;
static float xy2_scale_1 = 1.0f;

// Compute scale factor for one axis from settings
static float compute_scale (uint_fast8_t axis_idx)
{
    float travel = settings.axis[axis_idx].max_travel;   // mm
    float steps_per_mm = settings.axis[axis_idx].steps_per_mm;

    if(travel <= 0.0f || steps_per_mm <= 0.0f)
        return 1.0f;

    // total steps across full travel
    // 16-bit range maps to full travel
    return (float)XY2_POS_MAX / (travel * steps_per_mm);
}

void xy2_100_init (void)
{
    // Use pio0 for XY2-100 (pio1 is used by stepper timer)
    xy2_pio = pio0;

    // Add PIO program
    xy2_offset = pio_add_program(xy2_pio, &xy2_100_program);

    // Claim and configure state machines
    xy2_sm_0 = pio_claim_unused_sm(xy2_pio, true);
    xy2_sm_1 = pio_claim_unused_sm(xy2_pio, true);

    xy2_100_program_init(xy2_pio, xy2_sm_0, xy2_offset, XY2_DATA_0_PIN, XY2_CLK_PIN, XY2_SYNC_PIN);
    xy2_100_program_init(xy2_pio, xy2_sm_1, xy2_offset, XY2_DATA_1_PIN, XY2_CLK_PIN, XY2_SYNC_PIN);

    // Sync-start both SMs so CLK/SYNC waveforms are perfectly aligned
    pio_sm_set_enabled(xy2_pio, xy2_sm_0, false);
    pio_sm_set_enabled(xy2_pio, xy2_sm_1, false);
    pio_enable_sm_mask_in_sync(xy2_pio, (1u << xy2_sm_0) | (1u << xy2_sm_1));

    // Compute initial scale from current settings
    xy2_scale_0 = compute_scale(XY2_AXIS_0);
    xy2_scale_1 = compute_scale(XY2_AXIS_1);
}

// Called from stepper ISR — must be fast, non-blocking
void __not_in_flash_func(xy2_100_update)(int32_t axis0_steps, int32_t axis1_steps)
{
    // Convert step position to 16-bit galvo position
    // Position 0 (machine home) → center of galvo range
    int32_t pos0 = (int32_t)(axis0_steps * xy2_scale_0) + XY2_POS_CENTER;
    int32_t pos1 = (int32_t)(axis1_steps * xy2_scale_1) + XY2_POS_CENTER;

    // Clamp to 16-bit range
    if(pos0 < 0) pos0 = 0;
    else if(pos0 > XY2_POS_MAX) pos0 = XY2_POS_MAX;

    if(pos1 < 0) pos1 = 0;
    else if(pos1 > XY2_POS_MAX) pos1 = XY2_POS_MAX;

    // Build 20-bit XY2-100 frames and send to PIO (non-blocking)
    uint32_t frame0 = xy2_100_build_frame((uint16_t)pos0);
    uint32_t frame1 = xy2_100_build_frame((uint16_t)pos1);

    if(!pio_sm_is_tx_fifo_full(xy2_pio, xy2_sm_0))
        pio_sm_put(xy2_pio, xy2_sm_0, frame0);

    if(!pio_sm_is_tx_fifo_full(xy2_pio, xy2_sm_1))
        pio_sm_put(xy2_pio, xy2_sm_1, frame1);
}

// Recalculate when settings change (travel or steps_per_mm updated)
void xy2_100_settings_changed (void)
{
    xy2_scale_0 = compute_scale(XY2_AXIS_0);
    xy2_scale_1 = compute_scale(XY2_AXIS_1);
}

#endif // XY2_100_ENABLE
