/*
  pio_usb_host.h - PIO USB host bring-up for grblHAL (RP2350)

  Part of grblHAL
*/

#ifndef _GRBLHAL_PIO_USB_HOST_H_
#define _GRBLHAL_PIO_USB_HOST_H_

#if defined(PIO_USB_HOST_ENABLE) && PIO_USB_HOST_ENABLE

// Start the PIO USB host stack (launches the host task on core1). Call once
// from core0 after the grblHAL stream has been connected.
void pio_usb_host_start (void);

#endif

#endif // _GRBLHAL_PIO_USB_HOST_H_
