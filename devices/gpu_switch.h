#ifndef HENGEVM_GPU_SWITCH_H
#define HENGEVM_GPU_SWITCH_H
/* The one check of the switches that keep Henge off the GPU, and the one way Henge asks for a Metal device.
   HV_NO_GPU keeps a host's GPU alone (the MacBook running tests: Metal work there froze it twice); HV_PVG_NO_METAL
   leaves the graphics device without its Metal side, and on the Mac without the ParavirtualizedGraphics device too
   (it runs on one). Either set: Henge's own code asks for no device (pvg_lite.c's ensure_metal, pvg_metal.m's
   hv_pvg_metal_create, pvg_mac.m's hv_pvg_attach); tests/test_gpu_switch checks it. The GPU tests (make test-gpu)
   make their own. */
#include <stdlib.h>

/* The switch that is set (its name, for the log), or NULL */
static inline const char *hv_gpu_switched_off(void) {
    return getenv("HV_NO_GPU") ? "HV_NO_GPU" : getenv("HV_PVG_NO_METAL") ? "HV_PVG_NO_METAL" : NULL;
}

#ifdef __OBJC__
#import <Metal/Metal.h>
/* The system's Metal device, or nil with either switch set */
static inline id<MTLDevice> hv_gpu_device(void) { return hv_gpu_switched_off() ? nil : MTLCreateSystemDefaultDevice(); }
#endif

#endif
