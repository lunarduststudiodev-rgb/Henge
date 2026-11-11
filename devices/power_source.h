#ifndef HENGEVM_POWER_SOURCE_H
#define HENGEVM_POWER_SOURCE_H

#include <pthread.h>
#include <stdint.h>

#include "virtio.h"

/* Apple's virtual Mac power source (PCI 0x106b:0x1a07): the battery, or the
   wall power, that Virtualization.framework gives a macOS guest with
   _VZMacBatteryPowerSourceDeviceConfiguration / _VZMacWallPowerSourceDevice.
   macOS's own driver, AppleVirtualPlatformPowerSource (AppleVirtualPlatform.kext,
   matched by IOVirtIOPrimaryMatch 0x1a07106b), turns what it says into an
   IOPMPowerSource: the menu-bar battery, Battery settings, low-battery warnings,
   and everything that runs "only on battery".

   The protocol, read from that driver (macOS 15.6.1's virtual-Mac kernel):
   - virtio 1.x, VIRTIO_F_VERSION_1 required, nothing else negotiated; one queue.
   - Device config byte 0: the kind of source, HV_POWER_BATTERY (0) or
     HV_POWER_WALL (1); anything else and the driver gives up. Only a battery
     sets BatteryInstalled.
   - The driver keeps one buffer posted: 8 bytes it doesn't fill (device
     readable) then 12 bytes for the answer (device writable). The device holds
     it until it has news, writes the answer, and gives it back; the driver
     takes the news and posts the buffer again.
   - The answer, packed: u8 type (1, "status"); f64 charge in percent, 0 to 100
     (the driver reports CurrentCapacity = charge * 100 of MaxCapacity 10000,
     and drops anything outside 0..100); u8 ExternalConnected; u8 IsCharging;
     u8 FullyCharged.
   There is no Low Power Mode or time-remaining field: macOS works those out
   itself (its own Low Power Mode setting "only on battery" follows the
   ExternalConnected given here). */
#define HV_POWER_BATTERY 0
#define HV_POWER_WALL 1

#define HV_POWER_MSG_STATUS 1
#define HV_POWER_MSG_LEN 12

typedef struct HvPowerState {
    double percent;    /* 0..100 */
    uint8_t external;  /* on a charger (or the wall) */
    uint8_t charging;  /* the battery is filling */
    uint8_t full;      /* the charger is in and the battery is full */
} HvPowerState;

typedef struct HvPowerSource {
    HvVirtio vio;
    int kind;                 /* HV_POWER_* */
    pthread_mutex_t mu;       /* want/want_gen: set from any thread */
    HvPowerState want;
    uint64_t want_gen;
    /* CPU thread (the device lock) only */
    HvPowerState sent;        /* last state the guest was given */
    uint64_t sent_gen;
    int resend;               /* give the guest the state again even if it hasn't changed (after a load) */
    uint64_t messages;
} HvPowerSource;

HvPowerSource *hv_power_source_create(HvMachine *m, HvPcie *bus, int kind);
/* The host's battery as it is now. Any thread; the guest hears of it at the next poll. */
void hv_power_source_set(HvPowerSource *p, const HvPowerState *s);
/* CPU thread, each tick (with the device lock): tell the guest of a change, if it has a buffer waiting. */
void hv_power_source_poll(HvPowerSource *p);
/* The answer for a state, as the driver reads it (exposed for the tests). */
void hv_power_source_encode(const HvPowerState *s, uint8_t out[HV_POWER_MSG_LEN]);

#endif
