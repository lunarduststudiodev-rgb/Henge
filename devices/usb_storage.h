#ifndef HENGEVM_USB_STORAGE_H
#define HENGEVM_USB_STORAGE_H

#include <stdint.h>

#include "disk.h"

struct HvMachine;

/* A USB drive: USB mass storage, bulk-only transport (BOT), with a SCSI disk
   behind it, the way a USB stick or an external SSD shows itself. It plugs
   into the xHCI controller (xhci.c) at any time, so the Mac sees a drive
   arrive and go while it runs, as Virtualization.framework's USB mass
   storage devices do. It is high speed (USB 2.0): bulk endpoint 1 IN and 2 OUT.

   It reports removable media, so macOS asks it to eject when the drive is
   ejected on the Mac (Finder, diskutil): the drive then reads as ejected
   (hv_usb_storage_ejected) and can be unplugged with nothing lost.

   The xHCI hands it one transfer descriptor at a time, as the segments of
   guest memory the TD covers. Disk reads and writes run on its own thread,
   straight between the file and the guest's buffers; everything else runs on
   the CPU thread. */

#define HV_USB_NAK (-1)     /* not now: the TD stays queued until the drive wants it */
#define HV_USB_PENDING (-2) /* taken: the drive's thread is on it (hv_usb_storage_done) */

typedef struct HvUsbSeg {
    void *p;      /* host pointer to the guest buffer */
    uint64_t pa;  /* its guest address (for hv_dma_written) */
    uint32_t len;
} HvUsbSeg;

typedef struct HvUsbStorage HvUsbStorage;

/* Opens the image at path (read-only if readonly, or if it cannot be written). name is what the
   Mac calls the device (16 characters at most show), serial its USB serial number, which names the
   drive to the helper. NULL if the file can't be opened. */
HvUsbStorage *hv_usb_storage_open(struct HvMachine *m, const char *path, int readonly, const char *name, const char *serial);
/* Waits for the drive's thread, writes what it holds to the file and closes it. */
void hv_usb_storage_free(HvUsbStorage *s);

/* Its descriptors and requests on endpoint 0. Returns the reply length, or -1 to stall. */
int hv_usb_storage_control(HvUsbStorage *s, const uint8_t setup[8], uint8_t *out, int max);
int hv_usb_storage_configured(HvUsbStorage *s);
/* The device went back to its default state (port or device reset): no transfer is under way. */
void hv_usb_storage_reset(HvUsbStorage *s);

/* CPU thread: a TD on the bulk IN (in = 1) or OUT endpoint, len bytes in segments. Returns the bytes
   moved (the TD is done), HV_USB_NAK or HV_USB_PENDING. */
int hv_usb_storage_td(HvUsbStorage *s, int in, const HvUsbSeg *seg, int nseg, uint32_t len);
/* CPU thread: once the drive's thread is done, 1 and the bytes the pending TD moved; else 0. */
int hv_usb_storage_done(HvUsbStorage *s, uint32_t *moved);
/* Any thread: 1 while the drive's thread works (a TD or a cache flush). */
int hv_usb_storage_busy(HvUsbStorage *s);

/* Any thread. */
int hv_usb_storage_ejected(HvUsbStorage *s); /* macOS ejected it */
int hv_usb_storage_readonly(HvUsbStorage *s);
uint64_t hv_usb_storage_size(HvUsbStorage *s);
/* What it has been written: 0 now in the host's cache (sync 0) or on its storage (sync 1); -1. */
int hv_usb_storage_flush(HvUsbStorage *s, int sync);
const char *hv_usb_storage_serial(HvUsbStorage *s);
const char *hv_usb_storage_name(HvUsbStorage *s);
/* When its file last changed (ns since 1970), so a saved state only goes back onto the same file as it was; -1. */
int hv_usb_storage_mtime(HvUsbStorage *s, int64_t *ns);

/* Suspend: what a drive holds between TDs (state and the command under way). */
typedef struct HvUsbStorageState {
    int32_t configured, ejected, state, host_in, io;
    uint32_t tag, host_len, host_done, resp_pos;
    uint64_t disk_off, dev_left;
    uint8_t status, sense_key, asc, ascq;
    uint8_t resp[256];
} HvUsbStorageState;
/* CPU thread, not busy: 0, or -1 while its thread works. */
int hv_usb_storage_get_state(HvUsbStorage *s, HvUsbStorageState *st);
/* CPU thread, not busy: 0, or -1 (and nothing changed) if the state can't be this drive's. */
int hv_usb_storage_set_state(HvUsbStorage *s, const HvUsbStorageState *st);

#endif
