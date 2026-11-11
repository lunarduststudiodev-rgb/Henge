#ifndef HENGEVM_VMAPPLE_H
#define HENGEVM_VMAPPLE_H

#include <pthread.h>
#include <stdint.h>

#include "aes.h"
#include "bdif.h"
#include "cfg.h"
#include "gicv3.h"
#include "pcie.h"
#include "pl011.h"
#include "pl061.h"
#include "pl031.h"
#include "virtio_blk.h"
#include "virtio_fs.h"
#include "virtio_input.h"
#include "virtio_usb.h"
#include "xhci.h"
#include "virtio_net.h"
#include "virtio_snd.h"
#include "power_source.h"
#include "virtio_balloon.h"

/* The virtual Mac: QEMU's vmapple memory map and interrupt numbers (hw/vmapple/vmapple.c). */
#define VMAPPLE_FIRMWARE   0x00100000ull /* 1 MB: the boot ROM */
#define VMAPPLE_CONFIG     0x00400000ull
#define VMAPPLE_GIC_DIST   0x10000000ull
#define VMAPPLE_GIC_REDIST 0x10010000ull
#define VMAPPLE_GIC_MSI    0x1fff0000ull /* third gic reg in the guest's device tree; not in QEMU */
#define VMAPPLE_UART       0x20010000ull
#define VMAPPLE_RTC        0x20050000ull
#define VMAPPLE_GPIO       0x20060000ull
#define VMAPPLE_PVPANIC    0x20070000ull
#define VMAPPLE_BDOOR      0x30000000ull
#define VMAPPLE_GFX        0x30200000ull /* paravirtualized graphics */
#define VMAPPLE_IOSFC      0x30210000ull
#define VMAPPLE_AES_1      0x30220000ull
#define VMAPPLE_AES_2      0x30230000ull
#define VMAPPLE_RTC_SELECT 0x30240000ull /* not in QEMU's map; see vmapple.c */
#define VMAPPLE_CTRR       0x30260000ull /* macOS 27: avp-ctrr, the kernel text lock's information and registers */
#define VMAPPLE_SEALED     0x30270000ull /* macOS 27: avp-sealed-registers */
#define VMAPPLE_PCIE_ECAM  0x40000000ull
#define VMAPPLE_PCIE_MMIO  0x50000000ull
#define VMAPPLE_MEM        0x70000000ull

/* SPI numbers; the INTID is 32 more. */
#define VMAPPLE_IRQ_UART 1
#define VMAPPLE_IRQ_RTC 2
#define VMAPPLE_IRQ_GPIO 5
#define VMAPPLE_IRQ_AES 0x12
#define VMAPPLE_IRQ_PCIE 0x20
#define VMAPPLE_IRQ_AVP_RTC 0x13 /* the Apple virtual RTC's "time changed" line (Virtualization.framework: INTID 0x33) */

/* hv_vmapple_rtc_event bits, as Virtualization.framework sets them. */
#define HV_AVP_RTC_WOKE     2 /* the host slept (here: the Mac was paused); macOS rereads its clock */
#define HV_AVP_RTC_RESTORED 4 /* the machine was restored from a saved state */
#define VMAPPLE_AVP_REGS 0x10000 /* the avp-ctrr and avp-sealed-registers windows */
#define VMAPPLE_MSI_BASE 96   /* INTIDs handed out through the MSI frame */
#define VMAPPLE_MSI_COUNT 64

typedef struct HvVmapple {
    uint64_t avp_rtc[8];     /* Apple virtual RTC registers +0x00..+0x38 ([4] interrupt mask, [5] status; see vmapple.c) */
    pthread_mutex_t avp_rtc_lock;
    HvMachine *m;
    HvCpu *cpu;
    int ncpus;
    HvGic *gic;
    HvPl011 *uart;
    HvPl031 *rtc;
    HvPl061 *gpio;
    HvPcie *pcie;
    HvBdif *bdif;
    HvAes *aes;
    struct HvPvgLite *pvg;   /* our own graphics device (the Mac replaces it with the framework) */
    HvVirtioBlk *aux_blk, *root_blk;
    HvVirtioNet *net;
    HvVirtioSnd *snd;
    HvVirtioInput *keyboard, *mouse, *tablet; /* tablet: the absolute pointer touch drives */
    HvVirtioFs *fs;          /* shared folders, mounted by macOS as "My Shared Files" (optional, added last) */
    HvVirtioBalloon *balloon; /* HV_BALLOON=MB (the first target; 0 = none yet) adds it; off by default until tried on a phone */
    HvXhci *usb;             /* USB controller with the keyboard macOS actually uses */
    HvVirtioUsb *vusb;       /* the USB device port an installer talks to (installer boots only) */
    HvPowerSource *power;    /* Apple's virtual battery or wall power (optional, hv_vmapple_add_power_source) */
    struct HvUsbVideo *camera; /* the webcam on usb's port 8 (not on installer boots, nor with HV_NO_CAMERA) */
    uint8_t *cfg, *rom, *ram;
    uint64_t ram_size;
    int ram_fd;              /* the file under guest RAM, kept open for the balloon's holes, or -1 */
    uint64_t panic_events;   /* last pvpanic event the guest reported */
    uint8_t *avp_ctrr, *avp_sealed; /* macOS 27's avp-ctrr and avp-sealed-registers: VMAPPLE_AVP_REGS bytes each */
    int snd_restart;                /* Suspend: sound was playing when saved; hv_vmapple_restored starts it */
    void (*poll)(void);      /* extra device work on the CPU thread each tick (Mac PVG) */
    char save_why[200];      /* Suspend: why the last hv_vmapple_snap_save failed, a sentence for the person */
    struct HvVmappleCapture *capture; /* Suspend: the screen's state read ahead (hv_vmapple_snap_capture), or NULL */
} HvVmapple;

/* Builds the machine around m and cfg->nr_cpus CPUs (cpus is an array of them). Disks may be NULL (no device then).
   Guest RAM is reserved, not committed: pages are only used once touched. */
int hv_vmapple_create(HvVmapple *b, HvMachine *m, HvCpu *cpus, uint64_t ram_size, HvDisk *root, HvDisk *aux,
                      const HvCfgParams *cfg, const uint8_t mac[6]);
/* After hv_vmapple_create, before the guest runs: host folders the Mac sees in "My Shared Files", each
   under its own name. NULL paths are skipped. Returns how many were shared, or -1 if the device can't be made. */
int hv_vmapple_share_folders(HvVmapple *b, const char *const names[], const char *const paths[], const int readonly[], int n);
/* After hv_vmapple_create and before hv_vmapple_share_folders (so the devices before it keep their PCI slots):
   Apple's virtual power source, HV_POWER_BATTERY or HV_POWER_WALL; feed it with hv_power_source_set(b->power, ...).
   Returns 0, or -1 if it can't be made. */
int hv_vmapple_add_power_source(HvVmapple *b, int kind);
/* Tells macOS its clock may be wrong (HV_AVP_RTC_* bits): it rereads the time and sets it. Any thread. */
void hv_vmapple_rtc_event(HvVmapple *b, unsigned bits);
/* Copies a boot ROM image into the firmware region. */
int hv_vmapple_load_rom(HvVmapple *b, const char *path);
/* CPU reset: EL1, MMU off, interrupts masked, at the start of the ROM. */
void hv_vmapple_reset_cpu(HvVmapple *b);

/* Suspend (vmapple_snap.c, core/snapshot.h): the board's devices and its GIC in a snapshot.
   can_save: 0 if this board's state can be saved now, else -1 and why (a USB drive is plugged in,
   an installer boot, ...). Saving needs the machine paused (hv_machine_pause_wait); it first lets
   every disk and shared-folder request under way finish and reach the guest, so it changes guest RAM:
   save the RAM after it. Loading is into a board built the same way (the same devices, disks of the
   same size) whose CPUs are not running. After a load, before the CPUs go: hv_vmapple_restored. */
struct HvSnapWriter;
struct HvSnapReader;
/* Storage the RAM's part of a save may take, at most, were it made now (paused): kept in files, what they need
   (core/ramfile.h hv_ramfile_save_need); otherwise the whole RAM. */
uint64_t hv_vmapple_save_ram_room(const HvVmapple *b);
int hv_vmapple_snap_can_save(HvVmapple *b, char *why, size_t why_len);
int hv_vmapple_snap_save(HvVmapple *b, struct HvSnapWriter *w);
/* After hv_vmapple_snap_save failed: why, a sentence for the person. */
const char *hv_vmapple_snap_save_why(HvVmapple *b);
/* The screen's state read ahead of a save, with the machine paused and staying paused: what only the GPU holds
   into memory (at most limit bytes of it), and the device's own state. A later hv_vmapple_snap_save writes this
   instead of reading the GPU then: on iOS the GPU can only be used until Henge has finished leaving the screen,
   and the save often runs after that. Valid only while the guest doesn't run: drop it before it goes on (a
   capture from before would be out of date). Waits for the GPU's work up to deadline_ns (CLOCK_MONOTONIC).
   One already taken (since the pause) is kept: 0 at once.
   0 captured; -1 not (why says; nothing is kept, and the save reads the GPU itself). */
int hv_vmapple_snap_capture(HvVmapple *b, uint64_t limit, uint64_t deadline_ns, char *why, size_t why_len);
void hv_vmapple_snap_drop_capture(HvVmapple *b);
int hv_vmapple_snap_load(HvVmapple *b, struct HvSnapReader *r);
/* Before a load changes anything: can this snapshot go into this board as it is now (no drive in yet, and each
   USB drive the saved Mac had plugged in again unchanged)? 0, or -1 with a sentence for the person in why. */
int hv_vmapple_snap_check(HvVmapple *b, struct HvSnapReader *r, char *why, size_t why_len);
/* Whether the saved Mac had a power source (battery or wall): 1, or 0 (saved by a Henge from before it). */
int hv_vmapple_snap_has_power(struct HvSnapReader *r);
/* The machine was restored: let go of every key and button the guest still thinks held (nobody is
   holding them now), and tell macOS its clock is behind (HV_AVP_RTC_RESTORED). */
void hv_vmapple_restored(HvVmapple *b);

#endif
