#include "xhci.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "machine.h"
#include "sysreg.h"
#include "usb_audio.h"
#include "usb_storage.h"
#include "usb_video.h"

/* Register layout in BAR 0. */
#define CAPLENGTH 0x40
#define OP 0x40          /* operational registers */
#define PORTREGS 0x440   /* OP + 0x400 */
#define RT 0x1000        /* runtime registers */
#define DB 0x2000        /* doorbells */
#define XECP 0x800       /* extended capabilities */
#define BAR_SIZE 0x4000

/* TRB types. */
enum {
    TRB_NORMAL = 1, TRB_SETUP = 2, TRB_DATA = 3, TRB_STATUS = 4, TRB_ISOCH = 5, TRB_LINK = 6, TRB_EVENT_DATA = 7, TRB_NOOP = 8,
    TRB_ENABLE_SLOT = 9, TRB_DISABLE_SLOT = 10, TRB_ADDRESS_DEVICE = 11, TRB_CONFIGURE_EP = 12, TRB_EVALUATE_CTX = 13,
    TRB_RESET_EP = 14, TRB_STOP_EP = 15, TRB_SET_TR_DEQUEUE = 16, TRB_RESET_DEVICE = 17, TRB_NOOP_CMD = 23,
    TRB_TRANSFER_EVENT = 32, TRB_CMD_COMPLETION = 33, TRB_PORT_STATUS = 34, TRB_MFINDEX_WRAP = 39,
};
/* Completion codes. */
enum { CC_SUCCESS = 1, CC_TRB_ERROR = 5, CC_STALL = 6, CC_SLOTS_FULL = 9, CC_SLOT_NOT_ENABLED = 11, CC_SHORT_PACKET = 13, CC_CONTEXT_STATE = 19 };

/* PORTSC bits. */
#define PORT_CCS (1u << 0)
#define PORT_PED (1u << 1)
#define PORT_PR (1u << 4)
#define PORT_PLS_SHIFT 5
#define PORT_PP (1u << 9)
#define PORT_SPEED_SHIFT 10
#define PORT_LWS (1u << 16)
#define PORT_CSC (1u << 17)
#define PORT_PEC (1u << 18)
#define PORT_WRC (1u << 19)
#define PORT_OCC (1u << 20)
#define PORT_PRC (1u << 21)
#define PORT_PLC (1u << 22)
#define PORT_CEC (1u << 23)
#define PORT_CHANGES (PORT_CSC | PORT_PEC | PORT_WRC | PORT_OCC | PORT_PRC | PORT_PLC | PORT_CEC)
#define SPEED_FULL 1
#define SPEED_HIGH 3
#define PLS_RXDETECT 5
#define PLS_POLLING 7

#define KBD_DCI_IN 3 /* endpoint 1 IN */
#define TOUCH_DCI_IN 3 /* endpoint 1 IN */
#define TOUCH_P (HV_XHCI_TOUCH_PORT - 1) /* its port index */
#define DRIVE_DCI_IN 3  /* endpoint 1 IN */
#define DRIVE_DCI_OUT 4 /* endpoint 2 OUT */
#define SPK_P (HV_XHCI_SPEAKER_PORT - 1)
#define CAM_DCI_IN 3    /* endpoint 1 IN: video, bulk */
#define CAM_P (HV_XHCI_CAMERA_PORT - 1)

/* Time: the guest's counter runs at 24 MHz; a microframe is 125 us, a frame 1 ms. */
#define TICKS_PER_UFRAME 3000ull
#define TICKS_PER_FRAME (8 * TICKS_PER_UFRAME)
#define MFINDEX_PERIOD (16384 * TICKS_PER_UFRAME) /* MFINDEX is 14 bits of microframes */

/* ---- the keyboard's descriptors (USB HID 1.11 boot keyboard) ---- */

static const uint8_t report_desc[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
    0x75, 0x08, 0x15, 0x00, 0x26, 0xff, 0x00, 0x05, 0x07, 0x19, 0x00, 0x29, 0xff, 0x81, 0x00, 0xc0,
};

static const uint8_t device_desc[18] = {
    18, 1, 0x00, 0x02, 0, 0, 0, 64, /* USB 2.0, class per interface, EP0 64 bytes */
    0xac, 0x05, 0x20, 0x02,         /* vendor Apple, product 0x0220 (an ANSI keyboard): macOS knows its layout, no Keyboard Setup Assistant */
    0x00, 0x01, 1, 2, 3, 1,         /* bcdDevice 1.00, strings 1-3, one configuration */
};

static const uint8_t config_desc[34] = {
    9, 2, 34, 0, 1, 1, 0, 0xa0, 50,           /* one interface, remote wakeup, 100 mA */
    9, 4, 0, 0, 1, 3, 1, 1, 0,                /* interface 0: HID, boot, keyboard */
    9, 0x21, 0x11, 0x01, 0, 1, 0x22, sizeof report_desc, 0, /* HID 1.11, one report descriptor */
    7, 5, 0x81, 3, 8, 0, 10,                  /* EP1 IN, interrupt, 8 bytes, every 10 ms */
};

static const char *const strings[] = {NULL, "HengeVM", "HengeVM USB Keyboard", "1"};

/* ---- the touchscreen's descriptors (a HID multi-touch touchscreen, the kind Windows and macOS 27 take) ---- */

static const uint8_t touch_device_desc[18] = {
    18, 1, 0x00, 0x02, 0, 0, 0, 64, /* USB 2.0, class per interface, EP0 64 bytes */
    0x09, 0x12, 0x01, 0x00,         /* pid.codes' vendor ID, its test product: no driver matches it by ID */
    0x00, 0x01, 1, 2, 3, 1,         /* bcdDevice 1.00, strings 1-3, one configuration */
};

static const char *const touch_strings[] = {NULL, "Henge", "Henge Touchscreen", "1"};

/* Its report descriptor: report 1 holds ten Finger collections (tip, in range, confidence, contact ID, X, Y) and the
   contact count, in Windows' "parallel" layout; feature report 2 is Contact Count Maximum. X and Y run
   0..32767 over the screen, whose size (in 0.1 mm) is the physical extent. */
static int touch_build_desc(uint8_t *d, int w_mm, int h_mm) {
    int n = 0;
#define B(...)                                                   \
    do {                                                         \
        static const uint8_t b_[] = {__VA_ARGS__};               \
        memcpy(d + n, b_, sizeof b_);                            \
        n += (int)sizeof b_;                                     \
    } while (0)
    int w = w_mm * 10, h = h_mm * 10; /* 0.1 mm; a signed 16-bit item */
    if (w < 1 || w > 32767) w = 2500;
    if (h < 1 || h > 32767) h = 1800;
    B(0x05, 0x0d, 0x09, 0x04, 0xa1, 0x01, 0x85, 0x01);             /* Digitizer: Touch Screen, application; report 1 */
    for (int i = 0; i < HV_XHCI_CONTACTS; i++) {
        B(0x09, 0x22, 0xa1, 0x02);                                 /* Finger, logical */
        B(0x09, 0x42, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02); /* Tip Switch */
        B(0x09, 0x32, 0x81, 0x02);                                 /* In Range */
        B(0x09, 0x47, 0x81, 0x02);                                 /* Confidence: 0 cancels the contact */
        B(0x75, 0x05, 0x81, 0x03);                                 /* 5 bits padding */
        B(0x09, 0x51, 0x25, 0x1f, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02); /* Contact Identifier */
        B(0x05, 0x01, 0x26, 0xff, 0x7f, 0x75, 0x10, 0x55, 0x0e, 0x65, 0x11, 0x35, 0x00); /* 16 bits, 0..32767, cm x 10^-2 */
        B(0x09, 0x30, 0x46);                                       /* X, its physical maximum */
        d[n++] = (uint8_t)w, d[n++] = (uint8_t)(w >> 8);
        B(0x81, 0x02, 0x09, 0x31, 0x46);                           /* Y */
        d[n++] = (uint8_t)h, d[n++] = (uint8_t)(h >> 8);
        B(0x81, 0x02, 0x45, 0x00, 0x65, 0x00, 0x55, 0x00, 0x05, 0x0d, 0xc0);
    }
    B(0x09, 0x54, 0x15, 0x00, 0x25, HV_XHCI_CONTACTS, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02); /* Contact Count */
    B(0x85, 0x02, 0x09, 0x55, 0x25, HV_XHCI_CONTACTS, 0xb1, 0x02); /* feature 2: Contact Count Maximum */
    B(0xc0);
#undef B
    return n;
}

/* The contact ID of a Finger collection with no finger in it: one no finger has (they are 0..9), within the
   descriptor's 0..31. macOS's HID driver (IOHIDEventDriver's createDigitizerTransducerEventForReport) ignores
   Contact Count: it reads every collection whose values changed, and keeps each contact's state by its ID. An
   empty collection as zeros is contact 0 not touching, so when one emptied (a second finger had lifted) macOS
   lifted whichever finger was contact 0 at the screen's corner, as it moved; it went down again on its next move.
   During a window drag by the title bar that left macOS's window-drag gesture waiting for its finger forever,
   and it cancelled every touch after (8 Nov 2026). With this ID an empty collection is a contact never down. */
#define TOUCH_NO_CONTACT 0x1f

/* Report 1 with no finger in it. */
static void touch_report_empty(uint8_t r[HV_XHCI_TOUCH_REPORT]) {
    memset(r, 0, HV_XHCI_TOUCH_REPORT);
    r[0] = 1;
    for (int i = 0; i < HV_XHCI_CONTACTS; i++) r[2 + 6 * i] = TOUCH_NO_CONTACT;
}

/* Its configuration: one HID interface, an interrupt IN endpoint polled every millisecond. */
static int touch_config_desc(const HvXhci *x, uint8_t c[34]) {
    static const uint8_t head[34] = {
        9, 2, 34, 0, 1, 1, 0, 0xa0, 50,
        9, 4, 0, 0, 1, 3, 0, 0, 0,                  /* interface 0: HID, no boot protocol */
        9, 0x21, 0x11, 0x01, 0, 1, 0x22, 0, 0,      /* HID 1.11, one report descriptor (length below) */
        7, 5, 0x81, 3, 64, 0, 1,                    /* EP1 IN, interrupt, 64 bytes, every 1 ms */
    };
    memcpy(c, head, 34);
    c[25] = (uint8_t)x->touch.desc_len, c[26] = (uint8_t)(x->touch.desc_len >> 8);
    return 34;
}

static int xlog(void) {
    static int on = -1;
    if (on < 0) on = getenv("HV_XHCI_LOG") != NULL;
    return on;
}

/* ---- guest memory ---- */

static void rd(HvXhci *x, uint64_t pa, void *buf, unsigned len) {
    if (hv_phys_read(x->m, pa, buf, len) != 0) memset(buf, 0, len);
}

static void wr(HvXhci *x, uint64_t pa, const void *buf, unsigned len) {
    hv_phys_write(x->m, pa, buf, len);
}

static uint32_t rd32(HvXhci *x, uint64_t pa) {
    uint32_t v;
    rd(x, pa, &v, 4);
    return v;
}

/* ---- interrupts and the event ring ---- */

static void update_irq(HvXhci *x) {
    int level = 0;
    for (int i = 0; i < HV_XHCI_INTRS; i++) level |= (x->intr[i].iman & 1) && (x->intr[i].iman & 2); /* IP, IE */
    hv_pci_set_intx(&x->pci, level && (x->usbcmd & 4)); /* INTE */
}

static void load_segment(HvXhci *x, struct HvXhciIntr *it) {
    uint64_t entry[2];
    rd(x, it->erstba + 16ull * it->seg_index, entry, 16);
    it->seg_base = entry[0] & ~0x3full;
    it->seg_size = (uint32_t)entry[1] & 0xffff;
    it->index = 0;
}

/* bei: the event goes on the ring without interrupting (Block Event Interrupt): a later one does. */
static void event_to_bei(HvXhci *x, int target, uint64_t param, uint32_t status, uint32_t control, int bei) {
    struct HvXhciIntr *it = &x->intr[target < HV_XHCI_INTRS ? target : 0];
    if (!it->erstsz || !it->seg_size) return;
    uint32_t trb[4] = {(uint32_t)param, (uint32_t)(param >> 32), status, (control & ~1u) | (uint32_t)it->pcs};
    wr(x, it->seg_base + 16ull * it->index, trb, 16);
    if (++it->index == it->seg_size) { /* next segment, or wrap and flip the cycle */
        if (++it->seg_index >= it->erstsz) {
            it->seg_index = 0;
            it->pcs ^= 1;
        }
        load_segment(x, it);
    }
    if (bei) return;
    it->iman |= 1;        /* IP */
    x->usbsts |= 1u << 3; /* EINT */
    update_irq(x);
}

static void event_to(HvXhci *x, int target, uint64_t param, uint32_t status, uint32_t control) {
    event_to_bei(x, target, param, status, control, 0);
}

static void event(HvXhci *x, uint64_t param, uint32_t status, uint32_t control) {
    event_to(x, 0, param, status, control);
}

static void port_event(HvXhci *x, int port) {
    if (!(x->usbcmd & 1)) return; /* halted: the driver reads the ports when it starts */
    x->usbsts |= 1u << 4;         /* PCD */
    event(x, (uint64_t)(port + 1) << 24, (uint32_t)CC_SUCCESS << 24, TRB_PORT_STATUS << 10);
}

static void cmd_completion(HvXhci *x, uint64_t trb_pa, int cc, int slot) {
    event(x, trb_pa, (uint32_t)cc << 24, TRB_CMD_COMPLETION << 10 | (uint32_t)slot << 24);
}

static void transfer_event(HvXhci *x, uint64_t trb_pa, int cc, uint32_t residual, int slot, int dci) {
    uint32_t dw2 = rd32(x, trb_pa + 8); /* each transfer TRB names its interrupter (bits 31:22) */
    event_to(x, (int)(dw2 >> 22), trb_pa, (uint32_t)cc << 24 | (residual & 0xffffff), TRB_TRANSFER_EVENT << 10 | (uint32_t)dci << 16 | (uint32_t)slot << 24);
}

/* ---- rings ---- */

/* Next TRB the guest owns on a ring, following links; 0 if the ring is empty. */
static int ring_peek(HvXhci *x, HvXhciRing *r, uint32_t trb[4], uint64_t *at) {
    for (int guard = 0; guard < 64; guard++) {
        /* The guest writes the TRB, then its cycle bit, from another core:
           read the control word first (acquire), then the rest. */
        const uint32_t *ctl = hv_phys_ptr_r(x->m, r->dequeue + 12, 4); /* only read: no page lent at every poll */
        uint32_t c = ctl ? __atomic_load_n(ctl, __ATOMIC_ACQUIRE) : 0;
        if ((c & 1) != (uint32_t)r->ccs) return 0;
        rd(x, r->dequeue, trb, 12);
        trb[3] = c;
        if (((trb[3] >> 10) & 0x3f) == TRB_LINK) {
            if (trb[3] & 2) r->ccs ^= 1; /* toggle cycle */
            r->dequeue = ((uint64_t)trb[1] << 32 | trb[0]) & ~0xfull;
            continue;
        }
        *at = r->dequeue;
        return 1;
    }
    return 0;
}

static void ring_advance(HvXhciRing *r) {
    r->dequeue += 16;
}

/* ---- device contexts (32 bytes each; the output context is at DCBAA[slot]) ---- */

static uint64_t out_ctx(HvXhci *x, int slot) {
    uint64_t p;
    rd(x, x->dcbaap + 8ull * slot, &p, 8);
    return p & ~0x3full;
}

static void set_ep_state(HvXhci *x, int slot, int dci, uint32_t state) {
    uint64_t ctx = out_ctx(x, slot) + 32ull * dci;
    uint32_t dw0 = rd32(x, ctx);
    dw0 = (dw0 & ~7u) | state;
    wr(x, ctx, &dw0, 4);
}

static int drive_port(int port) { /* 1-based: a port that takes drives */
    return port >= 2 && port <= HV_XHCI_PORTS;
}

static struct HvUsbStorage *slot_drive(HvXhci *x, int slot) {
    if (slot < 1 || slot > HV_XHCI_SLOTS) return NULL;
    int port = x->slots[slot].port;
    return drive_port(port) ? x->port[port - 1].drive : NULL;
}

/* ---- the keyboard's and touchscreen's control requests ---- */

/* Answer a SETUP on endpoint 0. Returns the reply length (data to send for IN
   requests), or -1 to stall. */
static void touch_forget(HvXhci *x);

/* CPU thread: the touchscreen's slot and configuration, which hv_xhci_touch_frame reads on any thread. */
static void touch_set(HvXhci *x, int slot, int configured) {
    __atomic_store_n(&x->touch.slot, slot, __ATOMIC_RELAXED);
    __atomic_store_n(&x->touch.configured, configured, __ATOMIC_RELAXED);
}

/* The keyboard's, or (touch) the touchscreen's. */
static int control(HvXhci *x, int touch, const uint8_t setup[8], uint8_t *out, int max) {
    uint8_t type = setup[0], req = setup[1];
    uint16_t value = (uint16_t)(setup[2] | setup[3] << 8);
    if (xlog()) fprintf(stderr, "xhci: %s request %02x %02x value %04x len %u\n", touch ? "touch" : "kbd", type, req, value, setup[6] | setup[7] << 8);
    const uint8_t *src = NULL;
    int len = 0;
    uint8_t tmp[HV_XHCI_TOUCH_REPORT > 64 ? HV_XHCI_TOUCH_REPORT : 64];
    int *configured = touch ? &x->touch.configured : &x->configured;
    if (type == 0x80 && req == 6) { /* GET_DESCRIPTOR (device) */
        uint8_t dt = value >> 8, idx = value & 0xff;
        const char *const *str = touch ? touch_strings : strings;
        if (dt == 1) src = touch ? touch_device_desc : device_desc, len = 18;
        else if (dt == 2) {
            if (touch) src = tmp, len = touch_config_desc(x, tmp);
            else src = config_desc, len = sizeof config_desc;
        } else if (dt == 3) {
            if (idx == 0) {
                tmp[0] = 4, tmp[1] = 3, tmp[2] = 0x09, tmp[3] = 0x04; /* English (US) */
                src = tmp, len = 4;
            } else if (idx < 4) {
                const char *s = str[idx];
                int n = (int)strlen(s);
                tmp[0] = (uint8_t)(2 + 2 * n), tmp[1] = 3;
                for (int i = 0; i < n; i++) tmp[2 + 2 * i] = (uint8_t)s[i], tmp[3 + 2 * i] = 0;
                src = tmp, len = 2 + 2 * n;
            } else
                return -1;
        } else
            return -1; /* no device qualifier: full speed only */
    } else if (type == 0x81 && req == 6) { /* GET_DESCRIPTOR (interface): HID, report */
        if ((value >> 8) == 0x22) {
            if (touch) src = x->touch.desc, len = x->touch.desc_len;
            else src = report_desc, len = sizeof report_desc;
        } else if ((value >> 8) == 0x21) {
            if (touch) touch_config_desc(x, tmp), memmove(tmp, tmp + 18, 9), src = tmp;
            else src = config_desc + 18;
            len = 9;
        } else
            return -1;
    } else if (type == 0x80 && req == 0) { /* GET_STATUS */
        tmp[0] = 0, tmp[1] = 0;
        src = tmp, len = 2;
    } else if ((type == 0x81 || type == 0x82) && req == 0) {
        tmp[0] = 0, tmp[1] = 0;
        src = tmp, len = 2;
    } else if (type == 0x80 && req == 8) { /* GET_CONFIGURATION */
        tmp[0] = (uint8_t)*configured;
        src = tmp, len = 1;
    } else if (type == 0x00 && req == 9) { /* SET_CONFIGURATION */
        if (touch) touch_forget(x); /* a new start for macOS's HID driver (before it can be ready again) */
        __atomic_store_n(configured, value & 0xff, __ATOMIC_RELAXED); /* the touchscreen's is read on any thread */
        return 0;
    } else if (type == 0x81 && req == 10) { /* GET_INTERFACE */
        tmp[0] = 0;
        src = tmp, len = 1;
    } else if (type == 0xa1 && req == 1 && touch) { /* GET_REPORT: the input report as it stands, or a feature */
        if (value == 0x0101) {
            pthread_mutex_lock(&x->mu);
            memcpy(tmp, x->touch.cur, HV_XHCI_TOUCH_REPORT);
            pthread_mutex_unlock(&x->mu);
            tmp[0] = 1;
            src = tmp, len = HV_XHCI_TOUCH_REPORT;
        } else if (value == 0x0302) {
            tmp[0] = 2, tmp[1] = HV_XHCI_CONTACTS; /* Contact Count Maximum */
            src = tmp, len = 2;
        } else
            return -1;
    } else if (type == 0xa1 && req == 1) { /* GET_REPORT */
        src = x->state, len = 8;
    } else if (type == 0xa1 && (req == 2 || req == 3)) { /* GET_IDLE, GET_PROTOCOL */
        tmp[0] = req == 3 && !touch ? 1 : 0; /* report protocol for the touchscreen: it has no boot one */
        src = tmp, len = 1;
    } else if ((type & 0x60) == 0x20 || (type == 0x00 && (req == 1 || req == 3)) || (type == 0x01 && req == 11) ||
               (type == 0x02 && (req == 1 || req == 3))) {
        return 0; /* SET_IDLE, SET_PROTOCOL, SET_REPORT (LEDs), features, SET_INTERFACE: accepted */
    } else {
        if (x->m->log) fprintf(stderr, "hengevm: xhci: unhandled request %02x %02x value %04x\n", type, req, value);
        return -1;
    }
    if (len > max) len = max;
    memcpy(out, src, (size_t)len);
    return len;
}

/* Run the control transfers queued on a device's endpoint 0. */
static void run_ep0(HvXhci *x, int slot) {
    HvXhciSlot *sl = &x->slots[slot];
    HvXhciRing *r = &sl->ep[1];
    struct HvUsbStorage *drive = slot_drive(x, slot);
    struct HvUsbVideo *cam = x->cam.present && slot == x->cam.slot ? x->cam.dev : NULL;
    uint32_t trb[4];
    uint64_t at;
    uint8_t setup[8], reply[1024]; /* the touchscreen's report descriptor is about 600 bytes */
    int reply_len = 0, stalled = 0;
    int deferred = 0; /* the camera's request that sends data: answered once the data stage brings it */
    while (ring_peek(x, r, trb, &at)) {
        int type = (trb[3] >> 10) & 0x3f;
        int ioc = (trb[3] >> 5) & 1;
        if (type == TRB_SETUP) {
            memcpy(setup, trb, 8);
            uint16_t wlen = (uint16_t)(setup[6] | setup[7] << 8);
            int max = wlen < sizeof reply ? wlen : (int)sizeof reply;
            if (cam) {
                deferred = !(setup[0] & 0x80) && wlen;
                reply_len = deferred ? 0 : hv_usb_video_control(cam, setup, NULL, 0, reply, max);
            } else if (slot == x->spk.slot) {
                /* The speaker's OUT requests (SET_CUR) carry data: they go to it with their data stage. */
                x->spk.out_pending = !(setup[0] & 0x80) && wlen;
                if (x->spk.out_pending) memcpy(x->spk.out_setup, setup, 8), reply_len = 0;
                else reply_len = hv_usb_audio_control(x->spk.dev, setup, NULL, 0, reply, max);
            } else
                reply_len = drive ? hv_usb_storage_control(drive, setup, reply, max) : control(x, sl->port == HV_XHCI_TOUCH_PORT, setup, reply, max);
            stalled = reply_len < 0;
            ring_advance(r);
            if (stalled) {
                transfer_event(x, at, CC_STALL, 0, slot, 1);
                set_ep_state(x, slot, 1, 2); /* halted */
                sl->ep_running[1] = 0;
                return;
            }
            if (ioc) transfer_event(x, at, CC_SUCCESS, 0, slot, 1);
        } else if (type == TRB_DATA) {
            uint64_t buf = (uint64_t)trb[1] << 32 | trb[0];
            uint32_t want = trb[2] & 0x1ffff;
            int in = (trb[3] >> 16) & 1;
            uint32_t n = 0;
            if (in) {
                n = reply_len < 0 ? 0 : (uint32_t)reply_len < want ? (uint32_t)reply_len : want;
                if (n) wr(x, buf, reply, n);
            } else {
                n = want; /* SET_REPORT and friends: take the data and ignore it */
                if (slot == x->spk.slot && x->spk.out_pending) {
                    uint32_t k = want < sizeof reply ? want : (uint32_t)sizeof reply;
                    if (trb[3] & (1u << 6)) memcpy(reply, trb, k = k > 8 ? 8 : k); /* immediate data */
                    else rd(x, buf, reply, k);
                    x->spk.out_pending = 0;
                    if (hv_usb_audio_control(x->spk.dev, x->spk.out_setup, reply, (int)k, reply, 0) < 0) {
                        ring_advance(r);
                        transfer_event(x, at, CC_STALL, want, slot, 1);
                        set_ep_state(x, slot, 1, 2); /* halted */
                        sl->ep_running[1] = 0;
                        return;
                    }
                } else if (deferred) { /* the camera's (probe, commit): it gets the data */
                    uint8_t data[64], none[1];
                    int k = want < sizeof data ? (int)want : (int)sizeof data;
                    if (trb[3] & (1u << 6)) memcpy(data, trb, (size_t)(k < 8 ? k : 8)); /* immediate data */
                    else rd(x, buf, data, (unsigned)k);
                    deferred = 0;
                    if (hv_usb_video_control(cam, setup, data, k, none, 0) < 0) {
                        ring_advance(r);
                        transfer_event(x, at, CC_STALL, 0, slot, 1);
                        set_ep_state(x, slot, 1, 2); /* halted */
                        sl->ep_running[1] = 0;
                        return;
                    }
                }
            }
            ring_advance(r);
            int isp = (trb[3] >> 2) & 1;
            if (ioc || (isp && n < want)) transfer_event(x, at, n < want ? CC_SHORT_PACKET : CC_SUCCESS, want - n, slot, 1);
        } else if (type == TRB_STATUS) {
            ring_advance(r);
            if (ioc) transfer_event(x, at, CC_SUCCESS, 0, slot, 1);
        } else {
            ring_advance(r);
            if (ioc) transfer_event(x, at, CC_SUCCESS, 0, slot, 1);
        }
    }
}

/* Give waiting key reports to the interrupt IN endpoint's posted transfers. */
static void run_ep_in(HvXhci *x) {
    int slot = x->kbd_slot;
    if (!slot || !x->slots[slot].ep_running[KBD_DCI_IN]) return;
    HvXhciRing *r = &x->slots[slot].ep[KBD_DCI_IN];
    uint32_t trb[4];
    uint64_t at;
    pthread_mutex_lock(&x->mu);
    while (x->rhead != x->rtail && ring_peek(x, r, trb, &at)) {
        int type = (trb[3] >> 10) & 0x3f;
        ring_advance(r);
        if (type != TRB_NORMAL) continue;
        uint64_t buf = (uint64_t)trb[1] << 32 | trb[0];
        uint32_t want = trb[2] & 0x1ffff;
        uint32_t n = want < 8 ? want : 8;
        wr(x, buf, x->reports[x->rhead % HV_XHCI_REPORTS], n);
        x->rhead++;
        x->delivered++;
        transfer_event(x, at, n < want ? CC_SHORT_PACKET : CC_SUCCESS, want - n, slot, KBD_DCI_IN);
    }
    pthread_mutex_unlock(&x->mu);
}

/* Give the speaker's messages to its interrupt IN endpoints: the audio status (a control changed) and the
   volume buttons. */
static void run_speaker_in(HvXhci *x) {
    int slot = x->spk.slot;
    if (!slot || !x->spk.dev) return;
    for (int k = 0; k < 2; k++) {
        int dci = k ? HV_USB_AUDIO_DCI_HID : HV_USB_AUDIO_DCI_STATUS;
        if (!x->slots[slot].ep_running[dci]) continue;
        HvXhciRing *r = &x->slots[slot].ep[dci];
        uint32_t trb[4];
        uint64_t at;
        while (ring_peek(x, r, trb, &at)) {
            if (((trb[3] >> 10) & 0x3f) != TRB_NORMAL) {
                ring_advance(r);
                continue;
            }
            uint8_t msg[6];
            int n = k ? hv_usb_audio_next_hid(x->spk.dev, msg) : hv_usb_audio_next_status(x->spk.dev, msg);
            if (!n) break;
            ring_advance(r);
            uint64_t buf = (uint64_t)trb[1] << 32 | trb[0];
            uint32_t want = trb[2] & 0x1ffff, m = want < (uint32_t)n ? want : (uint32_t)n;
            wr(x, buf, msg, m);
            transfer_event(x, at, m < want ? CC_SHORT_PACKET : CC_SUCCESS, want - m, slot, dci);
        }
    }
}

/* Under mu: at the queue's end, a lift of every finger macOS has down. It is a marker (report ID 0) until it is
   delivered, when the report is made from what macOS has then (the reports before it may put fingers down). */
static void touch_queue_lift_all(struct HvXhciTouch *t) {
    memset(t->q[t->qtail++ % HV_XHCI_REPORTS], 0, HV_XHCI_TOUCH_REPORT);
}

/* Under mu: the lift of every finger macOS has down, each where it last was (confident: they end as touches do,
   so macOS lets go of them for certain; where that isn't known, as cancelled). How many: 0, none to lift. */
static int touch_lift_report(const struct HvXhciTouch *t, uint8_t r[HV_XHCI_TOUCH_REPORT]) {
    touch_report_empty(r);
    int n = 0;
    for (int id = 0; id < HV_XHCI_CONTACTS; id++) {
        if (!(t->mac_down >> id & 1)) continue;
        uint8_t *f = r + 1 + 6 * n++;
        f[0] = t->mac_unknown ? 0 : 4, f[1] = (uint8_t)id;
        f[2] = (uint8_t)t->mac_x[id], f[3] = (uint8_t)(t->mac_x[id] >> 8);
        f[4] = (uint8_t)t->mac_y[id], f[5] = (uint8_t)(t->mac_y[id] >> 8);
    }
    r[HV_XHCI_TOUCH_REPORT - 1] = (uint8_t)n;
    return n;
}

/* Under mu: macOS was given report r. Each frame has every finger that is down (a lifted one once, and then not
   again), so one not in it is up. */
static void touch_given(struct HvXhciTouch *t, const uint8_t r[HV_XHCI_TOUCH_REPORT]) {
    uint16_t down = 0;
    int n = r[HV_XHCI_TOUCH_REPORT - 1];
    for (int i = 0; i < n && i < HV_XHCI_CONTACTS; i++) {
        const uint8_t *f = r + 1 + 6 * i;
        if (f[1] >= HV_XHCI_CONTACTS) continue;
        if (f[0] & 1) down |= (uint16_t)(1u << f[1]);
        t->mac_x[f[1]] = (uint16_t)(f[2] | f[3] << 8);
        t->mac_y[f[1]] = (uint16_t)(f[4] | f[5] << 8);
    }
    t->mac_down = down;
    t->mac_unknown = 0;
}

/* The touchscreen starts again for macOS (enumerated or configured again, reset, or gone): reports waiting
   were for the device macOS had before, and that one's fingers went with it. Not under mu. */
static void touch_forget(HvXhci *x) {
    pthread_mutex_lock(&x->mu);
    x->touch.qhead = x->touch.qtail;
    __atomic_store_n(&x->touch.lost, 0, __ATOMIC_RELAXED);
    x->touch.mac_down = 0, x->touch.mac_unknown = 0;
    touch_report_empty(x->touch.cur);
    pthread_mutex_unlock(&x->mu);
}

static int touch_takes(HvXhci *x);

/* Give waiting touch reports to the touchscreen's interrupt IN endpoint. */
static void run_touch_in(HvXhci *x) {
    int slot = x->touch.slot;
    if (!slot || !x->slots[slot].ep_running[TOUCH_DCI_IN]) return;
    HvXhciRing *r = &x->slots[slot].ep[TOUCH_DCI_IN];
    uint32_t trb[4];
    uint64_t at;
    pthread_mutex_lock(&x->mu);
    /* Frames were dropped (the queue was full, or it wasn't ready) and no frame has come since: lift every finger. */
    if (x->touch.lost && touch_takes(x) && x->touch.qtail - x->touch.qhead < HV_XHCI_REPORTS) {
        touch_queue_lift_all(&x->touch);
        __atomic_store_n(&x->touch.lost, 0, __ATOMIC_RELAXED);
    }
    while (x->touch.qhead != x->touch.qtail) {
        const uint8_t *report = x->touch.q[x->touch.qhead % HV_XHCI_REPORTS];
        uint8_t lift[HV_XHCI_TOUCH_REPORT];
        if (report[0] == 0) { /* a lift of every finger: those macOS has down now, or nothing if none is */
            if (!touch_lift_report(&x->touch, lift)) {
                x->touch.qhead++;
                continue;
            }
            report = lift;
        }
        if (!ring_peek(x, r, trb, &at)) break;
        int type = (trb[3] >> 10) & 0x3f;
        ring_advance(r);
        if (type != TRB_NORMAL) continue;
        uint64_t buf = (uint64_t)trb[1] << 32 | trb[0];
        uint32_t want = trb[2] & 0x1ffff;
        uint32_t n = want < HV_XHCI_TOUCH_REPORT ? want : HV_XHCI_TOUCH_REPORT;
        wr(x, buf, report, n);
        touch_given(&x->touch, report);
        x->touch.qhead++;
        x->touch.delivered++;
        transfer_event(x, at, n < want ? CC_SHORT_PACKET : CC_SUCCESS, want - n, slot, TOUCH_DCI_IN);
    }
    pthread_mutex_unlock(&x->mu);
}

/* ---- commands ---- */

static void init_ring_from_ctx(HvXhci *x, int slot, int dci) {
    uint64_t ctx = out_ctx(x, slot) + 32ull * dci;
    uint64_t deq;
    rd(x, ctx + 8, &deq, 8);
    x->slots[slot].ep[dci].dequeue = deq & ~0xfull;
    x->slots[slot].ep[dci].ccs = (int)(deq & 1);
    x->slots[slot].ep_running[dci] = 1;
}

static int address_device(HvXhci *x, int slot, uint64_t input, int bsr) {
    uint64_t out = out_ctx(x, slot);
    uint8_t slot_ctx[32], ep0[32];
    rd(x, input + 32, slot_ctx, 32);
    rd(x, input + 64, ep0, 32);
    int port = slot_ctx[6]; /* root hub port number, dword 1 bits 23:16 */
    if (port < 1 || port > x->nports || (drive_port(port) && !x->port[port - 1].drive) ||
        (port == HV_XHCI_TOUCH_PORT && !x->touch.present) || (port == HV_XHCI_SPEAKER_PORT && !x->spk.present) ||
        (port == HV_XHCI_CAMERA_PORT && !x->cam.present))
        return CC_TRB_ERROR;
    uint32_t dw3 = (uint32_t)(bsr ? 1 : 2) << 27 | (uint32_t)(bsr ? 0 : slot); /* default or addressed; address = slot */
    memcpy(slot_ctx + 12, &dw3, 4);
    uint32_t e0 = 0;
    memcpy(&e0, ep0, 4);
    e0 = (e0 & ~7u) | 1; /* running */
    memcpy(ep0, &e0, 4);
    wr(x, out, slot_ctx, 32);
    wr(x, out + 32, ep0, 32);
    x->slots[slot].port = port;
    if (port == 1) x->kbd_slot = slot;
    else if (port == HV_XHCI_TOUCH_PORT) touch_set(x, slot, 0), touch_forget(x); /* enumerated again */
    else if (port == HV_XHCI_SPEAKER_PORT) x->spk.slot = slot, x->spk.out_pending = 0;
    else if (port == HV_XHCI_CAMERA_PORT) x->cam.slot = slot, hv_usb_video_reset(x->cam.dev);
    else x->port[port - 1].slot = slot;
    init_ring_from_ctx(x, slot, 1);
    return CC_SUCCESS;
}

static int configure_endpoints(HvXhci *x, int slot, uint64_t input, int deconfigure) {
    uint64_t out = out_ctx(x, slot);
    if (deconfigure) {
        for (int dci = 2; dci < HV_XHCI_EPS; dci++) x->slots[slot].ep_running[dci] = 0;
        return CC_SUCCESS;
    }
    uint32_t drop = rd32(x, input), add = rd32(x, input + 4);
    for (int dci = 2; dci < HV_XHCI_EPS; dci++) {
        if (drop & (1u << dci)) {
            x->slots[slot].ep_running[dci] = 0;
            x->slots[slot].ep_iso[dci] = 0;
            uint8_t zero[32] = {0};
            wr(x, out + 32ull * dci, zero, 32);
        }
        if (add & (1u << dci)) {
            uint8_t ep[32];
            rd(x, input + 32ull * (dci + 1), ep, 32);
            uint32_t e0;
            memcpy(&e0, ep, 4);
            e0 = (e0 & ~7u) | 1;
            memcpy(ep, &e0, 4);
            wr(x, out + 32ull * dci, ep, 32);
            init_ring_from_ctx(x, slot, dci);
            uint32_t type = (ep[4] >> 3) & 7; /* EP Type: 1 isochronous OUT, 5 IN */
            x->slots[slot].ep_iso[dci] = type == 1 || type == 5;
            x->slots[slot].iso_valid[dci] = 0;
        }
    }
    if (add & 1) { /* slot context: context entries and friends */
        uint8_t slot_ctx[32];
        rd(x, input + 32, slot_ctx, 32);
        uint32_t dw3 = (uint32_t)3 << 27 | (uint32_t)slot; /* configured */
        memcpy(slot_ctx + 12, &dw3, 4);
        wr(x, out, slot_ctx, 32);
    } else {
        uint32_t dw3 = (uint32_t)3 << 27 | (uint32_t)slot;
        wr(x, out + 12, &dw3, 4);
    }
    return CC_SUCCESS;
}


/* ---- drives: bulk transfers, a TD at a time ---- */

/* The next whole TD on a ring, without taking it: 1, 0 if the driver hasn't finished queueing it,
   -1 if it is longer than this model follows. */
static int td_gather(HvXhci *x, const HvXhciRing *r, HvXhciTd *td) {
    HvXhciRing c = *r;
    td->n = 0;
    td->len = 0;
    for (;;) {
        uint32_t t[4];
        uint64_t at;
        if (!ring_peek(x, &c, t, &at)) return 0;
        if (td->n == HV_XHCI_TD_MAX) return -1;
        memcpy(td->trb[td->n], t, 16);
        td->at[td->n++] = at;
        ring_advance(&c);
        int type = (t[3] >> 10) & 0x3f;
        if (type == TRB_NORMAL || type == TRB_ISOCH) td->len += t[2] & 0x1ffff;
        if (!(t[3] & (1u << 4))) break; /* no chain bit: the TD's last TRB */
    }
    td->after = c;
    return 1;
}

/* The TD's transfer events once it moved `moved` bytes, as QEMU's xHCI reports them (which macOS takes):
   one event, at the first TRB with IOC, or at the TRB a short packet ended on if it has ISP. */
static void td_report_bei(HvXhci *x, int slot, int dci, const HvXhciTd *td, uint32_t moved, int bei_ok) {
    uint32_t left = moved, edtla = 0;
    int reported = 0, shortpkt = 0;
    for (int i = 0; i < td->n && !reported; i++) {
        const uint32_t *t = td->trb[i];
        int type = (t[3] >> 10) & 0x3f;
        uint32_t chunk = 0;
        if (type == TRB_NORMAL || type == TRB_ISOCH) {
            chunk = t[2] & 0x1ffff;
            if (chunk > left) chunk = left, shortpkt = 1;
            left -= chunk;
            edtla += chunk;
        }
        int ioc = (t[3] >> 5) & 1, isp = (t[3] >> 2) & 1;
        if (!ioc && !(shortpkt && isp)) continue;
        uint32_t cc = shortpkt ? CC_SHORT_PACKET : CC_SUCCESS;
        uint32_t ctl = TRB_TRANSFER_EVENT << 10 | (uint32_t)dci << 16 | (uint32_t)slot << 24;
        int bei = bei_ok && (t[3] >> 9 & 1);
        if (type == TRB_EVENT_DATA) /* the event carries the TRB's data and the bytes moved so far */
            event_to_bei(x, (int)(t[2] >> 22), (uint64_t)t[1] << 32 | t[0], cc << 24 | (edtla & 0xffffff), ctl | 1u << 2, bei);
        else
            event_to_bei(x, (int)(t[2] >> 22), td->at[i], cc << 24 | (((t[2] & 0x1ffff) - chunk) & 0xffffff), ctl, bei);
        reported = 1;
    }
}

static void td_report(HvXhci *x, int slot, int dci, const HvXhciTd *td, uint32_t moved) {
    td_report_bei(x, slot, dci, td, moved, 0);
}

/* ---- isochronous transfers ---- */

static int64_t frame_now(void) {
    return (int64_t)(hv_counter() / TICKS_PER_FRAME);
}

/* An isochronous TD's data, to its device or from it: the bytes moved. */
static uint32_t iso_xfer(HvXhci *x, int slot, int dci, int in, uint8_t *buf, uint32_t len) {
    if (slot == x->spk.slot && x->spk.dev && dci == HV_USB_AUDIO_DCI_ISO_OUT && !in) {
        hv_usb_audio_iso_out(x->spk.dev, buf, len);
        return len;
    }
    return in ? 0 : len; /* nobody's: an OUT is taken, an IN has nothing */
}

/* Run the TDs on an isochronous endpoint whose frame has ended: each at the end of the 1 ms frame its
   Frame ID names (the nearest one with those 11 bits), or, with SIA, in the frame after the last one's
   (the next one, if none ran). One whose frame has passed runs at once: the CPU thread is late, not the
   guest. The first still to come sets iso_due. */
static void run_iso(HvXhci *x, int slot, int dci) {
    HvXhciSlot *sl = &x->slots[slot];
    if (!sl->ep_running[dci]) return;
    uint64_t ctx = out_ctx(x, slot) + 32ull * dci;
    uint32_t ival = (rd32(x, ctx) >> 16) & 0xff, type = (rd32(x, ctx + 4) >> 3) & 7;
    int64_t period = ival > 3 && ival < 16 ? 1ll << (ival - 3) : 1; /* 2^Interval microframes, at least a frame */
    int in = type == 5;
    int64_t now = frame_now();
    HvXhciTd *td = &sl->td;
    static uint8_t buf[16 * 3072]; /* CPU thread: up to 16 TRBs of the largest high-bandwidth packets */
    for (int guard = 0; guard < 256; guard++) {
        int g = td_gather(x, &sl->ep[dci], td);
        if (g < 0) {
            if (x->m->log) fprintf(stderr, "hengevm: xhci: an isochronous TD of more than %d TRBs\n", HV_XHCI_TD_MAX);
            sl->ep[dci] = td->after; /* not followed: dropped */
            continue;
        }
        if (g == 0) return;
        const uint32_t *t0 = td->trb[0];
        int64_t frame;
        if (((t0[3] >> 10) & 0x3f) != TRB_ISOCH) frame = now - 1; /* a No Op (or stray Normal) TD: now */
        else if (t0[3] >> 31) /* SIA */
            frame = sl->iso_valid[dci] && sl->iso_next[dci] > now - 32 ? sl->iso_next[dci] : now;
        else {
            int64_t d = (int64_t)((((t0[3] >> 20) & 0x7ff) - (uint64_t)now) & 0x7ff);
            frame = now + (d >= 1024 ? d - 2048 : d);
        }
        if (frame >= now) { /* its frame hasn't ended */
            uint64_t due = (uint64_t)(frame + 1) * TICKS_PER_FRAME;
            if (!x->iso_due || due < x->iso_due) x->iso_due = due;
            return;
        }
        uint32_t n = 0;
        for (int i = 0; i < td->n; i++) {
            const uint32_t *t = td->trb[i];
            int tt = (t[3] >> 10) & 0x3f;
            if (tt != TRB_ISOCH && tt != TRB_NORMAL) continue;
            uint32_t len = t[2] & 0x1ffff;
            if (len > sizeof buf - n) len = (uint32_t)sizeof buf - n;
            if (!in) {
                if (t[3] & (1u << 6)) memcpy(buf + n, t, len > 8 ? (len = 8) : len); /* immediate data */
                else rd(x, (uint64_t)t[1] << 32 | t[0], buf + n, len);
            }
            n += len;
        }
        uint32_t moved = iso_xfer(x, slot, dci, in, buf, n);
        if (in && moved) { /* back into the TRBs' buffers, in order */
            uint32_t off = 0;
            for (int i = 0; i < td->n && off < moved; i++) {
                const uint32_t *t = td->trb[i];
                int tt = (t[3] >> 10) & 0x3f;
                if (tt != TRB_ISOCH && tt != TRB_NORMAL) continue;
                uint32_t len = t[2] & 0x1ffff;
                if (len > moved - off) len = moved - off;
                wr(x, (uint64_t)t[1] << 32 | t[0], buf + off, len);
                off += len;
            }
        }
        sl->ep[dci] = td->after;
        sl->iso_next[dci] = frame + period;
        sl->iso_valid[dci] = 1;
        td_report_bei(x, slot, dci, td, moved, 1);
    }
}

/* Which endpoints are isochronous, from their output contexts (after a snapshot loads). */
static void iso_rescan(HvXhci *x) {
    for (int s = 1; s <= HV_XHCI_SLOTS; s++) {
        HvXhciSlot *sl = &x->slots[s];
        memset(sl->iso_valid, 0, sizeof sl->iso_valid);
        for (int dci = 2; dci < HV_XHCI_EPS; dci++) {
            uint32_t type = x->slot_used[s] && sl->port ? (rd32(x, out_ctx(x, s) + 32ull * dci + 4) >> 3) & 7 : 0;
            sl->ep_iso[dci] = type == 1 || type == 5;
        }
    }
}

/* Finish a TD the drive's thread has: 1 if there was none or it is done (reported), 0 if it still works. */
static int drive_finish(HvXhci *x, int slot, struct HvUsbStorage *d, int wait) {
    HvXhciSlot *sl = &x->slots[slot];
    uint32_t moved;
    for (;;) {
        if (!hv_usb_storage_busy(d)) return 1;
        if (hv_usb_storage_done(d, &moved)) break;
        if (!wait) return 0;
        usleep(50);
    }
    if (sl->pend_dci) {
        int dci = sl->pend_dci;
        sl->pend_dci = 0;
        if (sl->ep_running[dci]) {
            td_report(x, slot, dci, &sl->td, moved);
            sl->ep[dci] = sl->td.after;
        }
    }
    return 1;
}

/* Hand the drive what its bulk endpoints have queued, while it takes it. */
static void drive_pump(HvXhci *x, int slot) {
    struct HvUsbStorage *d = slot_drive(x, slot);
    if (!d) return;
    HvXhciSlot *sl = &x->slots[slot];
    HvUsbSeg seg[HV_XHCI_TD_MAX];
    for (int guard = 0; guard < 256; guard++) {
        if (!drive_finish(x, slot, d, 0)) return;
        int progress = 0;
        for (int k = 0; k < 2; k++) {
            int dci = k ? DRIVE_DCI_IN : DRIVE_DCI_OUT;
            if (!sl->ep_running[dci]) continue;
            HvXhciTd *td = &sl->td;
            int g = td_gather(x, &sl->ep[dci], td);
            if (g < 0 && x->m->log) fprintf(stderr, "hengevm: xhci: a TD of more than %d TRBs\n", HV_XHCI_TD_MAX);
            if (g <= 0) continue;
            if (!td->len) { /* nothing to move (a No Op): done as it is */
                td_report(x, slot, dci, td, 0);
                sl->ep[dci] = td->after;
                progress = 1;
                continue;
            }
            int nseg = 0, bad = 0;
            for (int i = 0; i < td->n; i++) {
                uint32_t *t = td->trb[i];
                if (((t[3] >> 10) & 0x3f) != TRB_NORMAL || !(t[2] & 0x1ffff)) continue;
                uint32_t len = t[2] & 0x1ffff;
                uint64_t pa = (uint64_t)t[1] << 32 | t[0];
                void *p;
                if (t[3] & (1u << 6)) /* immediate data, in the TRB itself */
                    p = t, pa = 0, len = len > 8 ? 8 : len;
                else if (dci == DRIVE_DCI_IN)
                    p = hv_phys_ptr(x->m, pa, len);
                else /* to the drive: only read (a page lent stays dirty until the device says it's done) */
                    p = (void *)(uintptr_t)hv_phys_ptr_r(x->m, pa, len);
                if (!p) bad = 1, p = t;
                seg[nseg++] = (HvUsbSeg){p, pa, len};
            }
            int r = bad ? 0 : hv_usb_storage_td(d, dci == DRIVE_DCI_IN, seg, nseg, td->len);
            if (bad && x->m->log) fprintf(stderr, "hengevm: xhci: a drive's buffer is outside guest RAM\n");
            if (r == HV_USB_NAK) continue;
            if (r == HV_USB_PENDING) {
                sl->pend_dci = dci;
                return;
            }
            td_report(x, slot, dci, td, (uint32_t)r);
            sl->ep[dci] = td->after;
            progress = 1;
        }
        if (!progress) return;
    }
}

/* A drive's slot stops: whatever its thread does finishes, and nothing more goes to it. */
static void drive_quiesce(HvXhci *x, int slot) {
    struct HvUsbStorage *d = slot_drive(x, slot);
    if (d) drive_finish(x, slot, d, 1);
    x->slots[slot].pend_dci = 0;
}

/* ---- the camera: its bulk endpoint ---- */

static uint64_t mono_ns(void);

/* Each tick and when the guest rings: the camera says whether a frame is due, and fills the TDs waiting. */
static void cam_pump(HvXhci *x) {
    struct HvXhciCam *c = &x->cam;
    int slot = c->slot;
    if (!c->present || !slot) return;
    HvXhciSlot *sl = &x->slots[slot];
    HvUsbSeg seg[HV_XHCI_TD_MAX];
    uint64_t now = mono_ns();
    for (int guard = 0; guard < 1024; guard++) {
        HvXhciTd *td = &sl->td;
        int g = sl->ep_running[CAM_DCI_IN] ? td_gather(x, &sl->ep[CAM_DCI_IN], td) : 0;
        if (g < 0 && x->m->log) fprintf(stderr, "hengevm: xhci: a TD of more than %d TRBs\n", HV_XHCI_TD_MAX);
        if (!guard) hv_usb_video_tick(c->dev, g > 0, now);
        if (g <= 0) return;
        int nseg = 0, bad = 0;
        for (int i = 0; i < td->n; i++) {
            uint32_t *t = td->trb[i];
            if (((t[3] >> 10) & 0x3f) != TRB_NORMAL || !(t[2] & 0x1ffff)) continue;
            uint32_t len = t[2] & 0x1ffff;
            uint64_t pa = (uint64_t)t[1] << 32 | t[0];
            void *p = hv_phys_ptr(x->m, pa, len);
            if (!p) bad = 1;
            seg[nseg++] = (HvUsbSeg){p, pa, len};
        }
        int r = 0;
        if (td->len && !bad) {
            r = hv_usb_video_td(c->dev, seg, nseg, td->len, now);
            if (r == HV_USB_NAK) return;
        }
        if (bad && x->m->log) fprintf(stderr, "hengevm: xhci: the camera's buffer is outside guest RAM\n");
        td_report(x, slot, CAM_DCI_IN, td, (uint32_t)r);
        sl->ep[CAM_DCI_IN] = td->after;
    }
}

static void run_commands(HvXhci *x) {
    uint32_t trb[4];
    uint64_t at;
    while (ring_peek(x, &x->cmd, trb, &at)) {
        ring_advance(&x->cmd);
        int type = (trb[3] >> 10) & 0x3f;
        int slot = (int)(trb[3] >> 24);
        uint64_t ptr = ((uint64_t)trb[1] << 32 | trb[0]) & ~0xfull;
        int cc = CC_SUCCESS;
        if (xlog()) fprintf(stderr, "xhci: command %d slot %d ptr %llx dw3 %08x\n", type, slot, (unsigned long long)ptr, trb[3]);
        if (type != TRB_ENABLE_SLOT && type != TRB_NOOP_CMD && (slot < 1 || slot > HV_XHCI_SLOTS || !x->slot_used[slot])) {
            cmd_completion(x, at, CC_SLOT_NOT_ENABLED, slot);
            continue;
        }
        switch (type) {
        case TRB_ENABLE_SLOT: {
            int s = 1;
            while (s <= HV_XHCI_SLOTS && x->slot_used[s]) s++;
            if (s > HV_XHCI_SLOTS) {
                cmd_completion(x, at, CC_SLOTS_FULL, 0);
                continue;
            }
            x->slot_used[s] = 1;
            memset(&x->slots[s], 0, sizeof x->slots[s]);
            cmd_completion(x, at, CC_SUCCESS, s);
            continue;
        }
        case TRB_DISABLE_SLOT: {
            drive_quiesce(x, slot);
            int port = x->slots[slot].port;
            if (slot == x->kbd_slot) x->kbd_slot = 0;
            if (slot == x->touch.slot) touch_set(x, 0, 0), touch_forget(x);
            if (slot == x->spk.slot) x->spk.slot = 0, x->spk.out_pending = 0, hv_usb_audio_reset(x->spk.dev);
            if (slot == x->cam.slot) x->cam.slot = 0, hv_usb_video_reset(x->cam.dev);
            if (drive_port(port) && x->port[port - 1].slot == slot) x->port[port - 1].slot = 0;
            memset(&x->slots[slot], 0, sizeof x->slots[slot]);
            x->slot_used[slot] = 0;
            break;
        }
        case TRB_ADDRESS_DEVICE:
            cc = address_device(x, slot, ptr, (trb[3] >> 9) & 1);
            break;
        case TRB_CONFIGURE_EP:
            cc = configure_endpoints(x, slot, ptr, (trb[3] >> 9) & 1);
            break;
        case TRB_EVALUATE_CTX: { /* EP0 max packet size, slot fields: copy what was added */
            uint32_t add = rd32(x, ptr + 4);
            uint64_t out = out_ctx(x, slot);
            if (add & 2) {
                uint32_t dw1 = rd32(x, ptr + 64 + 4);
                wr(x, out + 32 + 4, &dw1, 4);
            }
            break;
        }
        case TRB_RESET_EP: {
            int dci = (trb[3] >> 16) & 0x1f;
            set_ep_state(x, slot, dci, 3); /* stopped */
            break;
        }
        case TRB_STOP_EP: {
            int dci = (trb[3] >> 16) & 0x1f;
            drive_quiesce(x, slot); /* a TD the drive was on completes first */
            x->slots[slot].ep_running[dci] = 0;
            x->slots[slot].iso_valid[dci] = 0;
            set_ep_state(x, slot, dci, 3);
            /* The dequeue pointer goes back into the context for the driver to read. */
            uint64_t deq = x->slots[slot].ep[dci].dequeue | (uint64_t)x->slots[slot].ep[dci].ccs;
            wr(x, out_ctx(x, slot) + 32ull * dci + 8, &deq, 8);
            break;
        }
        case TRB_SET_TR_DEQUEUE: {
            int dci = (trb[3] >> 16) & 0x1f;
            uint64_t p = (uint64_t)trb[1] << 32 | trb[0];
            drive_quiesce(x, slot);
            if (slot == x->cam.slot && dci == CAM_DCI_IN) hv_usb_video_abort(x->cam.dev); /* its TDs were dropped */
            x->slots[slot].ep[dci].dequeue = p & ~0xfull;
            x->slots[slot].ep[dci].ccs = (int)(p & 1);
            x->slots[slot].iso_valid[dci] = 0;
            wr(x, out_ctx(x, slot) + 32ull * dci + 8, &p, 8);
            break;
        }
        case TRB_RESET_DEVICE: {
            drive_quiesce(x, slot);
            for (int dci = 2; dci < HV_XHCI_EPS; dci++) x->slots[slot].ep_running[dci] = 0;
            uint32_t dw3 = (uint32_t)1 << 27; /* default state, address 0 */
            wr(x, out_ctx(x, slot) + 12, &dw3, 4);
            struct HvUsbStorage *d = slot_drive(x, slot);
            if (d) hv_usb_storage_reset(d);
            else if (slot == x->touch.slot) touch_set(x, x->touch.slot, 0), touch_forget(x);
            else if (slot == x->spk.slot) x->spk.out_pending = 0, hv_usb_audio_reset(x->spk.dev);
            else if (slot == x->cam.slot) hv_usb_video_reset(x->cam.dev);
            else if (slot == x->kbd_slot) x->configured = 0;
            break;
        }
        case TRB_NOOP_CMD:
            break;
        default:
            if (x->m->log) fprintf(stderr, "hengevm: xhci: unhandled command TRB type %d\n", type);
            cc = CC_TRB_ERROR;
            break;
        }
        cmd_completion(x, at, cc, slot);
    }
}

/* ---- ports ---- */

/* Port index p's PORTSC (the touchscreen's, speaker's and camera's live apart: snapshots hold HV_XHCI_PORTS of the
   rest). */
static uint32_t *psc(HvXhci *x, int p) {
    if (p == TOUCH_P) return &x->touch.portsc;
    if (p == SPK_P) return &x->spk.portsc;
    if (p == CAM_P) return &x->cam.portsc;
    return p < HV_XHCI_PORTS ? &x->portsc[p] : &x->idle_portsc;
}

/* A device on port index p (0 the keyboard) is there: connected, waiting for a reset. */
static uint32_t connected(int p) {
    int full = p == 0 || p == TOUCH_P; /* the keyboard and touchscreen are full speed, drives high */
    return PORT_PP | PORT_CCS | PORT_CSC | (uint32_t)(full ? SPEED_FULL : SPEED_HIGH) << PORT_SPEED_SHIFT | (uint32_t)PLS_POLLING << PORT_PLS_SHIFT;
}

static void port_connect(HvXhci *x, int p) {
    *psc(x, p) = connected(p);
    port_event(x, p);
}

static void port_disconnect(HvXhci *x, int p) {
    *psc(x, p) = PORT_PP | PORT_CSC | (uint32_t)PLS_RXDETECT << PORT_PLS_SHIFT;
    port_event(x, p);
}

static int touch_sleeps_said; /* CPU thread: how often the touchscreen's sleeping and waking were logged */

static void port_write(HvXhci *x, int p, uint32_t v) {
    if (xlog()) fprintf(stderr, "xhci: portsc %d write %08x (was %08x)\n", p + 1, v, *psc(x, p));
    if (p == TOUCH_P && (v & PORT_LWS) && touch_sleeps_said < 20) {
        uint32_t pls = (v >> PORT_PLS_SHIFT) & 0xf;
        if (pls == 3) touch_sleeps_said++, fprintf(stderr, "hengevm: xhci: macOS put the touchscreen to sleep (USB suspend)\n");
        else if (pls == 0 && ((*psc(x, p) >> PORT_PLS_SHIFT) & 0xf) != 0)
            touch_sleeps_said++, fprintf(stderr, "hengevm: xhci: macOS woke the touchscreen\n");
    }
    uint32_t s = *psc(x, p);
    s &= ~(v & PORT_CHANGES); /* write 1 to clear */
    if (v & PORT_PED) s &= ~PORT_PED;
    if ((v & PORT_PR) && (s & PORT_CCS)) { /* reset: enabled, in U0 */
        if (drive_port(p + 1) && x->port[p].drive) {
            if (x->port[p].slot) drive_quiesce(x, x->port[p].slot);
            hv_usb_storage_reset(x->port[p].drive);
        }
        if (p == TOUCH_P) touch_set(x, x->touch.slot, 0), touch_forget(x);
        if (p == SPK_P) x->spk.out_pending = 0, hv_usb_audio_reset(x->spk.dev);
        if (p == CAM_P && x->cam.dev) hv_usb_video_reset(x->cam.dev);
        s = (s & ~(0xfu << PORT_PLS_SHIFT)) | PORT_PED | PORT_PRC;
        *psc(x, p) = s;
        port_event(x, p);
        return;
    }
    if ((v & PORT_LWS) && (s & PORT_CCS)) {
        uint32_t pls = (v >> PORT_PLS_SHIFT) & 0xf;
        s = (s & ~(0xfu << PORT_PLS_SHIFT)) | pls << PORT_PLS_SHIFT;
        if (pls == 0 || pls == 15) { /* U0 or resume: report the link change */
            s = (s & ~(0xfu << PORT_PLS_SHIFT)) | PORT_PLC;
            *psc(x, p) = s;
            port_event(x, p);
            return;
        }
    }
    s = (s & ~PORT_PP) | (v & PORT_PP);
    *psc(x, p) = s;
}

/* ---- registers ---- */

static void reset(HvXhci *x) {
    for (int p = 1; p < HV_XHCI_PORTS; p++) {
        if (x->port[p].slot) drive_quiesce(x, x->port[p].slot);
        x->port[p].slot = 0;
        if (x->port[p].drive) hv_usb_storage_reset(x->port[p].drive);
    }
    x->usbcmd = 0;
    x->usbsts = 1; /* halted */
    x->dnctrl = x->config = 0;
    x->dcbaap = 0;
    memset(&x->cmd, 0, sizeof x->cmd);
    memset(x->intr, 0, sizeof x->intr);
    for (int i = 0; i < HV_XHCI_INTRS; i++) x->intr[i].pcs = 1;
    memset(x->slot_used, 0, sizeof x->slot_used);
    memset(x->slots, 0, sizeof *x->slots * (HV_XHCI_SLOTS + 1));
    x->kbd_slot = 0;
    x->configured = 0;
    touch_set(x, 0, 0);
    touch_forget(x);
    x->spk.slot = 0, x->spk.out_pending = 0;
    if (x->spk.dev) hv_usb_audio_reset(x->spk.dev);
    x->iso_due = 0;
    x->cam.slot = 0;
    if (x->cam.dev) hv_usb_video_reset(x->cam.dev);
    for (int p = 0; p < HV_XHCI_ROOT_PORTS; p++) {
        int there = p == 0 || (p == TOUCH_P ? x->touch.present : p == SPK_P ? x->spk.present : p == CAM_P ? x->cam.present :
                               p < HV_XHCI_PORTS && x->port[p].drive != NULL);
        *psc(x, p) = there ? connected(p) : PORT_PP | (uint32_t)PLS_RXDETECT << PORT_PLS_SHIFT;
    }
    update_irq(x);
}

static uint64_t bar_read(HvPciDev *d, int bar, uint64_t off, unsigned size) {
    HvXhci *x = d->opaque;
    if (bar != 0) return 0;
    uint64_t v = 0;
    if (xlog()) {
        static int n;
        if (n++ < 60) fprintf(stderr, "xhci: read +%llx (%u)\n", (unsigned long long)off, size);
    }
    if (off < CAPLENGTH) {
        const uint32_t cap[8] = {
            CAPLENGTH | 0x0100u << 16,                           /* CAPLENGTH, HCIVERSION 1.0 */
            HV_XHCI_SLOTS | (uint32_t)HV_XHCI_INTRS << 8 | (uint32_t)x->nports << 24, /* slots, interrupters, ports */
            1u << 4 | 0x8 | 2,                                   /* ERST max 2 segments; isochronous TDs 2 frames ahead */
            0,
            1u | (uint32_t)(XECP / 4) << 16,                     /* 64-bit addressing, extended caps */
            DB,
            RT,
            0,
        };
        uint32_t i = (uint32_t)off / 4;
        v = cap[i];
        if (size == 8 && i + 1 < 8) v |= (uint64_t)cap[i + 1] << 32;
        v >>= 8 * (off & 3);
    } else if (off >= XECP && off < XECP + 16) { /* Supported Protocol: USB 2.0 on every port */
        const uint32_t ext[4] = {2u | 0x0200u << 16, 0x20425355, 1u | (uint32_t)x->nports << 8, 0};
        uint32_t i = (uint32_t)(off - XECP) / 4;
        v = ext[i];
        if (size == 8 && i + 1 < 4) v |= (uint64_t)ext[i + 1] << 32;
    } else if (off >= OP && off < PORTREGS) {
        switch (off - OP) {
        case 0x00: v = x->usbcmd; break;
        case 0x04: v = x->usbsts; break;
        case 0x08: v = 1; break; /* 4 KB pages */
        case 0x14: v = x->dnctrl; break;
        case 0x18: v = x->cmd_running ? 8 : 0; break; /* CRCR reads CRR only */
        case 0x1c: v = 0; break;
        case 0x30: v = size == 8 ? x->dcbaap : (uint32_t)x->dcbaap; break;
        case 0x34: v = x->dcbaap >> 32; break;
        case 0x38: v = x->config; break;
        }
    } else if (off >= PORTREGS && off < PORTREGS + 16u * (unsigned)x->nports) {
        int p = (int)(off - PORTREGS) / 16;
        v = (off - PORTREGS) % 16 == 0 ? *psc(x, p) : 0;
    } else if (off >= RT && off < RT + 0x20 + 32u * HV_XHCI_INTRS) {
        if (off - RT < 0x20) /* MFINDEX: the guest's time in microframes, while the controller runs */
            return off - RT == 0 && (x->usbcmd & 1) ? (hv_counter() / TICKS_PER_UFRAME) & 0x3fff : 0;
        struct HvXhciIntr *it = &x->intr[(off - RT - 0x20) / 32];
        switch ((off - RT - 0x20) % 32) {
        case 0x00: v = it->iman; break;
        case 0x04: v = it->imod; break;
        case 0x08: v = it->erstsz; break;
        case 0x10: v = size == 8 ? it->erstba : (uint32_t)it->erstba; break;
        case 0x14: v = it->erstba >> 32; break;
        case 0x18: v = size == 8 ? it->erdp : (uint32_t)it->erdp; break;
        case 0x1c: v = it->erdp >> 32; break;
        }
    }
    return v;
}

static void set64(uint64_t *f, uint64_t off_in_reg, uint64_t v, unsigned size) {
    if (size == 8) *f = v;
    else if (off_in_reg) *f = (*f & 0xffffffffull) | v << 32;
    else *f = (*f & ~0xffffffffull) | (uint32_t)v;
}

static void bar_write(HvPciDev *d, int bar, uint64_t off, uint64_t v, unsigned size) {
    HvXhci *x = d->opaque;
    if (bar != 0) return;
    if (xlog() && off < DB) fprintf(stderr, "xhci: write +%llx = %llx (%u)\n", (unsigned long long)off, (unsigned long long)v, size);
    if (off >= OP && off < PORTREGS) {
        uint64_t r = off - OP;
        switch (r) {
        case 0x00:
            if (v & 2) { /* HCRST */
                reset(x);
                x->usbsts |= 1;
                return;
            }
            if ((v & 1) && !(x->usbcmd & 1)) { /* run */
                x->usbcmd = (uint32_t)v;
                x->usbsts &= ~1u;
                for (int p = 0; p < x->nports; p++)
                    if (*psc(x, p) & PORT_CSC) port_event(x, p);
            } else if (!(v & 1)) {
                x->usbcmd = (uint32_t)v;
                x->usbsts |= 1;
            } else
                x->usbcmd = (uint32_t)v;
            update_irq(x);
            break;
        case 0x04:
            x->usbsts &= ~((uint32_t)v & (1u << 2 | 1u << 3 | 1u << 4 | 1u << 10)); /* HSE, EINT, PCD, SRE: write 1 to clear */
            break;
        case 0x14: x->dnctrl = (uint32_t)v; break;
        case 0x18:
        case 0x1c: {
            uint64_t crcr = 0;
            set64(&crcr, r - 0x18, v, size);
            if (r == 0x18 && size == 4) { /* low half: pointer low bits and RCS */
                x->cmd.dequeue = (x->cmd.dequeue & ~0xffffffffull) | ((uint32_t)v & ~0x3fu);
                x->cmd.ccs = (int)(v & 1);
            } else if (r == 0x1c) {
                x->cmd.dequeue = (x->cmd.dequeue & 0xffffffffull) | v << 32;
            } else {
                x->cmd.dequeue = v & ~0x3full;
                x->cmd.ccs = (int)(v & 1);
            }
            break;
        }
        case 0x30:
        case 0x34: set64(&x->dcbaap, r - 0x30, v, size); x->dcbaap &= ~0x3full; break;
        case 0x38: x->config = (uint32_t)v; break;
        }
    } else if (off >= PORTREGS && off < PORTREGS + 16u * (unsigned)x->nports) {
        if ((off - PORTREGS) % 16 == 0) port_write(x, (int)(off - PORTREGS) / 16, (uint32_t)v);
    } else if (off >= RT + 0x20 && off < RT + 0x20 + 32u * HV_XHCI_INTRS) {
        struct HvXhciIntr *it = &x->intr[(off - RT - 0x20) / 32];
        uint64_t r = (off - RT - 0x20) % 32;
        switch (r) {
        case 0x00:
            if (v & 1) it->iman &= ~1u; /* IP: write 1 to clear */
            it->iman = (it->iman & 1) | ((uint32_t)v & 2);
            update_irq(x);
            break;
        case 0x04: it->imod = (uint32_t)v; break;
        case 0x08: it->erstsz = (uint32_t)v & 0xffff; break;
        case 0x10:
        case 0x14:
            set64(&it->erstba, r - 0x10, v, size);
            if (r == 0x14 || size == 8) { /* the high half (or all of it) is written last */
                it->erstba &= ~0x3full;
                it->seg_index = 0;
                it->pcs = 1;
                load_segment(x, it);
            }
            break;
        case 0x18:
        case 0x1c:
            set64(&it->erdp, r - 0x18, v, size);
            if (r == 0x18 && (v & 8)) it->erdp &= ~8ull; /* EHB: write 1 to clear */
            break;
        }
    } else if (off >= DB && off < DB + 4u * (HV_XHCI_SLOTS + 1)) {
        int slot = (int)(off - DB) / 4;
        if (slot == 0) {
            x->cmd_running = 1;
            run_commands(x);
        } else if (x->slot_used[slot] && x->slots[slot].port) {
            int dci = (int)(v & 0xff);
            if (dci < 1 || dci >= HV_XHCI_EPS) return;
            /* A doorbell starts a stopped endpoint again (a halted one waits for Reset Endpoint). */
            uint32_t state = rd32(x, out_ctx(x, slot) + 32ull * dci) & 7;
            if (state == 2) return;
            if (state != 1) set_ep_state(x, slot, dci, 1);
            x->slots[slot].ep_running[dci] = 1;
            if (dci == 1) run_ep0(x, slot);
            else if (x->slots[slot].ep_iso[dci]) run_iso(x, slot, dci);
            else if (slot == x->spk.slot) run_speaker_in(x);
            else if (slot == x->kbd_slot) {
                if (dci == KBD_DCI_IN) run_ep_in(x);
            } else if (slot == x->touch.slot) {
                if (dci == TOUCH_DCI_IN) run_touch_in(x);
            } else if (slot == x->cam.slot) {
                if (dci == CAM_DCI_IN) cam_pump(x);
            } else
                drive_pump(x, slot);
        }
    }
}

void hv_xhci_key(HvXhci *x, uint8_t usage, int down) {
    if (!x || !usage) return;
    pthread_mutex_lock(&x->mu);
    uint8_t *s = x->state;
    if (usage >= 0xe0 && usage <= 0xe7) { /* modifiers live in byte 0 */
        uint8_t bit = (uint8_t)(1u << (usage - 0xe0));
        s[0] = down ? s[0] | bit : s[0] & ~bit;
    } else {
        int i;
        for (i = 2; i < 8 && s[i] != usage; i++) {}
        if (down && i == 8) { /* not held yet: take a free place */
            for (i = 2; i < 8 && s[i]; i++) {}
            if (i < 8) s[i] = usage;
        } else if (!down && i < 8) { /* let go: close the gap */
            memmove(s + i, s + i + 1, (size_t)(7 - i));
            s[7] = 0;
        }
    }
    if (x->rtail - x->rhead < HV_XHCI_REPORTS) memcpy(x->reports[x->rtail++ % HV_XHCI_REPORTS], s, 8);
    pthread_mutex_unlock(&x->mu);
    hv_machine_wake(x->m);
}

void hv_xhci_release_keys(HvXhci *x) {
    if (!x) return;
    pthread_mutex_lock(&x->mu);
    static const uint8_t none[8];
    int held = memcmp(x->state, none, 8) != 0;
    if (held) {
        memset(x->state, 0, 8);
        if (x->rtail - x->rhead < HV_XHCI_REPORTS) memcpy(x->reports[x->rtail++ % HV_XHCI_REPORTS], none, 8);
    }
    pthread_mutex_unlock(&x->mu);
    if (held) hv_machine_wake(x->m);
}

void hv_xhci_release_modifiers(HvXhci *x) {
    if (!x) return;
    pthread_mutex_lock(&x->mu);
    int held = x->state[0] != 0;
    if (held) {
        x->state[0] = 0;
        if (x->rtail - x->rhead < HV_XHCI_REPORTS) memcpy(x->reports[x->rtail++ % HV_XHCI_REPORTS], x->state, 8);
    }
    pthread_mutex_unlock(&x->mu);
    if (held) hv_machine_wake(x->m);
}

/* Linux input key codes to HID keyboard usages (the same table macOS's virtio
   keyboard driver uses), modifiers included. */
static const uint8_t linux_to_hid[256] = {
    0x00, 0x29, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2d, 0x2e, 0x2a, 0x2b,
    0x14, 0x1a, 0x08, 0x15, 0x17, 0x1c, 0x18, 0x0c, 0x12, 0x13, 0x2f, 0x30, 0x28, 0xe0, 0x04, 0x16,
    0x07, 0x09, 0x0a, 0x0b, 0x0d, 0x0e, 0x0f, 0x33, 0x34, 0x35, 0xe1, 0x31, 0x1d, 0x1b, 0x06, 0x19,
    0x05, 0x11, 0x10, 0x36, 0x37, 0x38, 0xe5, 0x55, 0xe2, 0x2c, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e,
    0x3f, 0x40, 0x41, 0x42, 0x43, 0x53, 0x47, 0x5f, 0x60, 0x61, 0x56, 0x5c, 0x5d, 0x5e, 0x57, 0x59,
    0x5a, 0x5b, 0x62, 0x63, 0x00, 0x00, 0x00, 0x44, 0x45, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x58, 0xe4, 0x54, 0x00, 0xe6, 0x00, 0x4a, 0x52, 0x4b, 0x50, 0x4f, 0x4d, 0x51, 0x4e, 0x49, 0x00,
    0x00, 0x7f, 0x81, 0x80, 0x66, 0x67, 0x00, 0x48, 0x00, 0x85, 0x00, 0x00, 0x00, 0xe3, 0xe7, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f, 0x70,
    0x71, 0x72, 0x73, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

void hv_xhci_key_linux(HvXhci *x, uint16_t code, int down) {
    if (code < 256) hv_xhci_key(x, linux_to_hid[code], down);
}

static void *free_drive(void *d) {
    hv_usb_storage_free(d);
    return NULL;
}

/* Plugs and unplugs the host asked for. */
static void apply_changes(HvXhci *x) {
    int again = 0;
    pthread_mutex_lock(&x->mu);
    __atomic_store_n(&x->changes, 0, __ATOMIC_RELAXED);
    for (int p = 1; p < HV_XHCI_PORTS; p++) {
        struct HvXhciPort *pt = &x->port[p];
        if (pt->unplug && pt->drive) {
            if (pt->slot && hv_usb_storage_busy(pt->drive)) {
                again = 1; /* its thread finishes the TD first (drive_pump, next tick) */
                continue;
            }
            if (pt->slot) {
                HvXhciSlot *sl = &x->slots[pt->slot];
                sl->port = 0; /* the slot stays the driver's until it disables it; nothing reaches the drive */
                memset(sl->ep_running, 0, sizeof sl->ep_running);
                sl->pend_dci = 0;
            }
            struct HvUsbStorage *d = pt->drive;
            pt->drive = NULL;
            pt->slot = 0;
            pt->unplug = 0;
            port_disconnect(x, p);
            fprintf(stderr, "hengevm: xhci: a drive left port %d\n", p + 1);
            pthread_t t; /* it writes what it holds to the file: not on the CPU thread */
            if (pthread_create(&t, NULL, free_drive, d) == 0) pthread_detach(t);
            else hv_usb_storage_free(d);
        }
        pt->unplug = 0;
        if (pt->plug && !pt->drive) {
            pt->drive = pt->plug;
            pt->plug = NULL;
            port_connect(x, p);
            fprintf(stderr, "hengevm: xhci: a drive is on port %d\n", p + 1);
        }
    }
    struct HvXhciTouch *t = &x->touch;
    if (t->want == 0 && t->present) {
        if (t->slot) {
            HvXhciSlot *sl = &x->slots[t->slot];
            sl->port = 0; /* as for a drive: the slot stays the driver's until it disables it */
            memset(sl->ep_running, 0, sizeof sl->ep_running);
        }
        __atomic_store_n(&t->present, 0, __ATOMIC_RELAXED);
        touch_set(x, 0, 0);
        t->qhead = t->qtail, t->lost = 0, t->mac_down = 0, t->mac_unknown = 0;
        touch_report_empty(t->cur);
        port_disconnect(x, TOUCH_P);
        fprintf(stderr, "hengevm: xhci: the touchscreen was unplugged\n");
    } else if (t->want == 1 && !t->present && x->nports >= HV_XHCI_TOUCH_PORT) {
        t->desc_len = touch_build_desc(t->desc, t->w_mm, t->h_mm);
        t->qhead = t->qtail, t->lost = 0, t->mac_down = 0, t->mac_unknown = 0;
        touch_report_empty(t->cur);
        __atomic_store_n(&t->present, 1, __ATOMIC_RELAXED);
        port_connect(x, TOUCH_P);
        fprintf(stderr, "hengevm: xhci: a touchscreen is on port %d (%d x %d mm)\n", HV_XHCI_TOUCH_PORT, t->w_mm, t->h_mm);
    }
    /* A plug the driver can't see (it was told of five ports) waits: for this run, it stays unavailable. */
    if (!(t->want == 1 && !t->present)) t->want = -1;
    struct HvXhciCam *c = &x->cam;
    if (c->dev && c->want == 0 && c->present) {
        if (c->slot) {
            HvXhciSlot *sl = &x->slots[c->slot];
            sl->port = 0; /* as for a drive: the slot stays the driver's until it disables it */
            memset(sl->ep_running, 0, sizeof sl->ep_running);
        }
        __atomic_store_n(&c->present, 0, __ATOMIC_RELAXED);
        c->slot = 0;
        hv_usb_video_reset(c->dev); /* the host's camera stops */
        port_disconnect(x, CAM_P);
        fprintf(stderr, "hengevm: xhci: the camera was unplugged\n");
    } else if (c->dev && c->want == 1 && !c->present && x->nports >= HV_XHCI_CAMERA_PORT) {
        hv_usb_video_reset(c->dev);
        __atomic_store_n(&c->present, 1, __ATOMIC_RELAXED);
        port_connect(x, CAM_P);
        fprintf(stderr, "hengevm: xhci: a camera (%s) is on port %d\n", hv_usb_video_name(c->dev), HV_XHCI_CAMERA_PORT);
    }
    if (!(c->want == 1 && !c->present)) c->want = -1;
    if (again) __atomic_store_n(&x->changes, 1, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&x->mu);
}

/* The isochronous endpoints' TDs that are due, and MFINDEX wraps. */
static void iso_poll(HvXhci *x) {
    uint64_t now = hv_counter();
    uint64_t wraps = now / MFINDEX_PERIOD;
    if (wraps != x->mf_wraps) {
        x->mf_wraps = wraps;
        if ((x->usbcmd & 1) && (x->usbcmd & (1u << 10))) event(x, 0, (uint32_t)CC_SUCCESS << 24, TRB_MFINDEX_WRAP << 10); /* EWE */
    }
    if (x->iso_due && now < x->iso_due) return;
    x->iso_due = 0;
    if (!(x->usbcmd & 1)) return;
    for (int s = 1; s <= HV_XHCI_SLOTS; s++) {
        if (!x->slot_used[s] || !x->slots[s].port) continue;
        for (int dci = 2; dci < HV_XHCI_EPS; dci++)
            if (x->slots[s].ep_iso[dci] && x->slots[s].ep_running[dci]) run_iso(x, s, dci);
    }
}

uint64_t hv_xhci_due(HvXhci *x) {
    if (!x) return 0;
    uint64_t due = x->iso_due;
    if ((x->usbcmd & 1) && (x->usbcmd & (1u << 10))) {
        uint64_t wrap = (x->mf_wraps + 1) * MFINDEX_PERIOD;
        if (!due || wrap < due) due = wrap;
    }
    return due;
}

struct HvUsbAudio *hv_xhci_speaker(HvXhci *x) {
    return x ? x->spk.dev : NULL;
}

void hv_xhci_poll(HvXhci *x) {
    if (!x) return;
    iso_poll(x);
    if (x->spk.slot && hv_usb_audio_pending(x->spk.dev)) run_speaker_in(x);
    if (x->rhead != x->rtail) run_ep_in(x);
    /* A touch on a touchscreen macOS put to sleep (U3): it signals a remote wakeup, as a real one does (it says it can),
       so macOS resumes the port and starts reading touches again instead of leaving them waiting for good. */
    if (x->touch.present && ((x->touch.portsc >> PORT_PLS_SHIFT) & 0xf) == 3 && x->touch.qhead != x->touch.qtail) {
        x->touch.portsc = (x->touch.portsc & ~(0xfu << PORT_PLS_SHIFT)) | 15u << PORT_PLS_SHIFT | PORT_PLC;
        port_event(x, TOUCH_P);
        if (touch_sleeps_said < 20) touch_sleeps_said++, fprintf(stderr, "hengevm: xhci: a touch wakes the sleeping touchscreen\n");
    }
    if (x->touch.qhead != x->touch.qtail || __atomic_load_n(&x->touch.lost, __ATOMIC_RELAXED)) run_touch_in(x);
    if (x->cam.present && x->cam.slot) cam_pump(x);
    for (int p = 1; p < HV_XHCI_PORTS; p++)
        if (x->port[p].drive && x->port[p].slot && hv_usb_storage_busy(x->port[p].drive)) drive_pump(x, x->port[p].slot);
    if (__atomic_load_n(&x->changes, __ATOMIC_ACQUIRE)) apply_changes(x);
}

int hv_xhci_plug(HvXhci *x, const char *path, int readonly, const char *name, const char *serial) {
    if (!x) return -1;
    struct HvUsbStorage *d = hv_usb_storage_open(x->m, path, readonly, name, serial);
    if (!d) return -1;
    pthread_mutex_lock(&x->mu);
    int p = 1;
    while (p < HV_XHCI_PORTS && (x->port[p].drive || x->port[p].plug)) p++;
    if (p < HV_XHCI_PORTS) {
        x->port[p].plug = d;
        __atomic_store_n(&x->changes, 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&x->mu);
    if (p == HV_XHCI_PORTS) {
        hv_usb_storage_free(d);
        return -2;
    }
    fprintf(stderr, "hengevm: xhci: plugging %s in (%s, %llu MB%s)\n", name ? name : "a drive", serial ? serial : "",
            (unsigned long long)(hv_usb_storage_size(d) >> 20), hv_usb_storage_readonly(d) ? ", read-only" : "");
    hv_machine_wake(x->m);
    return p + 1;
}

void hv_xhci_unplug(HvXhci *x, int port) {
    if (!x || port < 2 || port > HV_XHCI_PORTS) return;
    struct HvUsbStorage *never = NULL;
    pthread_mutex_lock(&x->mu);
    struct HvXhciPort *pt = &x->port[port - 1];
    if (pt->plug) never = pt->plug, pt->plug = NULL; /* not connected yet: it just doesn't arrive */
    else if (pt->drive) pt->unplug = 1, __atomic_store_n(&x->changes, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&x->mu);
    if (never) hv_usb_storage_free(never);
    hv_machine_wake(x->m);
}

int hv_xhci_drive_state(HvXhci *x, int port) {
    if (!x || port < 2 || port > HV_XHCI_PORTS) return HV_XHCI_DRIVE_EMPTY;
    pthread_mutex_lock(&x->mu);
    struct HvXhciPort *pt = &x->port[port - 1];
    int st = HV_XHCI_DRIVE_EMPTY;
    if (pt->plug) st = HV_XHCI_DRIVE_PLUGGED;
    else if (pt->drive) {
        if (pt->unplug) st = HV_XHCI_DRIVE_LEAVING;
        else if (hv_usb_storage_ejected(pt->drive)) st = HV_XHCI_DRIVE_EJECTED;
        else if (hv_usb_storage_configured(pt->drive)) st = HV_XHCI_DRIVE_IN_USE;
        else st = HV_XHCI_DRIVE_PLUGGED;
    }
    pthread_mutex_unlock(&x->mu);
    return st;
}

int hv_xhci_drive_readonly(HvXhci *x, int port) {
    if (!x || port < 2 || port > HV_XHCI_PORTS) return 0;
    pthread_mutex_lock(&x->mu);
    struct HvUsbStorage *d = x->port[port - 1].drive ? x->port[port - 1].drive : x->port[port - 1].plug;
    int ro = d ? hv_usb_storage_readonly(d) : 0;
    pthread_mutex_unlock(&x->mu);
    return ro;
}

int hv_xhci_flush_drives(HvXhci *x, int sync) {
    if (!x) return 0;
    int r = 0;
    pthread_mutex_lock(&x->mu);
    for (int p = 1; p < HV_XHCI_PORTS; p++)
        if (x->port[p].drive && hv_usb_storage_flush(x->port[p].drive, sync) != 0) r = -1;
    pthread_mutex_unlock(&x->mu);
    return r;
}

int hv_xhci_drives_busy(HvXhci *x) {
    if (!x) return 0;
    int busy = 0;
    pthread_mutex_lock(&x->mu);
    for (int p = 1; p < HV_XHCI_PORTS; p++)
        if (x->port[p].drive && hv_usb_storage_busy(x->port[p].drive)) busy = 1;
    pthread_mutex_unlock(&x->mu);
    return busy;
}

static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int hv_xhci_drives_settle(HvXhci *x, uint64_t deadline_ns) {
    if (!x) return 0;
    for (int p = 1; p < HV_XHCI_PORTS; p++) {
        struct HvUsbStorage *d = x->port[p].drive;
        int slot = x->port[p].slot;
        if (!d) continue;
        if (slot && x->slots[slot].port == p + 1) {
            for (int guard = 0;; guard++) {
                if (!drive_finish(x, slot, d, 0)) { /* reports the TD it had, once its thread is done */
                    if (mono_ns() > deadline_ns) return -2;
                    usleep(50);
                    continue;
                }
                if (guard == 65536) break;
                drive_pump(x, slot); /* what that let through, as the CPU thread would next */
                if (!hv_usb_storage_busy(d)) break;
            }
            x->slots[slot].pend_dci = 0;
        }
        uint32_t moved;
        while (hv_usb_storage_busy(d) && !hv_usb_storage_done(d, &moved)) { /* a flush with no slot to report to */
            if (mono_ns() > deadline_ns) return -2;
            usleep(50);
        }
    }
    return hv_xhci_flush_drives(x, 1);
}

HvXhci *hv_xhci_create(HvMachine *m, HvPcie *bus) {
    HvXhci *x = calloc(1, sizeof *x);
    x->m = m;
    x->slots = calloc(HV_XHCI_SLOTS + 1, sizeof *x->slots);
    pthread_mutex_init(&x->mu, NULL);
    x->touch.want = -1;
    x->cam.want = -1;
    x->nports = HV_XHCI_ROOT_PORTS; /* what its driver is told (a snapshot older than the touchscreen says fewer) */
    x->spk.dev = hv_usb_audio_create(m, NULL);
    x->spk.present = x->spk.dev != NULL;
    x->mf_wraps = hv_counter() / MFINDEX_PERIOD;
    /* QEMU's xHCI IDs. Class 0x0c0330 with revision 0: macOS's IOPCIClassMatch compares all 32 bits. */
    hv_pci_init(&x->pci, 0x1b36, 0x000d, 0x0c033000, 0x1af4, 0x1100);
    x->pci.opaque = x;
    x->pci.bar_read = bar_read;
    x->pci.bar_write = bar_write;
    hv_pci_add_bar(&x->pci, 0, BAR_SIZE, 1);
    x->pci.config[0x60] = 0x30; /* SBRN: USB 3.0 controller */
    x->pci.config[0x61] = 0x20; /* FLADJ default */
    if (hv_pcie_attach(bus, &x->pci) < 0) {
        free(x);
        return NULL;
    }
    reset(x);
    return x;
}

/* ---- the touchscreen ---- */

void hv_xhci_touch_plug(HvXhci *x, int on, int w_mm, int h_mm) {
    if (!x) return;
    pthread_mutex_lock(&x->mu);
    x->touch.want = on ? 1 : 0;
    if (on && !__atomic_load_n(&x->touch.present, __ATOMIC_RELAXED)) x->touch.w_mm = w_mm, x->touch.h_mm = h_mm;
    __atomic_store_n(&x->changes, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&x->mu);
    hv_machine_wake(x->m);
}

/* Ready: plugged in, configured, its interrupt endpoint running (macOS's HID driver reads it). */
static int touch_ready(HvXhci *x) {
    int slot = __atomic_load_n(&x->touch.slot, __ATOMIC_RELAXED);
    return __atomic_load_n(&x->touch.present, __ATOMIC_RELAXED) && __atomic_load_n(&x->touch.configured, __ATOMIC_RELAXED) &&
           slot > 0 && slot <= HV_XHCI_SLOTS && __atomic_load_n(&x->slots[slot].ep_running[TOUCH_DCI_IN], __ATOMIC_RELAXED);
}

/* Frames wait for it: plugged in and configured (its endpoint may be stopped for a moment: they wait for
   it to run again, rather than be dropped). */
static int touch_takes(HvXhci *x) {
    int slot = __atomic_load_n(&x->touch.slot, __ATOMIC_RELAXED);
    return __atomic_load_n(&x->touch.present, __ATOMIC_RELAXED) && __atomic_load_n(&x->touch.configured, __ATOMIC_RELAXED) &&
           slot > 0 && slot <= HV_XHCI_SLOTS;
}

int hv_xhci_touch_frame(HvXhci *x, int n, const HvXhciContact *c) {
    if (!x || n < 0) return -1;
    if (n > HV_XHCI_CONTACTS) n = HV_XHCI_CONTACTS;
    uint8_t r[HV_XHCI_TOUCH_REPORT];
    touch_report_empty(r);
    for (int i = 0; i < n; i++) {
        uint8_t *f = r + 1 + 6 * i;
        uint16_t px = c[i].x > 32767 ? 32767 : c[i].x, py = c[i].y > 32767 ? 32767 : c[i].y;
        f[0] = (c[i].down ? 3 : 0) | (c[i].cancel ? 0 : 4); /* Tip Switch, In Range, Confidence */
        f[1] = c[i].id;
        f[2] = (uint8_t)px, f[3] = (uint8_t)(px >> 8);
        f[4] = (uint8_t)py, f[5] = (uint8_t)(py >> 8);
    }
    r[HV_XHCI_TOUCH_REPORT - 1] = (uint8_t)n;
    pthread_mutex_lock(&x->mu);
    struct HvXhciTouch *t = &x->touch;
    if (!touch_takes(x)) {
        if (__atomic_load_n(&t->present, __ATOMIC_RELAXED)) __atomic_store_n(&t->lost, 1, __ATOMIC_RELAXED);
        pthread_mutex_unlock(&x->mu);
        return -1;
    }
    memcpy(t->cur, r, sizeof r);
    int queued = 1;
    if (t->lost && t->qtail - t->qhead < HV_XHCI_REPORTS - 1) { /* what was dropped: every finger lifts first (room for both) */
        touch_queue_lift_all(t);
        __atomic_store_n(&t->lost, 0, __ATOMIC_RELAXED);
    }
    /* A report still waiting that has the same fingers in the same states (they only moved) takes this
       one's places: the guest gets the latest, and the queue only grows when a finger goes down or up,
       so one is never lost to a full queue while the guest catches up. */
    int same = 0;
    if (t->qtail != t->qhead && !t->lost) {
        uint8_t *last = t->q[(t->qtail - 1) % HV_XHCI_REPORTS];
        same = last[0] == 1 && last[HV_XHCI_TOUCH_REPORT - 1] == r[HV_XHCI_TOUCH_REPORT - 1]; /* never a lift's marker */
        for (int i = 0; i < n && same; i++) same = last[1 + 6 * i] == r[1 + 6 * i] && last[2 + 6 * i] == r[2 + 6 * i];
        if (same) memcpy(last, r, sizeof r);
    }
    if (!same) {
        if (t->qtail - t->qhead < HV_XHCI_REPORTS && !t->lost) memcpy(t->q[t->qtail++ % HV_XHCI_REPORTS], r, sizeof r);
        else queued = 0, __atomic_store_n(&t->lost, 1, __ATOMIC_RELAXED);
    }
    pthread_mutex_unlock(&x->mu);
    hv_machine_wake(x->m);
    return queued ? 0 : -1;
}

int hv_xhci_touch_state(HvXhci *x) {
    if (!x) return HV_XHCI_TOUCH_ABSENT;
    pthread_mutex_lock(&x->mu);
    int st;
    if (touch_ready(x)) st = HV_XHCI_TOUCH_READY;
    else if (__atomic_load_n(&x->touch.present, __ATOMIC_RELAXED) || x->touch.want == 1)
        st = __atomic_load_n(&x->nports, __ATOMIC_RELAXED) < HV_XHCI_TOUCH_PORT ? HV_XHCI_TOUCH_UNAVAILABLE : HV_XHCI_TOUCH_PLUGGED;
    else st = HV_XHCI_TOUCH_ABSENT;
    pthread_mutex_unlock(&x->mu);
    return st;
}

void hv_xhci_touch_get_snap(HvXhci *x, HvXhciTouchSnap *s) {
    memset(s, 0, sizeof *s);
    s->nports = x->nports;
    s->present = x->touch.present;
    s->w_mm = x->touch.w_mm;
    s->h_mm = x->touch.h_mm;
    s->slot = x->touch.slot;
    s->configured = x->touch.configured;
    s->portsc = x->touch.portsc;
    pthread_mutex_lock(&x->mu);
    s->fingers_known = !x->touch.mac_unknown; /* restored from one that didn't say, and not lifted yet: still not known */
    s->mac_down = x->touch.mac_down;
    memcpy(s->mac_x, x->touch.mac_x, sizeof s->mac_x);
    memcpy(s->mac_y, x->touch.mac_y, sizeof s->mac_y);
    pthread_mutex_unlock(&x->mu);
}

int hv_xhci_touch_set_snap(HvXhci *x, const HvXhciTouchSnap *s) {
    struct HvXhciTouch *t = &x->touch;
    int ok = 1;
    pthread_mutex_lock(&x->mu);
    if (!s) { /* older than the touchscreen: the Mac's driver knows five ports */
        x->nports = HV_XHCI_PORTS;
        __atomic_store_n(&t->present, 0, __ATOMIC_RELAXED);
        touch_set(x, 0, 0);
        t->portsc = PORT_PP | (uint32_t)PLS_RXDETECT << PORT_PLS_SHIFT;
    } else {
        ok = s->nports >= HV_XHCI_PORTS && s->nports <= HV_XHCI_ROOT_PORTS && (s->present == 0 || s->present == 1) &&
             (!s->present || s->nports >= HV_XHCI_TOUCH_PORT) && s->slot >= 0 && s->slot <= HV_XHCI_SLOTS &&
             (!s->slot || (s->present && x->slot_used[s->slot] && x->slots[s->slot].port == HV_XHCI_TOUCH_PORT)) &&
             (!s->present || (s->w_mm > 0 && s->h_mm > 0));
        if (ok) {
            x->nports = s->nports;
            __atomic_store_n(&t->present, s->present, __ATOMIC_RELAXED);
            t->w_mm = s->w_mm, t->h_mm = s->h_mm;
            touch_set(x, s->slot, s->configured);
            t->portsc = s->portsc;
            if (t->present) t->desc_len = touch_build_desc(t->desc, t->w_mm, t->h_mm);
        }
    }
    /* Every slot on port 6 is the touchscreen's, and there is one only while it is plugged in. */
    for (int i = 1; i <= HV_XHCI_SLOTS && ok; i++)
        if (x->slots[i].port == HV_XHCI_TOUCH_PORT && i != t->slot) ok = 0;
    /* Nobody touches the screen now: whatever was waiting is dropped, and every finger the Mac may have
       seen down is lifted (as hv_xhci_release_keys does for keys). */
    t->qhead = t->qtail;
    t->lost = 0;
    touch_report_empty(t->cur);
    /* What macOS had down when the Mac was saved, where: lifted, so no finger stays down. From a snapshot
       that didn't say, every finger, as cancelled (where they are isn't known). */
    t->mac_down = 0, t->mac_unknown = 0;
    if (ok && s && s->fingers_known) {
        t->mac_down = (uint16_t)(s->mac_down & ((1u << HV_XHCI_CONTACTS) - 1));
        memcpy(t->mac_x, s->mac_x, sizeof t->mac_x);
        memcpy(t->mac_y, s->mac_y, sizeof t->mac_y);
    } else if (ok && s) {
        t->mac_down = (uint16_t)((1u << HV_XHCI_CONTACTS) - 1), t->mac_unknown = 1;
        memset(t->mac_x, 0, sizeof t->mac_x);
        memset(t->mac_y, 0, sizeof t->mac_y);
    }
    if (ok && t->present && t->configured) touch_queue_lift_all(t);
    t->want = -1;
    pthread_mutex_unlock(&x->mu);
    return ok ? 0 : -1;
}

/* ---- the speaker ---- */

void hv_xhci_speaker_get_snap(HvXhci *x, HvXhciSpeakerSnap *s) {
    memset(s, 0, sizeof *s);
    s->slot = x->spk.slot;
    s->portsc = x->spk.portsc;
    HvUsbAudioState st;
    hv_usb_audio_get_state(x->spk.dev, &st);
    s->configured = st.configured, s->alt = st.alt, s->vol = st.vol, s->mute = st.mute;
}

int hv_xhci_speaker_set_snap(HvXhci *x, const HvXhciSpeakerSnap *s) {
    int ok = 1;
    x->spk.out_pending = 0;
    if (!s || !x->spk.dev) { /* older than the speaker: not there for this run */
        x->spk.present = 0;
        x->spk.slot = 0;
        x->spk.portsc = PORT_PP | (uint32_t)PLS_RXDETECT << PORT_PLS_SHIFT;
        if (x->spk.dev) hv_usb_audio_reset(x->spk.dev);
        if (s) ok = 0;
    } else {
        HvUsbAudioState st = {s->configured, s->alt, s->vol, s->mute};
        ok = x->nports >= HV_XHCI_SPEAKER_PORT && s->slot >= 0 && s->slot <= HV_XHCI_SLOTS &&
             (!s->slot || (x->slot_used[s->slot] && x->slots[s->slot].port == HV_XHCI_SPEAKER_PORT)) &&
             (s->slot || (!s->configured && !s->alt)) && hv_usb_audio_set_state(x->spk.dev, &st) == 0;
        if (ok) {
            x->spk.present = 1;
            x->spk.slot = s->slot;
            x->spk.portsc = s->portsc;
        }
    }
    /* Every slot on port 7 is the speaker's. */
    for (int i = 1; i <= HV_XHCI_SLOTS && ok; i++)
        if (x->slots[i].port == HV_XHCI_SPEAKER_PORT && i != x->spk.slot) ok = 0;
    iso_rescan(x);
    x->iso_due = 0;
    x->mf_wraps = hv_counter() / MFINDEX_PERIOD;
    return ok ? 0 : -1;
}

/* ---- the camera ---- */

void hv_xhci_camera_attach(HvXhci *x, struct HvUsbVideo *v) {
    if (!x || !v) return;
    pthread_mutex_lock(&x->mu);
    x->cam.dev = v;
    x->cam.want = 1;
    __atomic_store_n(&x->changes, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&x->mu);
}

void hv_xhci_camera_plug(HvXhci *x, int on) {
    if (!x || !x->cam.dev) return;
    pthread_mutex_lock(&x->mu);
    x->cam.want = on ? 1 : 0;
    __atomic_store_n(&x->changes, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&x->mu);
}

int hv_xhci_camera_state(HvXhci *x) {
    if (!x || !x->cam.dev) return HV_XHCI_TOUCH_ABSENT;
    pthread_mutex_lock(&x->mu);
    int present = __atomic_load_n(&x->cam.present, __ATOMIC_RELAXED), st = HV_XHCI_TOUCH_ABSENT;
    int slot = __atomic_load_n(&x->cam.slot, __ATOMIC_RELAXED);
    if (present && slot && hv_usb_video_configured(x->cam.dev)) st = HV_XHCI_TOUCH_READY;
    else if (present || x->cam.want == 1)
        st = __atomic_load_n(&x->nports, __ATOMIC_RELAXED) < HV_XHCI_CAMERA_PORT ? HV_XHCI_TOUCH_UNAVAILABLE : HV_XHCI_TOUCH_PLUGGED;
    pthread_mutex_unlock(&x->mu);
    return st;
}

void hv_xhci_camera_get_snap(HvXhci *x, HvXhciCamSnap *s) {
    memset(s, 0, sizeof *s);
    s->present = x->cam.present;
    s->slot = x->cam.slot;
    s->portsc = x->cam.portsc;
    if (x->cam.dev) hv_usb_video_get_state(x->cam.dev, &s->dev);
}

int hv_xhci_camera_set_snap(HvXhci *x, const HvXhciCamSnap *s) {
    struct HvXhciCam *c = &x->cam;
    int ok = 1;
    pthread_mutex_lock(&x->mu);
    if (!s || !s->present) {
        __atomic_store_n(&c->present, 0, __ATOMIC_RELAXED);
        c->slot = 0;
        c->portsc = PORT_PP | (uint32_t)PLS_RXDETECT << PORT_PLS_SHIFT;
        if (c->dev) hv_usb_video_reset(c->dev);
        ok = !s || (s->slot == 0);
    } else {
        ok = s->present == 1 && c->dev && x->nports >= HV_XHCI_CAMERA_PORT && s->slot >= 0 && s->slot <= HV_XHCI_SLOTS &&
             (!s->slot || (x->slot_used[s->slot] && x->slots[s->slot].port == HV_XHCI_CAMERA_PORT)) &&
             hv_usb_video_set_state(c->dev, &s->dev) == 0;
        if (ok) {
            __atomic_store_n(&c->present, 1, __ATOMIC_RELAXED);
            c->slot = s->slot;
            c->portsc = s->portsc;
        }
    }
    /* Every slot on port 8 is the camera's, and there is one only while it is plugged in. */
    for (int i = 1; i <= HV_XHCI_SLOTS && ok; i++)
        if (x->slots[i].port == HV_XHCI_CAMERA_PORT && i != c->slot) ok = 0;
    pthread_mutex_unlock(&x->mu);
    return ok ? 0 : -1;
}
