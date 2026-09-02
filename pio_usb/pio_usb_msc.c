/*
  pio_usb_msc.c - MSC (mass storage) class host callbacks for the PIO USB host

  Part of grblHAL

  Handles USB mass-storage devices (e.g. flash drives) enumerated by the PIO
  USB host. For now this only reports attach/detach and basic geometry; block
  I/O and a FatFs diskio backend can be added later. Compiled only when the MSC
  host class is enabled (CFG_TUH_MSC).
*/

#include "driver.h"

#if defined(PIO_USB_HOST_ENABLE) && PIO_USB_HOST_ENABLE

#include "tusb.h"

#if CFG_TUH_MSC

#include <string.h>

#include "grbl/nuts_bolts.h"

#include "pio_usb_host_int.h"

void tuh_msc_mount_cb (uint8_t daddr)
{
    uint32_t block_count = tuh_msc_get_block_count(daddr, 0);
    uint32_t block_size  = tuh_msc_get_block_size(daddr, 0);
    uint32_t size_mb = (uint32_t)(((uint64_t)block_count * block_size) / (1024u * 1024u));

    char buf[PIO_USB_MSG_LEN];
    strcpy(buf, "[MSG:MSC mounted addr=");
    strcat(buf, uitoa(daddr));
    strcat(buf, " size=");
    strcat(buf, uitoa(size_mb));
    strcat(buf, "MB]" ASCII_EOL);
    pio_usb_queue_msg(buf);
}

void tuh_msc_umount_cb (uint8_t daddr)
{
    char buf[PIO_USB_MSG_LEN];
    strcpy(buf, "[MSG:MSC unmounted addr=");
    strcat(buf, uitoa(daddr));
    strcat(buf, "]" ASCII_EOL);
    pio_usb_queue_msg(buf);
}

#endif // CFG_TUH_MSC

#endif // PIO_USB_HOST_ENABLE
