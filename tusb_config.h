/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 * Copyright (c) 2020 Damien P. George
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#ifndef _PICO_STDIO_USB_TUSB_CONFIG_H
#define _PICO_STDIO_USB_TUSB_CONFIG_H

#define CFG_TUSB_RHPORT0_MODE   (OPT_MODE_DEVICE)

#define CFG_TUD_CDC             (1)
#define CFG_TUD_CDC_RX_BUFSIZE  (1 * 1024)
#define CFG_TUD_CDC_TX_BUFSIZE  (1 * 1024)

// ---------------------------------------------------------------------------
// PIO USB host (roothub port 1) - Pico-PIO-USB on GPIO45/46.
// Native USB (port 0) stays in device mode for the grblHAL CDC stream; the PIO
// USB controller runs the host stack on port 1. Enabled when PIO_USB_HOST_ENABLE
// is defined (set by the board map / CMake).
// ---------------------------------------------------------------------------
#if defined(PIO_USB_HOST_ENABLE) && PIO_USB_HOST_ENABLE

// Root hub port 1 = host (the PIO USB controller).
#define CFG_TUSB_RHPORT1_MODE   (OPT_MODE_HOST)

// Two controllers are present (native device + PIO host).
#ifndef CFG_TUH_RHPORT
#define CFG_TUH_RHPORT          (1)
#endif

// Max devices behind the (single) root hub port, excluding hubs.
#ifndef CFG_TUH_DEVICE_MAX
#define CFG_TUH_DEVICE_MAX      (1)
#endif

// Enough enumeration buffer for full-speed config descriptors.
#ifndef CFG_TUH_ENUMERATION_BUFSIZE
#define CFG_TUH_ENUMERATION_BUFSIZE (256)
#endif

// Stage 0 bring-up: enable HID host to validate enumeration with a keyboard /
// mouse. The UVC video host class is added in a later stage.
#ifndef CFG_TUH_HID
#define CFG_TUH_HID             (2)
#endif
#ifndef CFG_TUH_HID_EPIN_BUFSIZE
#define CFG_TUH_HID_EPIN_BUFSIZE (64)
#endif
#ifndef CFG_TUH_HID_EPOUT_BUFSIZE
#define CFG_TUH_HID_EPOUT_BUFSIZE (64)
#endif

// MSC host: enable to mount USB mass-storage devices (flash drives).
#ifndef CFG_TUH_MSC
#define CFG_TUH_MSC             (1)
#endif

// No external hub support for now (single camera / device on the port).
#ifndef CFG_TUH_HUB
#define CFG_TUH_HUB             (0)
#endif

#endif // PIO_USB_HOST_ENABLE

#endif
