#ifndef HENGEVM_CAMERA_APPLE_H
#define HENGEVM_CAMERA_APPLE_H

#include "usb_video.h"

/* The host's camera behind the virtual Mac's USB camera (devices/usb_video.c): the iPad's front camera, or the
   Mac's default camera. It runs only while macOS streams from the USB camera (so its light, and iOS's camera
   indicator, are on only then), and asks for permission the first time. Without permission, or with no camera,
   macOS gets a black picture. */
void hv_camera_attach(HvUsbVideo *v);
/* The machine is pausing (1) or going on (0): the camera is off while it is paused. */
void hv_camera_pause(int pause);

#endif
