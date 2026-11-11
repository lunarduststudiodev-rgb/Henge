#ifndef HENGEVM_USB_VIDEO_H
#define HENGEVM_USB_VIDEO_H

#include <stddef.h>
#include <stdint.h>

#include "usb_storage.h" /* HvUsbSeg, HV_USB_NAK */

struct HvMachine;

/* A USB webcam: the USB Video Class (UVC 1.0) with one video function, the kind of camera macOS's own driver
   (UVCAssistant) takes as it is, so FaceTime, Photo Booth, Zoom and browsers list it. It plugs into the xHCI
   controller's camera port (xhci.c). High speed (USB 2.0), streaming over a bulk endpoint (1 IN): macOS's UVC
   driver streams from bulk pipes, and there is no isochronous scheduling to keep.

   It offers YUY2 (4:2:2, what every UVC driver knows) and NV12 (4:2:0, what macOS uses inside) at 1280x720
   (the default), 640x480 and 1920x1080, 30 frames a second. Frames come from the host (the iPad's front camera,
   or the Mac's) on any thread, in NV12 or BGRA at any size: they are cropped to the picture's shape (centred),
   scaled and converted to what macOS chose. Until the first one comes it sends black.

   It tells the host when macOS is streaming from it and when it stops (on_stream), so the real camera, and its
   light, are on only while something on the Mac uses the camera.

   Everything but hv_usb_video_frame runs on the CPU thread (hv_dev_lock held). */

typedef struct HvUsbVideo HvUsbVideo;

enum { HV_USB_VIDEO_NV12 = 0, HV_USB_VIDEO_BGRA = 1 }; /* what hv_usb_video_frame takes */
enum { HV_USB_VIDEO_FMT_YUY2 = 1, HV_USB_VIDEO_FMT_NV12 = 2 }; /* the formats macOS is offered (bFormatIndex) */

/* m: for hv_dma_written (may be NULL in tests). name: the product name macOS shows (e.g. "iPad Camera"). */
HvUsbVideo *hv_usb_video_create(struct HvMachine *m, const char *name);
void hv_usb_video_free(HvUsbVideo *v);
const char *hv_usb_video_name(HvUsbVideo *v);

/* Called on the CPU thread when macOS starts streaming (on 1: the picture it wants, width x height at fps frames a
   second) and stops (on 0). It must not block: start or stop the real camera on another thread. A new on 1 with
   no on 0 between says the picture changed. */
typedef void (*HvUsbVideoStream)(void *opaque, int on, int width, int height, int fps);
void hv_usb_video_set_callback(HvUsbVideo *v, HvUsbVideoStream on_stream, void *opaque);

/* A frame from the camera, any thread: NV12 (plane0 the Y plane, plane1 the CbCr plane, video range) or BGRA
   (plane0 only), width x height pixels, strides in bytes. It replaces the frame before it. 0, or -1 if it can't
   be taken (a size of 0, or larger than 8192). */
int hv_usb_video_frame(HvUsbVideo *v, int fmt, int width, int height, const uint8_t *plane0, size_t stride0,
                       const uint8_t *plane1, size_t stride1);

/* ---- for the controller (CPU thread) ---- */

/* A SETUP on endpoint 0 and, for a request that sends data (host to device), the data (data_len bytes) it sent.
   Returns the reply length (for a request that reads), 0, or -1 to stall. */
int hv_usb_video_control(HvUsbVideo *v, const uint8_t setup[8], const uint8_t *data, int data_len, uint8_t *out, int max);
int hv_usb_video_configured(HvUsbVideo *v);
/* Port or device reset, or unplugged: back to its default state, not streaming. */
void hv_usb_video_reset(HvUsbVideo *v);
/* The bulk endpoint's transfers were thrown away (its ring moved, an endpoint reset): the next starts a frame. */
void hv_usb_video_abort(HvUsbVideo *v);
/* A TD on the bulk IN endpoint, len bytes in segments, at now_ns (CLOCK_MONOTONIC). The bytes it was given
   (fewer than len ends it short: the end of a payload), or HV_USB_NAK: not now (no frame is due yet, or macOS
   hasn't committed to a format). */
int hv_usb_video_td(HvUsbVideo *v, const HvUsbSeg *seg, int nseg, uint32_t len, uint64_t now_ns);
/* Each tick: whether a TD is waiting on the bulk endpoint. It decides when macOS streams (and tells the host). */
void hv_usb_video_tick(HvUsbVideo *v, int td_waiting, uint64_t now_ns);
/* 1 while macOS streams from it (the host's camera should be on). */
int hv_usb_video_streaming(HvUsbVideo *v);

/* Suspend: what the guest negotiated with it. The frame itself and where a payload was are not kept: a resumed
   Mac gets a new frame. */
typedef struct HvUsbVideoState {
    int32_t configured, committed;
    uint8_t probe[26], commit[26];
    uint32_t fid;
} HvUsbVideoState;
void hv_usb_video_get_state(HvUsbVideo *v, HvUsbVideoState *st);
/* 0, or -1 (nothing changed) if it can't be this camera's. */
int hv_usb_video_set_state(HvUsbVideo *v, const HvUsbVideoState *st);

#endif
