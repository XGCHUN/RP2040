/*
  xy2_100.h - XY2-100 galvo protocol driver for grblHAL

  Design:
  - Board map defines XY2_AXIS_0, XY2_AXIS_1, XY2_DATA_0_PIN, XY2_DATA_1_PIN,
    XY2_CLK_PIN, XY2_SYNC_PIN
  - XY2_AXES_MASK suppresses step/dir output on galvo axes (used by driver.c)
  - Work area from settings.axis[n].max_travel; position always 16-bit
  - Compatible with any axis pair and any number of total axes

  Board map must define (when XY2_100_ENABLE=1):
    XY2_AXIS_0, XY2_AXIS_1          — which axes are galvo
    XY2_DATA_0_PIN, XY2_DATA_1_PIN  — PIO data output GPIOs
    XY2_CLK_PIN, XY2_SYNC_PIN       — shared clock and sync (SYNC = CLK + 1)

  Part of grblHAL

  grblHAL is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#ifndef _XY2_100_H_
#define _XY2_100_H_

#include <stdint.h>
#include <stdbool.h>
#include "driver.h"

// === Axis bitmask for suppressing step/dir GPIO on galvo axes ===
// Pure-stepper boards have XY2_100_ENABLE=0, so this header is never included.
#define XY2_AXES_MASK  ((1 << XY2_AXIS_0) | (1 << XY2_AXIS_1))

void xy2_100_init (void);

// Send current galvo position via XY2-100 protocol
// Called from stepper interrupt after position update
void xy2_100_update (int32_t axis0_steps, int32_t axis1_steps);

// Recalculate scale factors when settings change
void xy2_100_settings_changed (void);

#endif // _XY2_100_H_
