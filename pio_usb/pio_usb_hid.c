/*
  pio_usb_hid.c - HID class host callbacks for the grblHAL PIO USB host

  Part of grblHAL

  Handles USB HID devices (keyboard / mouse) enumerated by the PIO USB host.
  For now this is used to validate the host link; report contents are not
  decoded. Compiled only when the HID host class is enabled (CFG_TUH_HID).
*/

#include "driver.h"

#if defined(PIO_USB_HOST_ENABLE) && PIO_USB_HOST_ENABLE

#include "tusb.h"

#if CFG_TUH_HID

#include <string.h>

#include "grbl/nuts_bolts.h"

#include "pio_usb_host_int.h"

void tuh_hid_mount_cb (uint8_t daddr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len)
{
    (void)desc_report;
    (void)desc_len;

    char buf[PIO_USB_MSG_LEN];
    strcpy(buf, "[MSG:HID mounted addr=");
    strcat(buf, uitoa(daddr));
    strcat(buf, " inst=");
    strcat(buf, uitoa(instance));
    strcat(buf, "]" ASCII_EOL);
    pio_usb_queue_msg(buf);

    // Kick off report reception so the endpoint is serviced.
    tuh_hid_receive_report(daddr, instance);
}

void tuh_hid_umount_cb (uint8_t daddr, uint8_t instance)
{
    char buf[PIO_USB_MSG_LEN];
    strcpy(buf, "[MSG:HID unmounted addr=");
    strcat(buf, uitoa(daddr));
    strcat(buf, " inst=");
    strcat(buf, uitoa(instance));
    strcat(buf, "]" ASCII_EOL);
    pio_usb_queue_msg(buf);
}

void tuh_hid_report_received_cb (uint8_t daddr, uint8_t instance, uint8_t const *report, uint16_t len)
{
    (void)report;
    (void)len;

    // Not decoded yet; keep the pipe going.
    tuh_hid_receive_report(daddr, instance);
}

#endif // CFG_TUH_HID

#endif // PIO_USB_HOST_ENABLE
