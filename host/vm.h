#ifndef HENGEVM_VM_H
#define HENGEVM_VM_H

#include <stddef.h>
#include <stdint.h>

#include "disk.h"
#include "vmapple.h"

/* The virtual Mac as a host app drives it: build it from files, run it, feed
   it input, stop it. The macOS command-line tool and the iOS app both use this;
   graphics are the host's own business (Mac: hv_pvg_attach before starting). */
typedef struct HvVmConfig {
    const char *rom;          /* AVPBooter.vmapple2.bin */
    const char *disk, *aux;   /* the root disk image and the auxiliary storage */
    uint64_t aux_offset;      /* where the aux image's data starts (16 KB) */
    uint64_t ecid, seed;      /* the machine identity the disk was installed with */
    uint64_t ram_mb;
    int ncpus;
    int log;                  /* m->log */
    int no_net, no_sound;
    int installer;            /* boot the ROM in DFU mode with the USB device port (installing macOS) */
    /* JIT memory the host already has (iOS: granted by a debugger), or NULL to map it here. */
    void *jit_rx, *jit_rw;
    size_t jit_bytes;         /* 0: 768 MB */
} HvVmConfig;

/* Returns 0, or -1 with a reason in err. */
int hv_vm_create(const HvVmConfig *cfg, char *err, size_t errlen);
/* The host's battery, which the Mac shows as its own when it was built with HV_BATTERY=1 (devices/power_source.h):
   percent 0..100, external 1 on a charger, charging 1 while it fills, full 1 on a charger and full. Any thread,
   any time, before hv_vm_create too: the last state is kept for the next Mac. */
void hv_vm_set_power(double percent, int external, int charging, int full);
/* After CPUs stop, detach backends and release the old machine. Borrowed iOS
   JIT memory stays owned by the app and can be attached to the next machine. */
void hv_vm_destroy(void);
HvMachine *hv_vm_machine(void);
HvVmapple *hv_vm_board(void);
HvCpu *hv_vm_cpu(int index);
extern void (*hv_prof_register)(void); /* debugging: each thread that will run guest code calls this */
/* Frames go to this CAMetalLayer, drawn by the GPU, instead of hv_vm_boot_display. */
void hv_vm_set_display_layer(void *layer);
uint64_t hv_vm_gpu_frames(void);
int hv_vm_ncpus(void);

/* The boot framebuffer iBoot hands the kernel (XNU's boot_args video info):
   the Apple logo and progress bar while booting or installing, before a GPU
   driver takes the display. Host pointer to the pixels (32-bit BGRA), or NULL
   until the kernel has its boot_args. */
typedef struct HvBootDisplay {
    const uint8_t *pixels;
    uint32_t width, height, stride;
} HvBootDisplay;
int hv_vm_boot_display(HvBootDisplay *d);

/* Extra device work on CPU 0 each tick (Mac graphics). Set before hv_vm_start. */
void hv_vm_set_poll(void (*poll)(void));

/* Runs CPU 0 on a new thread (unless the caller runs it: cpu0 = 0), the other
   CPUs and the 1 ms kicker. on_exit is called on CPU 0's thread when the
   machine stops (powered off, a fatal error, or hv_vm_stop), after the disks
   are flushed; reason is an HV_EXIT_* value. */
/* Returns -1 if a thread could not start; already-started workers are joined. */
int hv_vm_start(int cpu0, void (*on_exit)(int reason));
/* Call on a thread before it runs guest code (hv_vm_start's threads do): see vm.c. */
void hv_vm_prepare_cpu_thread(void);
/* That, and the scheduling class the guest's CPUs run at. */
void hv_vm_cpu_qos(void);
/* Run CPU 0 on the calling thread (what hv_vm_start does on its own thread); returns its exit reason. */
int hv_vm_run_cpu0(void);
/* Stop at the next block; the CPUs' threads end. */
void hv_vm_stop(void);
/* Instant resume (see snapshot.h). hv_vm_pause stops every CPU at a block boundary and quiets the devices (disk
   requests finished, the GPU's own memory copied back into the guest's, sound stopped), keeping the machine whole:
   0, or -1 if it stopped for good instead (shut down, or a CPU stopped it at the same moment). A machine whose CPU 0
   the caller runs (hv_vm_start(0, ...)) calls hv_vm_finish_pause on that thread once hv_vm_run_cpu0 returns with
   halted HV_HALT_SAVE. hv_vm_continue runs it on from where it stopped. */
int hv_vm_pause(void);
int hv_vm_finish_pause(void);
int hv_vm_paused(void);
int hv_vm_continue(void);
int hv_vm_pause_clean(void); /* paused with no disk request in flight: it can be saved */
uint64_t hv_vm_paused_counter(void); /* the guest's virtual counter when it paused */
/* The counter to go on from after a pause or a restore made at wall_ns (CLOCK_REALTIME): see vm.c. */
uint64_t hv_vm_resume_counter(uint64_t counter, int64_t wall_ns);
int64_t hv_vm_paused_wall_ns(void);
/* After a resume (or a long pause): the network link down for 2 s, 1.5 s from now. */
void hv_vm_link_flap(void);
void hv_vm_flush_disks(void);
/* The disk and auxiliary storage files as they are now (inode, size, last change): a saved machine resumes only onto these. */
int hv_vm_disk_identity(char *out, size_t n);
const char *hv_vm_reason_name(int reason);

/* Input from the host UI, any thread. Keys are Linux input codes (KEY_A = 30). */
void hv_vm_key(uint16_t code, int down);
void hv_vm_key_hid(uint8_t usage, int down); /* HID keyboard usage, which is what iOS reports for a hardware keyboard */
void hv_vm_mouse_move(int dx, int dy);
void hv_vm_mouse_button(int button, int down); /* 0 left, 1 right, 2 middle */
void hv_vm_mouse_wheel(int dy);
void hv_vm_mouse_hwheel(int dx);
/* The Magic Trackpad 2 on the USB bus: a finger on it. phase: 0 down, 1 move, 2 up; id a
   stable 1..15 per touch; x, y in sensor units (HV_TP_SENSOR_* in devices/xhci.h).
   ts_ms is the touch sample's own clock in ms (see hv_xhci_touch). */
void hv_vm_touch(int id, int phase, int x, int y, uint32_t ts_ms);
void hv_vm_touch_frame(uint32_t ts_ms); /* commit one frame with every finger that moved */
void hv_vm_touch_button(int down); /* the pad's full press (a click) */
/* The absolute pointing device (HV_TABLET=1): the pointer is at px, py in display pixels. The host
   draws the pointer there at once, and the guest is told the place rather than a movement. */
int hv_vm_tablet_on(void);
void hv_vm_tablet_move(int px, int py);
void hv_vm_tablet_button(int button, int down); /* 0 left, 1 right, 2 middle */
/* Another device has the pointer now: buttons up, and the pointer is drawn where the guest has it. */
void hv_vm_tablet_release(void);
/* The size macOS was told its screen is, in display pixels. */
void hv_vm_screen(int *w, int *h);
/* Where the guest has its pointer, in display pixels. 0, or -1 before it has one. */
int hv_vm_pointer(int *x, int *y);
/* A small picture of the display, BGRA, width * height * 4 bytes. 0, or -1 before anything shows. */
int hv_vm_snapshot(uint8_t *bgra, int width, int height);

#endif
