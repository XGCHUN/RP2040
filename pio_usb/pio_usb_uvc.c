/*
  pio_usb_uvc.c - UVC (USB video camera) host for the grblHAL PIO USB host

  Part of grblHAL

  Placeholder for the USB Video Class host. TinyUSB (as bundled with the Pico
  SDK) has no video *host* class driver, so this will be implemented at the
  application level on top of TinyUSB's generic host API (descriptor parsing,
  VS probe/commit, isochronous/bulk streaming, MJPEG frame assembly), using the
  ESP usb_stream component only as a protocol-flow reference.

  Enabled with PIO_USB_UVC_ENABLE (off by default) so it can be developed
  without affecting the HID/MSC bring-up paths.
*/

#include "driver.h"

#if defined(PIO_USB_HOST_ENABLE) && PIO_USB_HOST_ENABLE
#if defined(PIO_USB_UVC_ENABLE) && PIO_USB_UVC_ENABLE

#include "tusb.h"

#include "pio_usb_host_int.h"

// TODO: implement UVC host
//   - detect a Video Control / Video Streaming interface in the config descriptor
//   - negotiate format/frame via VS_PROBE_CONTROL / VS_COMMIT_CONTROL
//   - select a full-speed alt setting and open the iso/bulk endpoint
//   - assemble MJPEG frames from the streamed payloads
//   - hand completed frames to the application (SD card / network)

#endif // PIO_USB_UVC_ENABLE

#endif // PIO_USB_HOST_ENABLE
