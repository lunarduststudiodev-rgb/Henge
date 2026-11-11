#ifndef HENGEVM_XHCI_H
#define HENGEVM_XHCI_H

#include <pthread.h>

#include "pcie.h"
#include "usb_video.h"

/* An xHCI USB host controller (xHCI 1.0, one interrupter, 32-byte contexts,
   INTx) with a USB HID boot keyboard on port 1: what Virtualization.framework
   gives a macOS guest as its "Virtual USB Keyboard", and so the keyboard path
   macOS takes. Key changes come from the host UI on any thread
   (hv_xhci_key); the CPU thread delivers them (hv_xhci_poll).

   Ports 2 to 5 take USB drives (usb_storage.c), plugged in and pulled out
   while the Mac runs, as on a real Mac: macOS sees a drive arrive, mounts it,
   and ejects it before it goes.

   Port 6 takes a USB HID touchscreen (10 contacts), plugged in by the host
   only when the guest can take it as one: macOS 27 or later, with Henge Tools
   having turned on WindowServer's external-touchscreen support. Otherwise
   it stays empty and the guest never sees it.

   Port 7 has a USB Audio Class 2.0 speaker (usb_audio.c), always there: its
   PCM comes on an isochronous endpoint, and its volume is the host's (the
   iPad's own volume). Isochronous transfers run at the end of the 1 ms frame
   they were queued for (MFINDEX counts the guest's time), or as soon as the
   CPU thread gets to them after it.
   Port 8 takes a USB webcam (usb_video.c): the host's camera (the iPad's front camera), which macOS's own
   UVC driver takes. High speed, streaming over bulk. */
#define HV_XHCI_SLOTS 8
#define HV_XHCI_PORTS 5  /* 1 the keyboard, 2-5 drives (what the snapshot's port arrays hold) */
#define HV_XHCI_TOUCH_PORT 6
#define HV_XHCI_TOUCH_PORTS 6 /* the ports there were when the touchscreen came */
#define HV_XHCI_SPEAKER_PORT 7
#define HV_XHCI_CAMERA_PORT 8
#define HV_XHCI_ROOT_PORTS 8 /* the controller's ports: those, the touchscreen, the speaker and the camera */
#define HV_XHCI_CONTACTS 10
#define HV_XHCI_TOUCH_REPORT 62 /* report ID, 10 x (tip/range/confidence, ID, X, Y), contact count */
#define HV_XHCI_INTRS 4
#define HV_XHCI_EPS 32 /* device context index 1..31 */
#define HV_XHCI_REPORTS 256
#define HV_XHCI_TD_MAX 256 /* TRBs in one transfer descriptor */

struct HvUsbStorage;
struct HvUsbAudio;
struct HvUsbVideo;

typedef struct HvXhciRing {
    uint64_t dequeue; /* guest PA of the next TRB */
    int ccs;          /* consumer cycle state */
} HvXhciRing;

/* A transfer descriptor: TRBs chained together, as one transfer. */
typedef struct HvXhciTd {
    int n;
    uint64_t at[HV_XHCI_TD_MAX];
    uint32_t trb[HV_XHCI_TD_MAX][4];
    uint32_t len;      /* bytes its Normal TRBs hold */
    HvXhciRing after;  /* the ring past it */
} HvXhciTd;

typedef struct HvXhciSlot {
    int port;          /* root hub port (1-based) it was addressed on; 0 none, or its device has gone */
    HvXhciRing ep[HV_XHCI_EPS];
    int ep_running[HV_XHCI_EPS];
    int pend_dci;      /* a drive's thread works on td for this endpoint */
    HvXhciTd td;
    /* Isochronous endpoints: the frame (1 ms, counted from the guest's time) a TD that says "as soon as
       possible" goes in, past the last one's; iso_valid 0 when none ran yet (after a stop or a new ring). */
    int64_t iso_next[HV_XHCI_EPS];
    uint8_t iso_valid[HV_XHCI_EPS];
    uint8_t ep_iso[HV_XHCI_EPS]; /* the endpoint is isochronous (its context's type) */
} HvXhciSlot;

/* What a drive's port is doing, for the host UI. */
enum {
    HV_XHCI_DRIVE_EMPTY = 0,
    HV_XHCI_DRIVE_PLUGGED = 1, /* there, macOS hasn't taken it (yet) */
    HV_XHCI_DRIVE_IN_USE = 2,  /* macOS took it */
    HV_XHCI_DRIVE_EJECTED = 3, /* macOS ejected it: safe to unplug */
    HV_XHCI_DRIVE_LEAVING = 4, /* being unplugged */
};

typedef struct HvXhci {
    HvPciDev pci;
    HvMachine *m;
    /* operational */
    uint32_t usbcmd, usbsts, dnctrl, config;
    uint64_t dcbaap;
    HvXhciRing cmd;
    int cmd_running;
    uint32_t portsc[HV_XHCI_PORTS];
    /* interrupters and their event rings (macOS's driver wants at least three) */
    struct HvXhciIntr {
        uint32_t iman, imod, erstsz;
        uint64_t erstba, erdp;
        uint64_t seg_base;  /* current event ring segment */
        uint32_t seg_size, seg_index, index;
        int pcs;
    } intr[HV_XHCI_INTRS];
    int slot_used[HV_XHCI_SLOTS + 1];
    HvXhciSlot *slots;     /* HV_XHCI_SLOTS + 1 of them, by slot ID */
    int kbd_slot;          /* slot the keyboard was addressed in, or 0 */
    int configured;        /* the keyboard's configuration */
    /* drives, by port index (1..HV_XHCI_PORTS-1); drive and slot belong to the CPU thread, changed under mu */
    struct HvXhciPort {
        struct HvUsbStorage *drive;
        int slot;
        struct HvUsbStorage *plug; /* under mu: waiting to be connected */
        int unplug;                /* under mu: asked to go */
    } port[HV_XHCI_PORTS];
    int changes;           /* atomic: plug or unplug waiting */
    /* keyboard reports waiting for an IN transfer (8 bytes each) */
    pthread_mutex_t mu;
    uint8_t reports[HV_XHCI_REPORTS][8];
    uint32_t rhead, rtail;
    uint8_t state[8];      /* modifiers, reserved, six keys: the current report */
    uint64_t delivered;
    /* Ports the guest's driver was told about (HCSPARAMS1, read when it starts the controller):
       HV_XHCI_ROOT_PORTS, or fewer for a Mac resumed from a snapshot older than the touchscreen
       (HV_XHCI_PORTS) or the speaker (HV_XHCI_TOUCH_PORTS), for as long as it runs (a driver that
       resets the controller may not read it again; a restart makes a new controller). Each needs
       its port to be among them. */
    int nports;
    /* The touchscreen on port 6. slot, configured and portsc belong to the CPU thread;
       want/w_mm/h_mm and the report queue are under mu. */
    struct HvXhciTouch {
        int present;       /* connected (what the CPU thread applied) */
        int want;          /* under mu: -1 nothing asked, 0 unplug, 1 plug */
        int w_mm, h_mm;    /* the screen's size: the report descriptor's physical extent */
        uint32_t portsc;
        int slot;
        int configured;
        uint8_t desc[1024]; /* its HID report descriptor, built at plug time */
        int desc_len;
        uint8_t cur[HV_XHCI_TOUCH_REPORT]; /* the last report: what GET_REPORT answers */
        uint8_t q[HV_XHCI_REPORTS][HV_XHCI_TOUCH_REPORT];
        uint32_t qhead, qtail;
        int lost;          /* under mu (read atomically): a frame was dropped while plugged in, so a finger may
                              be down in macOS that has lifted here: every finger is lifted once it can be */
        /* Under mu: what macOS has been given (the reports delivered): which fingers it has down, and where
           each finger was last. A lift of every finger lifts those, where they are, so none is dropped or
           tapped anywhere else. */
        uint16_t mac_down;          /* bit per contact ID */
        uint16_t mac_x[HV_XHCI_CONTACTS], mac_y[HV_XHCI_CONTACTS];
        int mac_unknown;            /* resumed from a snapshot that didn't say: where they are isn't known, so
                                       they are lifted as cancelled (Confidence 0), not as touches somewhere */
        uint64_t delivered;
    } touch;
    /* The speaker on port 7. The CPU thread's: present is 0 for a Mac whose driver doesn't know port 7 (resumed
       from an older snapshot). */
    struct HvXhciSpeaker {
        struct HvUsbAudio *dev;
        int present;
        uint32_t portsc;
        int slot;
        int out_pending;     /* a control OUT request waits for its data stage */
        uint8_t out_setup[8];
    } spk;
    /* Isochronous scheduling (CPU thread): endpoints with TDs waiting, and when the first one is due (the
       guest's counter, 24 MHz; 0 none). MFINDEX wrap events, when the driver asks for them (USBCMD.EWE). */
    uint64_t iso_due;
    uint64_t mf_wraps;     /* MFINDEX wraps seen (the counter's 2.048 s periods) */
    /* The camera on port 8. present, slot and portsc belong to the CPU thread (present is read on any);
       want is under mu. */
    struct HvXhciCam {
        struct HvUsbVideo *dev; /* NULL: no camera on this machine */
        int present;
        int want;               /* under mu: -1 nothing asked, 0 unplug, 1 plug */
        uint32_t portsc;
        int slot;
    } cam;
    uint32_t idle_portsc;  /* a port with nothing on it (7) */
} HvXhci;

HvXhci *hv_xhci_create(HvMachine *m, HvPcie *bus);
/* Any thread: a key went down or up (HID usage page 7 usage, e.g. 4 = A; 0xe0-0xe7 are modifiers). */
void hv_xhci_key(HvXhci *x, uint8_t usage, int down);
/* Any thread: every key up (after a restore: nobody holds them now). */
void hv_xhci_release_keys(HvXhci *x);
/* Any thread: ⇧, ⌃, ⌥ and ⌘ up, whoever holds them (a report only if one was down). */
void hv_xhci_release_modifiers(HvXhci *x);
/* The same, by Linux input key code (what the host UI produces). */
void hv_xhci_key_linux(HvXhci *x, uint16_t code, int down);
/* CPU thread: hand waiting reports to the guest, connect and disconnect drives, finish their transfers, run
   the isochronous TDs that are due. */
void hv_xhci_poll(HvXhci *x);
/* CPU thread: when hv_xhci_poll next has something to do on time (an isochronous TD's frame ends, MFINDEX
   wraps), as the guest's counter (hv_counter); 0 if nothing. */
uint64_t hv_xhci_due(HvXhci *x);
/* The speaker on port 7 (always there): the host gives it its name and its sound and volume backend before
   the Mac starts. */
struct HvUsbAudio *hv_xhci_speaker(HvXhci *x);
/* Snapshots: the speaker's port and state, beside the rest (a section of its own). */
typedef struct HvXhciSpeakerSnap {
    int32_t slot;
    uint32_t portsc;
    int32_t configured, alt, vol, mute;
} HvXhciSpeakerSnap;
void hv_xhci_speaker_get_snap(HvXhci *x, HvXhciSpeakerSnap *s);
/* After the touchscreen's (which sets the ports the driver knows). NULL: the snapshot has none (older): the
   speaker isn't there for this run. 0, or -1 if it makes no sense with the rest. */
int hv_xhci_speaker_set_snap(HvXhci *x, const HvXhciSpeakerSnap *s);

/* Any thread. Plugs the disk image at path into a free port as a USB drive (read-only if readonly, or
   if the file can't be written); macOS sees it arrive. name is the device's name, serial its USB serial
   number. Returns the port (2..HV_XHCI_PORTS), -1 if the file can't be opened, -2 if every port is taken. */
int hv_xhci_plug(HvXhci *x, const char *path, int readonly, const char *name, const char *serial);
/* Pulls it out (eject it on the Mac first, or macOS warns it wasn't). Its writes reach the file. */
void hv_xhci_unplug(HvXhci *x, int port);
int hv_xhci_drive_state(HvXhci *x, int port); /* HV_XHCI_DRIVE_* */
int hv_xhci_drive_readonly(HvXhci *x, int port);
/* What the drives were written onto the host (sync 0: its cache; 1: its storage). 0, or -1. */
int hv_xhci_flush_drives(HvXhci *x, int sync);
/* Any thread: 1 while a drive's thread is reading or writing for the guest. */
int hv_xhci_drives_busy(HvXhci *x);
/* Suspend, CPUs paused: every drive finishes its TD and takes what else it can, then writes what it holds to
   its storage. Nothing is busy after; what stays queued waits on the guest. 0; -1 if a flush failed; -2 if a
   drive's thread was still working at deadline_ns (CLOCK_MONOTONIC; it carries on, and so can the Mac). */
int hv_xhci_drives_settle(HvXhci *x, uint64_t deadline_ns);

/* ---- the touchscreen ---- */

/* One finger: id 0..9 (stays the same while it touches), x and y 0..32767 over the screen, down 1
   while it touches. A finger lifted goes in one frame with down 0, and then not again. cancel 1 with
   down 0: it didn't end in a touch (HID's Confidence 0) — what macOS makes of that is its own. */
typedef struct HvXhciContact {
    uint8_t id, down, cancel, pad;
    uint16_t x, y;
} HvXhciContact;

enum {
    HV_XHCI_TOUCH_ABSENT = 0,     /* not plugged in */
    HV_XHCI_TOUCH_PLUGGED = 1,    /* there, macOS hasn't configured it (yet) */
    HV_XHCI_TOUCH_READY = 2,      /* macOS configured it and reads its reports */
    HV_XHCI_TOUCH_UNAVAILABLE = 3 /* this Mac's driver doesn't know port 6 (resumed from an older snapshot) */
};

/* Any thread: plug the touchscreen in (on) or pull it out. w_mm, h_mm: the screen's size in
   millimetres. Pulling it out drops the reports still waiting. */
void hv_xhci_touch_plug(HvXhci *x, int on, int w_mm, int h_mm);
/* Any thread: every contact that is down, and those lifted since the last frame (down 0), in one
   report. n up to HV_XHCI_CONTACTS. 0, or -1 if macOS hasn't configured it or the queue is full (the
   frame is dropped, and every finger is lifted for macOS as soon as it can be, so none stays down). */
int hv_xhci_touch_frame(HvXhci *x, int n, const HvXhciContact *c);
int hv_xhci_touch_state(HvXhci *x); /* HV_XHCI_TOUCH_* */

/* Snapshots: the touchscreen's part of the controller, beside the rest. */
typedef struct HvXhciTouchSnap {
    int32_t nports, present, w_mm, h_mm, slot, configured;
    uint32_t portsc;
    /* The fingers macOS has down, and where each was last (a section of their own, newer than the rest).
       fingers_known 0: not saved, and every finger is taken to be down, somewhere not known. */
    int32_t fingers_known;
    uint32_t mac_down;
    uint16_t mac_x[HV_XHCI_CONTACTS], mac_y[HV_XHCI_CONTACTS];
} HvXhciTouchSnap;
void hv_xhci_touch_get_snap(HvXhci *x, HvXhciTouchSnap *s);
/* After the controller's own state is loaded. NULL: the snapshot has none (older): the Mac's driver
   knows HV_XHCI_PORTS ports. A snapshot's nports is from HV_XHCI_PORTS to HV_XHCI_ROOT_PORTS. 0, or -1 if it makes no sense with the rest. */
int hv_xhci_touch_set_snap(HvXhci *x, const HvXhciTouchSnap *s);

/* ---- the camera ---- */

/* Before the machine runs: the camera device (usb_video.c), plugged in on port 8 once the guest's driver can
   see that port. The controller doesn't own it. */
void hv_xhci_camera_attach(HvXhci *x, struct HvUsbVideo *v);
/* Any thread: plug it in or pull it out (macOS sees the camera arrive or go). */
void hv_xhci_camera_plug(HvXhci *x, int on);
/* HV_XHCI_TOUCH_*'s values: absent, plugged in, macOS configured it, or unavailable (this Mac's driver was told
   of fewer ports: resumed from an older snapshot). */
int hv_xhci_camera_state(HvXhci *x);

typedef struct HvXhciCamSnap {
    int32_t present, slot;
    uint32_t portsc;
    HvUsbVideoState dev;
} HvXhciCamSnap;
void hv_xhci_camera_get_snap(HvXhci *x, HvXhciCamSnap *s);
/* After hv_xhci_touch_set_snap (which says how many ports the driver knows). NULL: the snapshot has no camera
   section (older). 0, or -1 if it makes no sense with the rest. */
int hv_xhci_camera_set_snap(HvXhci *x, const HvXhciCamSnap *s);

#endif
