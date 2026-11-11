#ifndef HENGEVM_VMAPPLE_H
#define HENGEVM_VMAPPLE_H

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
#include "virtio_input.h"
#include "virtio_usb.h"
#include "power_source.h"
#include "xhci.h"
#include "virtio_net.h"
#include "virtio_snd.h"
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
#define VMAPPLE_MSI_BASE 96   /* INTIDs handed out through the MSI frame */
#define VMAPPLE_MSI_COUNT 64

typedef struct HvVmapple {
    uint64_t avp_rtc[8];     /* Apple virtual RTC control registers +0x00..+0x38 (only +0x18, +0x20, +0x38 are written) */
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
    HvVirtioBalloon *balloon; /* HV_BALLOON=MB (the first target; 0 = none yet) adds it; off by default until tried on a phone */
    HvVirtioInput *keyboard, *mouse;
    HvXhci *usb;             /* USB controller with the keyboard macOS actually uses */
    HvVirtioUsb *vusb;       /* the USB device port an installer talks to (installer boots only) */
    HvPowerSource *power;    /* Apple's virtual battery or wall power (optional, hv_vmapple_add_power_source) */
    uint8_t *cfg, *rom, *ram;
    uint64_t ram_size;
    int ram_fd;              /* the file under guest RAM, kept open for the balloon's holes, or -1 */
    uint64_t panic_events;   /* last pvpanic event the guest reported */
    void *ctrr, *sealed;
    pthread_t prefault_thread;
    int prefault_started;
    void (*poll)(void);      /* extra device work on the CPU thread each tick (Mac PVG) */
} HvVmapple;

/* Builds the machine around m and cfg->nr_cpus CPUs (cpus is an array of them). Disks may be NULL (no device then).
   Guest RAM is reserved, not committed: pages are only used once touched. */
int hv_vmapple_create(HvVmapple *b, HvMachine *m, HvCpu *cpus, uint64_t ram_size, HvDisk *root, HvDisk *aux,
                      const HvCfgParams *cfg, const uint8_t mac[6]);
/* After hv_vmapple_create and before the guest runs, once: Apple's virtual power source, HV_POWER_BATTERY or
   HV_POWER_WALL, in the next PCI slot (after every other device, so none of theirs move). Feed it with
   hv_power_source_set(b->power, ...). 0, or -1. */
int hv_vmapple_add_power_source(HvVmapple *b, int kind);
/* Copies a boot ROM image into the firmware region. */
int hv_vmapple_load_rom(HvVmapple *b, const char *path);
/* CPU reset: EL1, MMU off, interrupts masked, at the start of the ROM. */
void hv_vmapple_reset_cpu(HvVmapple *b);
/* No CPU or host device callback may be using the board. */
void hv_vmapple_destroy(HvVmapple *b);
uint8_t *hv_vmapple_avp_regs(void *avp);

#endif
