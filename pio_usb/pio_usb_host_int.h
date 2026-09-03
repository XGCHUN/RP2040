/*
  pio_usb_host_int.h - internal shared API for the grblHAL PIO USB host

  Part of grblHAL

  Shared between the PIO USB host core (pio_usb_host.c) and the per-class
  driver files (pio_usb_hid.c, pio_usb_msc.c, pio_usb_uvc.c, ...). Not part of
  the public interface - application code should use pio_usb_host.h.
*/

#ifndef _GRBLHAL_PIO_USB_HOST_INT_H_
#define _GRBLHAL_PIO_USB_HOST_INT_H_

#if defined(PIO_USB_HOST_ENABLE) && PIO_USB_HOST_ENABLE

// Max length of a queued diagnostic message (including the trailing NUL).
#define PIO_USB_MSG_LEN 80

// Queue a short diagnostic line for core0 to print on the grblHAL stream.
// Safe to call from the host stack running on core1; the actual stream write
// happens on core0 from the realtime handler. Messages are dropped if the ring
// is full (diagnostics only).
void pio_usb_queue_msg (const char *s);

// Notify the UVC layer that a device was mounted, so it can probe for a video
// interface. Always defined (a no-op when UVC support is not built) so the
// host core can call it unconditionally.
void pio_usb_uvc_device_mounted (uint8_t daddr);

#endif // PIO_USB_HOST_ENABLE

#endif // _GRBLHAL_PIO_USB_HOST_INT_H_
