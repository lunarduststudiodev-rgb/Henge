/* HV_NO_GPU and HV_PVG_NO_METAL keep every way Henge has of making a Metal device from making one (devices/gpu_switch.h):
   the Mac's graphics device (pvg_mac.m hv_pvg_attach) and the Metal side (pvg_metal.m hv_pvg_metal_create; pvg_lite.c's
   ensure_metal checks the same switch before calling it). With either set, no device is asked for at all.
   No GPU can be reached here: this file defines MTLCreateSystemDefaultDevice and PGCreateDeviceWithDescriptor itself, so
   every call from Henge's code comes to them instead of the frameworks' (the Makefile fails the build if the binary
   still takes either), and they count the call and jump straight back, before anything is done with a device. Calls
   made inside the frameworks would still go to Metal's: the no-switch case below is safe because asking for the device
   is the first framework call each maker makes (pvg_mac.m makes nothing of ParavirtualizedGraphics before it). */
#include <objc/runtime.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

#include "pvg.h"
#include "pvg_lite.h"

static int checks, failures;
static void check(const char *what, long got, long want) {
    checks++;
    if (got != want) {
        printf("  FAIL %s: got %ld, want %ld\n", what, got, want);
        failures++;
    }
}

static unsigned devices_asked;
static jmp_buf back;
void *MTLCreateSystemDefaultDevice(void); /* Metal's, taken over: never returns */
void *MTLCreateSystemDefaultDevice(void) {
    devices_asked++;
    longjmp(back, 1);
}

/* The window (pvg_mac.m hv_pvg_window_run) without a device: AppKit's application and window makers are taken over the
   same way, so none is made here even if it asks */
static unsigned windows_asked;
static void *not_appkit(void *self, SEL cmd, ...) {
    (void)self, (void)cmd;
    windows_asked++;
    longjmp(back, 1);
}
static void take_over_appkit(void) {
    Method app = class_getClassMethod(objc_getClass("NSApplication"), sel_registerName("sharedApplication"));
    Method win = class_getInstanceMethod(objc_getClass("NSWindow"), sel_registerName("initWithContentRect:styleMask:backing:defer:"));
    if (!app || !win) { printf("  FAIL AppKit's makers not found\n"); exit(1); }
    method_setImplementation(app, (IMP)not_appkit);
    method_setImplementation(win, (IMP)not_appkit);
}

void *PGCreateDeviceWithDescriptor(void *descriptor); /* ParavirtualizedGraphics', taken over the same way */
void *PGCreateDeviceWithDescriptor(void *descriptor) {
    (void)descriptor;
    devices_asked++;
    longjmp(back, 1);
}

static void set(int no_gpu, int no_metal) {
    if (no_gpu) setenv("HV_NO_GPU", "1", 1); else unsetenv("HV_NO_GPU");
    if (no_metal) setenv("HV_PVG_NO_METAL", "1", 1); else unsetenv("HV_PVG_NO_METAL");
}

/* What each way of making a device does: its result, or 99 when it asked for a device */
static volatile long attach_result, metal_result;
static void attach(void) {
    attach_result = 99;
    if (!setjmp(back)) attach_result = hv_pvg_attach(NULL, NULL);
}
static void metal(void) {
    metal_result = 99;
    if (!setjmp(back)) metal_result = hv_pvg_metal_create(NULL) != NULL;
}

int main(void) {
    static const struct { int no_gpu, no_metal; const char *name; } on[] = {
        {1, 1, "both switches"}, {1, 0, "HV_NO_GPU"}, {0, 1, "HV_PVG_NO_METAL"},
    };
    char what[160];
    for (unsigned i = 0; i < sizeof on / sizeof on[0]; i++) {
        set(on[i].no_gpu, on[i].no_metal);
        unsigned before = devices_asked;
        attach();
        snprintf(what, sizeof what, "%s: hv_pvg_attach asks for no device", on[i].name);
        check(what, devices_asked - before, 0);
        snprintf(what, sizeof what, "%s: hv_pvg_attach fails (the iPad's graphics device stays)", on[i].name);
        check(what, attach_result, -1);
        snprintf(what, sizeof what, "%s: the framework's device isn't running, so a Suspend can be made", on[i].name);
        check(what, hv_pvg_running(), 0);
        before = devices_asked;
        metal();
        snprintf(what, sizeof what, "%s: hv_pvg_metal_create asks for no device", on[i].name);
        check(what, devices_asked - before, 0);
        snprintf(what, sizeof what, "%s: hv_pvg_metal_create gives no Metal side", on[i].name);
        check(what, metal_result, 0);
    }

    /* No device, so no window: it returns at once, with nothing asked of AppKit (its keys would go to a board the
       failed attach was never given) */
    take_over_appkit();
    volatile int returned = 0;
    if (!setjmp(back)) hv_pvg_window_run(), returned = 1;
    check("no device: hv_pvg_window_run returns", returned, 1);
    check("no device: hv_pvg_window_run asks AppKit for nothing", windows_asked, 0);

    /* Neither set: both ask, and the ask comes here (this file's stand-in is the one they call) */
    set(0, 0);
    unsigned before = devices_asked;
    attach();
    check("no switch: hv_pvg_attach asks for a device (this file's)", devices_asked - before, 1);
    before = devices_asked;
    metal();
    check("no switch: hv_pvg_metal_create asks for a device (this file's)", devices_asked - before, 1);
    set(1, 1);

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
