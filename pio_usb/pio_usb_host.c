/*
  pio_usb_host.c - PIO USB host core for grblHAL (RP2350)

  Part of grblHAL

  Brings up a USB host controller on an RP2350 PIO block using sekigon-gonnoc's
  Pico-PIO-USB together with TinyUSB's host stack, while the native USB
  controller keeps serving the grblHAL CDC stream (device mode).

  The PIO USB host runs on TinyUSB roothub port 1. Following the upstream
  examples the host SOF interrupt and host task are executed on core1 so they
  do not compete with grblHAL's time-critical work on core0.

  This file owns the shared host infrastructure only:
    - core1 host stack startup and task loop,
    - the cross-core diagnostic message ring (pio_usb_queue_msg),
    - the generic device mount/unmount callbacks.

  Per-class drivers live in separate files and use pio_usb_queue_msg():
    - pio_usb_hid.c   HID (keyboard / mouse, bring-up validation)
    - pio_usb_msc.c   MSC (USB mass storage)
    - pio_usb_uvc.c   UVC (USB video camera)

  Data pins (from the board map):
    PIO_USB_DP_PIN (D+), D- = D+ - 1   [must be consecutive]
*/

#include "driver.h"

#if defined(PIO_USB_HOST_ENABLE) && PIO_USB_HOST_ENABLE

#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"

#include "pio_usb.h"
#include "tusb.h"

#include "grbl/hal.h"
#include "grbl/nuts_bolts.h"

#include "pio_usb_host_int.h"

#ifndef PIO_USB_DP_PIN
// Board map should define this. MotionDev 2350: D+ = GPIO46, D- = GPIO45.
#define PIO_USB_DP_PIN 46
#endif

// PIO block *number* for the host controller (Pico-PIO-USB takes a number).
// grblHAL's other PIO users live on pio0 (step/timer/xy2_100) and pio1 (SDIO),
// so default the host to pio2. Board map may override.
#ifndef PIO_USB_HOST_PIO
#define PIO_USB_HOST_PIO 2
#endif

// --- Cross-core diagnostic message ring -----------------------------------
// The host stack runs on core1; grblHAL streams belong to core0. Callbacks
// stash a short message which core0 drains from on_execute_realtime, avoiding
// concurrent stream access.

#define PIO_USB_MSG_SLOTS 8

static volatile uint32_t msg_head = 0, msg_tail = 0;
static char msg_ring[PIO_USB_MSG_SLOTS][PIO_USB_MSG_LEN];

static on_execute_realtime_ptr on_execute_realtime;

void pio_usb_queue_msg (const char *s)
{
    uint32_t next = (msg_head + 1) % PIO_USB_MSG_SLOTS;
    if(next == msg_tail)
        return;                     // ring full, drop (diagnostics only)
    strncpy(msg_ring[msg_head], s, PIO_USB_MSG_LEN - 1);
    msg_ring[msg_head][PIO_USB_MSG_LEN - 1] = '\0';
    msg_head = next;
}

static void pio_usb_poll_msgs (uint_fast16_t state)
{
    while(msg_tail != msg_head) {
        hal.stream.write(msg_ring[msg_tail]);
        msg_tail = (msg_tail + 1) % PIO_USB_MSG_SLOTS;
    }

    on_execute_realtime(state);
}

// --- core1: PIO USB host stack --------------------------------------------

static void core1_usb_host (void)
{
    // Pass the PIO USB pin configuration to the host stack before init.
    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.pin_dp = PIO_USB_DP_PIN;    // D- is D+ - 1 (handled by the library)
    pio_cfg.pio_tx_num = PIO_USB_HOST_PIO;
    pio_cfg.pio_rx_num = PIO_USB_HOST_PIO;

    tuh_configure(1, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);

    // Initialise the host stack on roothub port 1 (the PIO USB controller).
    tuh_init(1);

    while(true)
        tuh_task();
}

// Start the PIO USB host. Call once from core0 after the grblHAL stream is up.
void pio_usb_host_start (void)
{
    on_execute_realtime = grbl.on_execute_realtime;
    grbl.on_execute_realtime = pio_usb_poll_msgs;

    // Launch the host stack on core1 (SOF IRQ + tuh_task run there).
    multicore_reset_core1();
    multicore_launch_core1(core1_usb_host);

    pio_usb_queue_msg("[MSG:PIO USB host started]" ASCII_EOL);
}

// --- Generic device enumeration callbacks (run on core1) ------------------

void tuh_mount_cb (uint8_t daddr)
{
    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(daddr, &vid, &pid);

    char buf[PIO_USB_MSG_LEN];
    // [MSG:USB mounted addr=1 VID:PID=1234:5678]
    strcpy(buf, "[MSG:USB mounted addr=");
    strcat(buf, uitoa(daddr));
    strcat(buf, " VID:PID=");
    strcat(buf, uitoa(vid));
    strcat(buf, ":");
    strcat(buf, uitoa(pid));
    strcat(buf, "]" ASCII_EOL);
    pio_usb_queue_msg(buf);
}

void tuh_umount_cb (uint8_t daddr)
{
    char buf[PIO_USB_MSG_LEN];
    strcpy(buf, "[MSG:USB unmounted addr=");
    strcat(buf, uitoa(daddr));
    strcat(buf, "]" ASCII_EOL);
    pio_usb_queue_msg(buf);
}

#endif // PIO_USB_HOST_ENABLE
