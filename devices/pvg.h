#ifndef HENGEVM_PVG_H
#define HENGEVM_PVG_H

#include "vmapple.h"

/* Mac-only: backs the vmapple gfx and iosurface-mapper devices with
   ParavirtualizedGraphics.framework, as QEMU's apple-gfx does. iOS has no such
   framework; this exists so the boot can proceed on the Mac, and so the
   device's behaviour can be observed for HengeVM's own implementation later.
   screenshot: PNG path written every few seconds while frames arrive (or NULL).
   Call before the main thread enters its run loop: the framework delivers
   display events on the main queue, so the CPU must run on another thread.
   Returns 0 on success. */
int hv_pvg_attach(HvVmapple *b, const char *screenshot);

/* Main thread, instead of its run loop: a window showing the display, with the
   host keyboard and mouse going to the guest's virtio-input devices. Does not return,
   unless hv_pvg_attach didn't succeed: then there is no window, and it returns at once. */
void hv_pvg_window_run(void);

/* The framework's device is running (hv_pvg_attach succeeded). A Suspend can't carry its state, so none is made
   then; asked for and not running (no device, or HV_NO_GPU / HV_PVG_NO_METAL set), the iPad's graphics device
   handles the guest and a Suspend can be made. */
int hv_pvg_running(void);

/* Called from the CPU thread: delivers interrupts the framework raised. */
void hv_pvg_poll(void);

#endif
