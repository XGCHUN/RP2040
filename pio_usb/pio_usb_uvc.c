/*
  pio_usb_uvc.c - UVC (USB video camera) host for the grblHAL PIO USB host

  Part of grblHAL

  USB Video Class host built on TinyUSB's generic host API (the bundled TinyUSB
  has no video host driver). Uses the ESP usb_stream component only as a
  protocol-flow reference.

  Implemented so far (stage 1): descriptor discovery. On device mount the full
  configuration descriptor is fetched and scanned for a VideoControl (VC) +
  VideoStreaming (VS) interface, and the supported MJPEG/uncompressed formats
  and frame sizes are reported over the grblHAL stream. This validates what a
  camera exposes (in particular at full speed) before adding probe/commit and
  isochronous/bulk streaming.

  Streaming (iso/bulk transfer + MJPEG frame assembly) is gated behind
  PIO_USB_UVC_ENABLE and added in a later stage.
*/

#include "driver.h"

#if defined(PIO_USB_HOST_ENABLE) && PIO_USB_HOST_ENABLE

#include <string.h>

#include "tusb.h"

#include "grbl/nuts_bolts.h"

#include "pio_usb_host_int.h"

// --- USB Video Class constants (UVC 1.1/1.5 spec) --------------------------

#ifndef TUSB_CLASS_VIDEO
#define TUSB_CLASS_VIDEO            0x0Eu
#endif

#define UVC_SUBCLASS_CONTROL        0x01u   // VideoControl interface
#define UVC_SUBCLASS_STREAMING      0x02u   // VideoStreaming interface

// VideoStreaming interface descriptor subtypes (bDescriptorSubtype).
#define UVC_VS_INPUT_HEADER         0x01u
#define UVC_VS_FORMAT_UNCOMPRESSED  0x04u
#define UVC_VS_FRAME_UNCOMPRESSED   0x05u
#define UVC_VS_FORMAT_MJPEG         0x06u
#define UVC_VS_FRAME_MJPEG          0x07u

// Class-specific interface descriptor type (CS_INTERFACE).
#define UVC_CS_INTERFACE            0x24u

// VideoStreaming interface control selectors (wValue high byte).
#define UVC_VS_PROBE_CONTROL        0x01u
#define UVC_VS_COMMIT_CONTROL       0x02u

// Class-specific request codes (bRequest).
#define UVC_SET_CUR                 0x01u
#define UVC_GET_CUR                 0x81u
#define UVC_GET_MIN                 0x82u
#define UVC_GET_MAX                 0x83u
#define UVC_GET_RES                 0x84u

// Probe/commit control block is 26 bytes for UVC 1.1 (34 for 1.5). 26 works
// for the low-frame-rate MJPEG cameras targeted here.
#define UVC_PROBE_LEN               26u

// Desired frame interval in 100 ns units. 3 fps = 1/3 s = 3333333 * 100 ns.
// This is the key knob for low-frame-rate cameras (ESP usb_stream used 666666
// = 15 fps by default). Override from the board map / build if needed.
#ifndef UVC_FRAME_INTERVAL_100NS
#define UVC_FRAME_INTERVAL_100NS    3333333u   // 3 fps
#endif

// Config descriptor fetch buffer. Full-speed configurations are small, but a
// camera with several formats/frames can still be a few hundred bytes.
#define UVC_CFG_DESC_BUFSIZE        512u

static uint8_t cfg_buf[UVC_CFG_DESC_BUFSIZE];

// UVC transfer type of the streaming endpoint.
typedef enum {
    UVC_XFER_UNKNOWN = 0,
    UVC_XFER_ISO,
    UVC_XFER_BULK
} uvc_xfer_t;

// Result of scanning the configuration descriptor: what to negotiate/open.
typedef struct {
    bool     found;             // a usable MJPEG stream was found
    uint8_t  daddr;             // device address
    uint8_t  vs_itf;            // VideoStreaming interface number
    uint8_t  vs_alt;            // alt setting that carries the streaming endpoint
    uint8_t  format_index;      // bFormatIndex of the MJPEG format
    uint8_t  frame_index;       // bFrameIndex of the chosen (largest) frame
    uint16_t width, height;     // chosen frame size
    uint8_t  ep_addr;           // streaming endpoint address
    uint16_t ep_mps;            // endpoint max packet size
    uvc_xfer_t xfer;            // iso or bulk
} uvc_stream_info_t;

static uvc_stream_info_t uvc_info;

// Negotiated probe/commit control block (filled from the camera's GET_CUR).
static uint8_t probe_ctrl[UVC_PROBE_LEN];

// --- Small helpers ---------------------------------------------------------

static uint16_t rd16 (const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32 (const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void report (const char *s)
{
    pio_usb_queue_msg(s);
}

// Report a "[MSG:UVC <label>=<value>]" line.
static void report_kv (const char *label, uint32_t value)
{
    char buf[PIO_USB_MSG_LEN];
    strcpy(buf, "[MSG:UVC ");
    strncat(buf, label, 32);
    strcat(buf, "=");
    strcat(buf, uitoa(value));
    strcat(buf, "]" ASCII_EOL);
    pio_usb_queue_msg(buf);
}

// Report a frame size: "[MSG:UVC frame WxH]".
static void report_frame (const char *fmt, uint16_t w, uint16_t h)
{
    char buf[PIO_USB_MSG_LEN];
    strcpy(buf, "[MSG:UVC ");
    strncat(buf, fmt, 12);
    strcat(buf, " ");
    strcat(buf, uitoa(w));
    strcat(buf, "x");
    strcat(buf, uitoa(h));
    strcat(buf, "]" ASCII_EOL);
    pio_usb_queue_msg(buf);
}

// --- Descriptor discovery --------------------------------------------------

// Walk a fetched configuration descriptor: report UVC interfaces/formats/frames
// (diagnostics) and record what to negotiate in uvc_info (largest MJPEG frame
// and the streaming endpoint / alt setting that carries it).
static void uvc_scan_config (uint8_t daddr, const uint8_t *desc, uint16_t total_len)
{
    const uint8_t *p = desc;
    const uint8_t *end = desc + total_len;
    bool in_vs = false;         // currently inside a VideoStreaming interface
    bool found_video = false;

    uint8_t  cur_vs_itf = 0;    // VS interface number currently being parsed
    uint8_t  cur_vs_alt = 0;    // its alt setting
    uint8_t  mjpeg_fmt_index = 0;   // bFormatIndex of the MJPEG format, once seen
    bool     in_mjpeg_fmt = false;  // frames after an MJPEG format belong to it

    memset(&uvc_info, 0, sizeof(uvc_info));
    uvc_info.daddr = daddr;

    while(p + 2 <= end) {
        uint8_t blen = p[0];
        uint8_t btype = p[1];

        if(blen < 2 || p + blen > end)
            break;              // malformed / truncated

        if(btype == TUSB_DESC_INTERFACE && blen >= 9) {
            uint8_t itf_class    = p[5];
            uint8_t itf_subclass = p[6];
            uint8_t itf_num      = p[2];
            uint8_t itf_alt      = p[3];
            uint8_t num_ep       = p[4];

            if(itf_class == TUSB_CLASS_VIDEO) {
                found_video = true;
                if(itf_subclass == UVC_SUBCLASS_CONTROL) {
                    in_vs = false;
                    report_kv("VC-itf", itf_num);
                } else if(itf_subclass == UVC_SUBCLASS_STREAMING) {
                    in_vs = true;
                    cur_vs_itf = itf_num;
                    cur_vs_alt = itf_alt;
                    char buf[PIO_USB_MSG_LEN];
                    strcpy(buf, "[MSG:UVC VS-itf=");
                    strcat(buf, uitoa(itf_num));
                    strcat(buf, " alt=");
                    strcat(buf, uitoa(itf_alt));
                    strcat(buf, " eps=");
                    strcat(buf, uitoa(num_ep));
                    strcat(buf, "]" ASCII_EOL);
                    pio_usb_queue_msg(buf);
                }
            } else
                in_vs = false;
        } else if(in_vs && btype == UVC_CS_INTERFACE && blen >= 3) {
            uint8_t subtype = p[2];
            switch(subtype) {
                case UVC_VS_FORMAT_MJPEG:
                    mjpeg_fmt_index = p[3];     // bFormatIndex
                    in_mjpeg_fmt = true;
                    uvc_info.vs_itf = cur_vs_itf;
                    uvc_info.format_index = mjpeg_fmt_index;
                    report_kv("MJPEG-fmt frames", p[4]);
                    break;
                case UVC_VS_FORMAT_UNCOMPRESSED:
                    in_mjpeg_fmt = false;
                    report_kv("YUY2-fmt frames", p[4]);
                    break;
                case UVC_VS_FRAME_MJPEG:
                    if(blen >= 9) {
                        uint8_t  frame_index = p[3];
                        uint16_t w = rd16(p + 5);
                        uint16_t h = rd16(p + 7);
                        report_frame("MJPEG", w, h);
                        // Pick the largest MJPEG frame as the default target.
                        if(in_mjpeg_fmt &&
                           (uint32_t)w * h > (uint32_t)uvc_info.width * uvc_info.height) {
                            uvc_info.found = true;
                            uvc_info.frame_index = frame_index;
                            uvc_info.width  = w;
                            uvc_info.height = h;
                        }
                    }
                    break;
                case UVC_VS_FRAME_UNCOMPRESSED:
                    if(blen >= 9)
                        report_frame("YUY2", rd16(p + 5), rd16(p + 7));
                    break;
                default:
                    break;
            }
        } else if(in_vs && btype == TUSB_DESC_ENDPOINT && blen >= 7) {
            uint8_t  ep_addr = p[2];
            uint8_t  ep_attr = p[3];
            uint16_t ep_mps  = rd16(p + 4) & 0x7FFu;
            bool     is_iso  = (ep_attr & 0x03u) == 0x01u;

            // Record the streaming endpoint (and the alt setting it lives in).
            uvc_info.ep_addr = ep_addr;
            uvc_info.ep_mps  = ep_mps;
            uvc_info.vs_alt  = cur_vs_alt;
            uvc_info.xfer    = is_iso ? UVC_XFER_ISO : UVC_XFER_BULK;

            char buf[PIO_USB_MSG_LEN];
            strcpy(buf, "[MSG:UVC ep=");
            strcat(buf, uitoa(ep_addr));
            strcat(buf, is_iso ? " iso mps=" : " bulk mps=");
            strcat(buf, uitoa(ep_mps));
            strcat(buf, "]" ASCII_EOL);
            pio_usb_queue_msg(buf);
        }

        p += blen;
    }

    if(!found_video)
        report("[MSG:UVC no video interface]" ASCII_EOL);
    else
        report("[MSG:UVC scan done]" ASCII_EOL);
}

// --- Control transfer helper (blocking) ------------------------------------

// Issue a UVC class control transfer and block until it completes.
// dir_in selects GET (device->host); buf/len is the data stage.
static bool uvc_control_xfer (uint8_t daddr, bool dir_in, uint8_t request,
                              uint16_t value, uint16_t index, uint8_t *buf, uint16_t len)
{
    tusb_control_request_t req = {
        .bmRequestType_bit = {
            .recipient = TUSB_REQ_RCPT_INTERFACE,
            .type      = TUSB_REQ_TYPE_CLASS,
            .direction = dir_in ? TUSB_DIR_IN : TUSB_DIR_OUT
        },
        .bRequest = request,
        .wValue   = value,
        .wIndex   = index,
        .wLength  = len
    };

    tuh_xfer_t xfer = {
        .daddr       = daddr,
        .ep_addr     = 0,
        .setup       = &req,
        .buffer      = buf,
        .complete_cb = NULL,        // NULL = blocking
        .user_data   = 0
    };

    return tuh_control_xfer(&xfer) && xfer.result == XFER_RESULT_SUCCESS;
}

// --- Probe / commit negotiation --------------------------------------------

// Negotiate the stream format with the camera: SET_CUR(PROBE) our request,
// GET_CUR(PROBE) the accepted parameters, then SET_CUR(COMMIT). On success the
// accepted control block is left in probe_ctrl[]. Returns true on success.
static bool uvc_negotiate (void)
{
    if(!uvc_info.found)
        return false;

    uint8_t buf[UVC_PROBE_LEN];
    memset(buf, 0, sizeof(buf));

    // Build the PROBE request (26-byte control block, little-endian fields).
    buf[0] = 0x01;                                  // bmHint: keep dwFrameInterval
    buf[2] = uvc_info.format_index;                 // bFormatIndex
    buf[3] = uvc_info.frame_index;                  // bFrameIndex
    buf[4] = (uint8_t)(UVC_FRAME_INTERVAL_100NS);   // dwFrameInterval (LE)
    buf[5] = (uint8_t)(UVC_FRAME_INTERVAL_100NS >> 8);
    buf[6] = (uint8_t)(UVC_FRAME_INTERVAL_100NS >> 16);
    buf[7] = (uint8_t)(UVC_FRAME_INTERVAL_100NS >> 24);

    // SET_CUR(PROBE)
    if(!uvc_control_xfer(uvc_info.daddr, false, UVC_SET_CUR,
                         (uint16_t)(UVC_VS_PROBE_CONTROL << 8), uvc_info.vs_itf,
                         buf, UVC_PROBE_LEN)) {
        report("[MSG:UVC probe SET failed]" ASCII_EOL);
        return false;
    }

    // GET_CUR(PROBE) - the camera returns the parameters it will use.
    if(!uvc_control_xfer(uvc_info.daddr, true, UVC_GET_CUR,
                         (uint16_t)(UVC_VS_PROBE_CONTROL << 8), uvc_info.vs_itf,
                         probe_ctrl, UVC_PROBE_LEN)) {
        report("[MSG:UVC probe GET failed]" ASCII_EOL);
        return false;
    }

    // Report the negotiated frame interval and max frame/payload sizes.
    report_kv("nego interval", rd32(probe_ctrl + 4));
    report_kv("nego maxframe", rd32(probe_ctrl + 18));
    report_kv("nego maxpayload", rd32(probe_ctrl + 22));

    // COMMIT the negotiated parameters (use what the camera returned).
    if(!uvc_control_xfer(uvc_info.daddr, false, UVC_SET_CUR,
                         (uint16_t)(UVC_VS_COMMIT_CONTROL << 8), uvc_info.vs_itf,
                         probe_ctrl, UVC_PROBE_LEN)) {
        report("[MSG:UVC commit failed]" ASCII_EOL);
        return false;
    }

    report("[MSG:UVC commit ok]" ASCII_EOL);
    return true;
}

// Called from the generic mount callback (core1). Fetch the configuration
// descriptor and, if it contains a UVC interface, report its capabilities.
void pio_usb_uvc_device_mounted (uint8_t daddr)
{
    // Blocking fetch of the first configuration descriptor.
    uint8_t rc = tuh_descriptor_get_configuration_sync(daddr, 0, cfg_buf, sizeof(cfg_buf));
    if(rc != XFER_RESULT_SUCCESS)
        return;                 // could not read config descriptor; not fatal

    // Total length is in the config descriptor header (offset 2, 2 bytes).
    uint16_t total_len = rd16(cfg_buf + 2);
    if(total_len > sizeof(cfg_buf))
        total_len = sizeof(cfg_buf);

    uvc_scan_config(daddr, cfg_buf, total_len);

    // If a usable MJPEG stream was found, negotiate format/frame/interval.
    // Streaming (open endpoint + payload assembly) is added in the next stage.
    if(uvc_info.found)
        uvc_negotiate();
}

#endif // PIO_USB_HOST_ENABLE
