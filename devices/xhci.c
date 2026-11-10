#include "xhci.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "machine.h"

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
    TRB_NORMAL = 1, TRB_SETUP = 2, TRB_DATA = 3, TRB_STATUS = 4, TRB_LINK = 6, TRB_EVENT_DATA = 7, TRB_NOOP = 8,
    TRB_ENABLE_SLOT = 9, TRB_DISABLE_SLOT = 10, TRB_ADDRESS_DEVICE = 11, TRB_CONFIGURE_EP = 12, TRB_EVALUATE_CTX = 13,
    TRB_RESET_EP = 14, TRB_STOP_EP = 15, TRB_SET_TR_DEQUEUE = 16, TRB_RESET_DEVICE = 17, TRB_NOOP_CMD = 23,
    TRB_TRANSFER_EVENT = 32, TRB_CMD_COMPLETION = 33, TRB_PORT_STATUS = 34,
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
#define SPEED_HIGH 2

/* ---- the devices' descriptors ---- */

/* Port 1: a USB HID 1.11 boot keyboard. */

static const uint8_t kbd_report_desc[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
    0x75, 0x08, 0x15, 0x00, 0x26, 0xff, 0x00, 0x05, 0x07, 0x19, 0x00, 0x29, 0xff, 0x81, 0x00, 0xc0,
};

static const uint8_t kbd_device_desc[18] = {
    18, 1, 0x00, 0x02, 0, 0, 0, 64, /* USB 2.0, class per interface, EP0 64 bytes */
    0xac, 0x05, 0x20, 0x02,         /* vendor Apple, product 0x0220 (an ANSI keyboard): macOS knows its layout, no Keyboard Setup Assistant */
    0x00, 0x01, 1, 2, 3, 1,         /* bcdDevice 1.00, strings 1-3, one configuration */
};

static const uint8_t kbd_config_desc[34] = {
    9, 2, 34, 0, 1, 1, 0, 0xa0, 50,           /* one interface, remote wakeup, 100 mA */
    9, 4, 0, 0, 1, 3, 1, 1, 0,                /* interface 0: HID, boot, keyboard */
    9, 0x21, 0x11, 0x01, 0, 1, 0x22, sizeof kbd_report_desc, 0, /* HID 1.11, one report descriptor */
    7, 5, 0x81, 3, 8, 0, 10,                  /* EP1 IN, interrupt, 8 bytes, every 10 ms */
};

static const char *const kbd_strings[] = {NULL, "HengeVM", "HengeVM USB Keyboard", "1"};

/* Port 2: an Apple Magic Trackpad 2 (vendor 0x05AC, product 0x0265). The report
   descriptor is the real device's: a boot-mouse face (report ID 2), a 16-byte
   vendor data channel (report 0x3f) and a 1387-byte vendor blob (report 0x44).
   The multitouch report is NOT declared anywhere: after the guest sends
   feature report {2, 1}, the pad simply starts sending report ID 2 with a
   12-byte prefix and nine bytes a finger, which the guest's driver knows by
   the device ID. (The Linux kernel's hid-magicmouse does the same: it
   registers the MT2's report ID 2 by hand, because the descriptor omits it.) */

static const uint8_t tp_report_desc[] = {
    /* The mouse face: what the pad looks like before the guest switches it to multitouch. */
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01,       /* usage page (generic desktop), usage (mouse), collection (application) */
    0x09, 0x01, 0xa1, 0x00,                  /* usage (pointer), collection (physical) */
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, /* buttons 1-3 */
    0x85, 0x02,                              /* report ID 2 */
    0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f, /* X, Y, -127..127 */
    0x75, 0x08, 0x95, 0x02, 0x81, 0x06,
    0x95, 0x04, 0x75, 0x08, 0x81, 0x01,
    0xc0, 0xc0,
    /* A 16-byte vendor data channel (report 0x3f). */
    0x05, 0x0d, 0x09, 0x05, 0xa1, 0x01,       /* usage page (digitizer), usage (touch pad), collection (application) */
    0x06, 0x00, 0xff, 0x09, 0x0c,             /* vendor page 0xff00, usage 0x0c */
    0x15, 0x00, 0x26, 0xff, 0x00,
    0x75, 0x08, 0x95, 0x10,
    0x85, 0x3f,
    0x81, 0x22,                               /* data, variable, volatile */
    0xc0,
    /* A 1387-byte vendor blob (report 0x44). */
    0x06, 0x00, 0xff, 0x09, 0x0c,             /* vendor page 0xff00, usage 0x0c */
    0xa1, 0x01,                               /* collection (application) */
    0x06, 0x00, 0xff, 0x09, 0x0c,             /* usage 0x0c */
    0x15, 0x00, 0x26, 0xff, 0x00,
    0x85, 0x44,
    0x75, 0x08, 0x96, 0x6b, 0x05,             /* 1387 bytes */
    0x81, 0x00,                               /* constant */
    0xc0,
};

static const uint8_t tp_device_desc[18] = {
    18, 1, 0x00, 0x02, 0, 0, 0, 64, /* USB 2.0, class per interface, EP0 64 bytes */
    0xac, 0x05, 0x65, 0x02,         /* vendor Apple, product 0x0265 (Magic Trackpad 2) */
    0x20, 0x02, 1, 2, 3, 1,         /* bcdDevice 2.20, strings 1-3, one configuration */
};

static const uint8_t tp_config_desc[34] = {
    9, 2, 34, 0, 1, 1, 0, 0xa0, 50,           /* one interface, remote wakeup, 100 mA */
    9, 4, 0, 0, 1, 3, 2, 1, 0,                /* interface 0: HID, boot, mouse */
    9, 0x21, 0x11, 0x01, 0, 1, 0x22, sizeof tp_report_desc, 0, /* HID 1.11, one report descriptor */
    7, 5, 0x81, 3, 64, 0, 4,                  /* EP1 IN, interrupt, 64 bytes, every 4 ms */
};

static const char *const tp_strings[] = {NULL, "Apple Inc.", "Magic Trackpad 2", "HengeVM"};

/* Port 3, with HV_TABLET=1: an absolute pointing device, the kind a virtual machine or a touch
   screen gives its guest: three buttons, X and Y as places on the screen (0..0x7fff), a wheel.
   The report descriptor and IDs are those of QEMU's usb-tablet, which macOS takes as a mouse. */

static const uint8_t tab_report_desc[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, /* generic desktop, mouse; pointer, physical */
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, /* buttons 1-3, 0..1 */
    0x95, 0x03, 0x75, 0x01, 0x81, 0x02,                         /* three bits, variable */
    0x95, 0x01, 0x75, 0x05, 0x81, 0x01,                         /* five bits of padding */
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31,                         /* X, Y */
    0x15, 0x00, 0x26, 0xff, 0x7f, 0x35, 0x00, 0x46, 0xff, 0x7f, /* 0..0x7fff, logical and physical */
    0x75, 0x10, 0x95, 0x02, 0x81, 0x02,                         /* two 16-bit values, absolute */
    0x05, 0x01, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x35, 0x00, 0x45, 0x00, /* wheel, -127..127 */
    0x75, 0x08, 0x95, 0x01, 0x81, 0x06,                         /* one byte, relative */
    0xc0, 0xc0,
};

static const uint8_t tab_device_desc[18] = {
    18, 1, 0x00, 0x02, 0, 0, 0, 64, /* USB 2.0, class per interface, EP0 64 bytes */
    0x27, 0x06, 0x01, 0x00,         /* vendor 0x0627, product 0x0001 (QEMU's tablet) */
    0x00, 0x01, 1, 2, 3, 1,         /* bcdDevice 1.00, strings 1-3, one configuration */
};

static const uint8_t tab_config_desc[34] = {
    9, 2, 34, 0, 1, 1, 0, 0xa0, 50,           /* one interface, remote wakeup, 100 mA */
    9, 4, 0, 0, 1, 3, 0, 0, 0,                /* interface 0: HID, no boot protocol */
    9, 0x21, 0x11, 0x01, 0, 1, 0x22, sizeof tab_report_desc, 0, /* HID 1.11, one report descriptor */
    7, 5, 0x81, 3, 8, 0, 4,                   /* EP1 IN, interrupt, 8 bytes, every 1 ms */
};

static const char *const tab_strings[] = {NULL, "HengeVM", "HengeVM USB Tablet", "1"};

struct HvXhciDev {
    const char *const *strings;
    const uint8_t *device_desc;
    const uint8_t *config_desc;
    const uint8_t *report_desc;
    unsigned report_desc_len;
};
static const struct HvXhciDev devs[HV_XHCI_DEVICES] = {
    {kbd_strings, kbd_device_desc, kbd_config_desc, kbd_report_desc, sizeof kbd_report_desc},
    {tp_strings, tp_device_desc, tp_config_desc, tp_report_desc, sizeof tp_report_desc},
    {tab_strings, tab_device_desc, tab_config_desc, tab_report_desc, sizeof tab_report_desc},
};

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

static void event_to(HvXhci *x, int target, uint64_t param, uint32_t status, uint32_t control) {
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
    it->iman |= 1;        /* IP */
    x->usbsts |= 1u << 3; /* EINT */
    update_irq(x);
}

static void event(HvXhci *x, uint64_t param, uint32_t status, uint32_t control) {
    event_to(x, 0, param, status, control);
}

static void port_event(HvXhci *x, int port) {
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
        uint32_t *ctl = hv_phys_ptr(x->m, r->dequeue + 12, 4);
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

/* ---- endpoint 0: the devices' control requests ---- */

static int tp_build_report(HvXhci *x, uint8_t *out);

/* The Magic Trackpad 2's surface, in 0.01 mm (160.0 x 114.9 mm). The guest's driver
   reads it out of the sensor-surface register to scale touches into millimetres. */
#define HV_TP_PHYS_X 16000
#define HV_TP_PHYS_Y 11490

/* Before the guest's multitouch driver will use the pad it reads a handful of
   sensor-configuration registers: it selects one by writing feature report 1, reads
   report 1 back for a five-byte header giving the register's length, then reads the
   register itself. These are the Magic Trackpad 2's answers, as VoodooInput's
   simulator gives them (the I2C trackpad emulators talk the same protocol). Returns
   the register's bytes, report ID first, or 0 for one we do not know. */
static int tp_register(int rid, uint8_t *out) {
    uint8_t wl = HV_TP_PHYS_X & 0xff, wh = (HV_TP_PHYS_X >> 8) & 0xff;
    uint8_t hl = HV_TP_PHYS_Y & 0xff, hh = (HV_TP_PHYS_Y >> 8) & 0xff;
    static const uint8_t d1[] = {0xd1, 0x81}; /* family ID */
    static const uint8_t d3[] = {0xd3, 0x01, 0x16, 0x1e, 0x03, 0x95, 0x00, 0x14, 0x1e, 0x62, 0x05, 0x00, 0x00}; /* sensor rows/columns */
    static const uint8_t d0[] = {0xd0, 0x02, 0x01, 0x00, 0x14, 0x01, 0x00, 0x1e, 0x00, 0x02, 0x14, 0x02, 0x01, 0x0e, 0x02, 0x00}; /* sensor region */
    static const uint8_t a1[] = {0xa1, 0x00, 0x00, 0x05, 0x00, 0xfc, 0x01}; /* sensor region param */
    static const uint8_t s7f[] = {0x7f, 0x00, 0x00, 0x00, 0x00};
    switch (rid) {
    case 0xd1: memcpy(out, d1, sizeof d1); return (int)sizeof d1;
    case 0xd3: memcpy(out, d3, sizeof d3); return (int)sizeof d3;
    case 0xd0: memcpy(out, d0, sizeof d0); return (int)sizeof d0;
    case 0xa1: memcpy(out, a1, sizeof a1); return (int)sizeof a1;
    case 0x7f: memcpy(out, s7f, sizeof s7f); return (int)sizeof s7f;
    case 0xc8: out[0] = 0xc8; out[1] = 0x08; return 2;
    case 0xd9: { /* sensor surface: width and height, then a fixed calibration tail */
        const uint8_t d9[] = {0xd9, wl, wh, 0, 0, hl, hh, 0, 0, 0x44, 0xe3, 0x52, 0xff, 0xbd, 0x1e, 0xe4, 0x26};
        memcpy(out, d9, sizeof d9); return (int)sizeof d9;
    }
    case 0xdb: { /* the whole sensor description: every register, length-prefixed */
        const uint8_t db[] = {
            0xdb, 0x01, 0x02, 0x00,
            0xd1, 0x81, 0x0d, 0x00,
            0xd3, 0x01, 0x16, 0x1e, 0x03, 0x95, 0x00, 0x14, 0x1e, 0x62, 0x05, 0x00, 0x00, 0x10, 0x00,
            0xd0, 0x02, 0x01, 0x00, 0x14, 0x01, 0x00, 0x1e, 0x00, 0x02, 0x14, 0x02, 0x01, 0x0e, 0x02, 0x00, 0x07, 0x00,
            0xa1, 0x00, 0x00, 0x05, 0x00, 0xfc, 0x01, 0x11, 0x00,
            0xd9, wl, wh, 0, 0, hl, hh, 0, 0, 0x44, 0xe3, 0x52, 0xff, 0xbd, 0x1e, 0xe4, 0x26,
            0x7f, 0x00, 0x00, 0x00, 0x00,
        };
        memcpy(out, db, sizeof db); return (int)sizeof db;
    }
    default: return 0;
    }
}

/* Feature report 1 was written with a register ID: remember the five-byte header the
   guest reads back next, telling it how long that register's answer is. */
static void tp_select(HvXhci *x, int reg) {
    uint8_t scratch[80];
    int n = tp_register(reg, scratch);
    if (n <= 0) { x->tp_f1_len = 0; return; }
    x->tp_f1[0] = 0x01;
    x->tp_f1[1] = (uint8_t)reg;
    x->tp_f1[2] = 0x00;
    x->tp_f1[3] = (uint8_t)((n - 1) & 0xff);
    x->tp_f1[4] = (uint8_t)(((n - 1) >> 8) & 0xff);
    x->tp_f1_len = 5;
}

/* Answer a SETUP on endpoint 0. Returns the reply length (data to send for IN
   requests), or -1 to stall. */
static int control(HvXhci *x, int slot, const uint8_t setup[8], uint8_t *out, int max) {
    const struct HvXhciDev *d = &devs[x->slots[slot].dev];
    uint8_t type = setup[0], req = setup[1];
    uint16_t value = (uint16_t)(setup[2] | setup[3] << 8);
    if (xlog()) fprintf(stderr, "xhci: request %02x %02x value %04x len %u\n", type, req, value, setup[6] | setup[7] << 8);
    /* Every request the trackpad gets goes to the log, unconditionally: the guest's
       driver handshake is exactly what we are trying to watch. Capped so a polling
       driver cannot flood it. */
    if (x->slots[slot].dev == 1 && x->tp_reqs < 80) {
        x->tp_reqs++;
        fprintf(stderr, "hengevm: xhci: tp request %02x %02x value %04x index %04x len %u\n",
                type, req, value, setup[4] | setup[5] << 8, setup[6] | setup[7] << 8);
    }
    const uint8_t *src = NULL;
    int len = 0;
    uint8_t tmp[HV_TP_REPORT_LEN + 8];
    if (type == 0x80 && req == 6) { /* GET_DESCRIPTOR (device) */
        uint8_t dt = value >> 8, idx = value & 0xff;
        if (dt == 1) src = d->device_desc, len = 18;
        else if (dt == 2) src = d->config_desc, len = 34;
        else if (dt == 3) {
            if (idx == 0) {
                tmp[0] = 4, tmp[1] = 3, tmp[2] = 0x09, tmp[3] = 0x04; /* English (US) */
                src = tmp, len = 4;
            } else if (idx < 4) {
                const char *s = d->strings[idx];
                int n = (int)strlen(s);
                tmp[0] = (uint8_t)(2 + 2 * n), tmp[1] = 3;
                for (int i = 0; i < n; i++) tmp[2 + 2 * i] = (uint8_t)s[i], tmp[3 + 2 * i] = 0;
                src = tmp, len = 2 + 2 * n;
            } else
                return -1;
        } else
            return -1; /* no device qualifier: full speed only */
    } else if (type == 0x81 && req == 6) { /* GET_DESCRIPTOR (interface): HID, report */
        if ((value >> 8) == 0x22) src = d->report_desc, len = (int)d->report_desc_len;
        else if ((value >> 8) == 0x21) src = d->config_desc + 18, len = 9;
        else return -1;
    } else if (type == 0x80 && req == 0) { /* GET_STATUS */
        tmp[0] = 0, tmp[1] = 0;
        src = tmp, len = 2;
    } else if ((type == 0x81 || type == 0x82) && req == 0) {
        tmp[0] = 0, tmp[1] = 0;
        src = tmp, len = 2;
    } else if (type == 0x80 && req == 8) { /* GET_CONFIGURATION */
        tmp[0] = (uint8_t)x->slots[slot].configured;
        src = tmp, len = 1;
    } else if (type == 0x00 && req == 9) { /* SET_CONFIGURATION */
        x->slots[slot].configured = value & 0xff;
        return 0;
    } else if (type == 0x81 && req == 10) { /* GET_INTERFACE */
        tmp[0] = 0;
        src = tmp, len = 1;
    } else if (type == 0xa1 && req == 1) { /* GET_REPORT: wValue is (type << 8) | report ID */
        if (x->slots[slot].dev == 0) src = x->state, len = 8;
        else if (x->slots[slot].dev == 2) src = x->tab_state, len = 6;
        else {
            int rtype = value >> 8, rid = value & 0xff;
            if (rtype == 3) { /* feature: the sensor-configuration registers */
                if (rid == 0x01) { /* the header for the register just selected */
                    if (!x->tp_f1_len) return -1;
                    src = x->tp_f1;
                    len = x->tp_f1_len;
                } else if (rid == 0x00) {
                    tmp[0] = 0x00; tmp[1] = 0x01;
                    src = tmp; len = 2;
                } else if (rid == 0x02) { /* multitouch mode: on */
                    tmp[0] = 0x02; tmp[1] = 0x01;
                    src = tmp; len = 2;
                } else {
                    len = tp_register(rid, tmp);
                    if (!len) return -1;
                    src = tmp;
                }
            } else if (rid == 0x02) { /* input: the current touch state */
                pthread_mutex_lock(&x->mu);
                len = tp_build_report(x, tmp);
                pthread_mutex_unlock(&x->mu);
                src = tmp;
            } else if (rid == 0x3f) { /* the vendor data channel: nothing on it */
                memset(tmp, 0, 16);
                len = 16;
            } else { /* 0x44 is a constant report: zeros, as far as we are concerned */
                memset(out, 0, (size_t)max);
                return max;
            }
        }
    } else if (type == 0xa1 && (req == 2 || req == 3)) { /* GET_IDLE, GET_PROTOCOL */
        tmp[0] = req == 3 ? 1 : 0;
        src = tmp, len = 1;
    } else if ((type & 0x60) == 0x20 || (type == 0x00 && (req == 1 || req == 3)) || (type == 0x01 && req == 11) ||
               (type == 0x02 && (req == 1 || req == 3)) ||
               ((type & 0x60) == 0x40 && x->slots[slot].dev == 1)) {
        /* SET_IDLE, SET_PROTOCOL, SET_REPORT (the trackpad's multitouch switch and friends): accepted.
           Vendor requests to the trackpad too: stalling one may be what stops the guest's
           driver handshake cold, and we cannot tell what it wants yet. */
        return 0;
    } else {
        if (x->m->log) fprintf(stderr, "hengevm: xhci: unhandled request %02x %02x value %04x (slot %d, device %d)\n",
                               type, req, value, slot, x->slots[slot].dev);
        return -1;
    }
    if (len > max) len = max;
    memcpy(out, src, (size_t)len);
    return len;
}

/* Run the control transfers queued on the device's endpoint 0. */
static void run_ep0(HvXhci *x, int slot) {
    HvXhciRing *r = &x->slots[slot].ep[1];
    uint32_t trb[4];
    uint64_t at;
    uint8_t setup[8], reply[256];
    int reply_len = 0, stalled = 0;
    while (ring_peek(x, r, trb, &at)) {
        int type = (trb[3] >> 10) & 0x3f;
        int ioc = (trb[3] >> 5) & 1;
        if (type == TRB_SETUP) {
            memcpy(setup, trb, 8);
            uint16_t wlen = (uint16_t)(setup[6] | setup[7] << 8);
            reply_len = control(x, slot, setup, reply, wlen < sizeof reply ? wlen : (int)sizeof reply);
            stalled = reply_len < 0;
            ring_advance(r);
            if (stalled) {
                transfer_event(x, at, CC_STALL, 0, slot, 1);
                set_ep_state(x, slot, 1, 2); /* halted */
                x->slots[slot].ep_running[1] = 0;
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
                if (x->slots[slot].dev == 1 && want >= 2) {
                    uint8_t data[2];
                    rd(x, buf, data, 2); /* the data lives in guest memory, at the TRB's pointer */
                    if (data[0] == 0x02 && (data[1] & 1)) {
                        x->tp_mt = 1; /* the guest switched the pad to multitouch */
                        fprintf(stderr, "hengevm: xhci: trackpad: multitouch on\n");
                    } else if (data[0] == 0x01) {
                        tp_select(x, data[1]); /* the guest selected a sensor register to read */
                        fprintf(stderr, "hengevm: xhci: trackpad: select register %02x (%d bytes)\n", data[1], x->tp_f1_len ? x->tp_f1[3] : 0);
                    } else {
                        fprintf(stderr, "hengevm: xhci: trackpad: set report %02x %02x\n", data[0], data[1]);
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

/* Give the device's waiting reports to the interrupt IN endpoint's posted transfers. */
static void run_ep_in(HvXhci *x, int slot) {
    struct HvXhciSlot *s = &x->slots[slot];
    int dci = -1;
    for (int d = 2; d < HV_XHCI_EPS; d++) if (s->ep_running[d]) { dci = d; break; }
    if (s->dev < 0 || dci < 0) return;
    HvXhciRing *r = &s->ep[dci];
    uint32_t trb[4];
    uint64_t at;
    pthread_mutex_lock(&x->mu);
    if (s->dev == 0) {
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
            transfer_event(x, at, n < want ? CC_SHORT_PACKET : CC_SUCCESS, want - n, slot, dci);
        }
    } else if (s->dev == 2) {
        while (x->tab_rhead != x->tab_rtail && ring_peek(x, r, trb, &at)) {
            int type = (trb[3] >> 10) & 0x3f;
            ring_advance(r);
            if (type != TRB_NORMAL) continue;
            uint64_t buf = (uint64_t)trb[1] << 32 | trb[0];
            uint32_t want = trb[2] & 0x1ffff;
            uint32_t n = want < 6 ? want : 6;
            wr(x, buf, x->tab_reports[x->tab_rhead % HV_TAB_REPORTS], n);
            x->tab_rhead++;
            x->tab_delivered++;
            if (x->tab_delivered <= 4) fprintf(stderr, "hengevm: xhci: tablet: %u bytes to the guest (delivered %llu)\n", n, (unsigned long long)x->tab_delivered);
            transfer_event(x, at, n < want ? CC_SHORT_PACKET : CC_SUCCESS, want - n, slot, dci);
        }
    } else {
        while (x->tp_rhead != x->tp_rtail && ring_peek(x, r, trb, &at)) {
            int type = (trb[3] >> 10) & 0x3f;
            ring_advance(r);
            if (type != TRB_NORMAL) continue;
            uint64_t buf = (uint64_t)trb[1] << 32 | trb[0];
            uint32_t want = trb[2] & 0x1ffff;
            uint32_t len = x->tp_rlen[x->tp_rhead % HV_TP_REPORTS];
            uint32_t n = want < len ? want : len;
            const uint8_t *rep = x->tp_reports[x->tp_rhead % HV_TP_REPORTS];
            wr(x, buf, rep, n);
            x->tp_rhead++;
            x->tp_delivered++;
            if (x->tp_delivered <= 16) fprintf(stderr, "hengevm: xhci: trackpad: %u bytes to the guest (delivered %llu)\n", n, (unsigned long long)x->tp_delivered);
            if (len >= 30 && x->tp_multi_logs < 40) { /* two fingers or more: dump the frame so we can watch them move */
                x->tp_multi_logs++;
                int nf = (int)((len - 12) / 9);
                fprintf(stderr, "hengevm: xhci: trackpad: %d fingers:", nf);
                for (int i = 0; i < nf; i++) {
                    const uint8_t *b = rep + 12 + 9 * i;
                    int xc = b[0] | ((b[1] & 0x1f) << 8); if (xc & 0x1000) xc -= 0x2000;
                    int yc = ((b[1] >> 5) & 0x7) | (b[2] << 3) | ((b[3] & 0x3) << 11); if (yc & 0x1000) yc -= 0x2000;
                    fprintf(stderr, " [id%d x%d y%d s%d]", b[8] & 0xf, xc, yc, (b[3] >> 5) & 7);
                }
                fprintf(stderr, "\n");
            }
            transfer_event(x, at, n < want ? CC_SHORT_PACKET : CC_SUCCESS, want - n, slot, dci);
        }
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
    int dev = port - 1;
    if (dev < 0 || dev >= HV_XHCI_DEVICES) return CC_TRB_ERROR;
    x->slots[slot].dev = dev;
    uint32_t dw3 = (uint32_t)(bsr ? 1 : 2) << 27 | (uint32_t)(bsr ? 0 : slot); /* default or addressed; address = slot */
    memcpy(slot_ctx + 12, &dw3, 4);
    uint32_t e0 = 0;
    memcpy(&e0, ep0, 4);
    e0 = (e0 & ~7u) | 1; /* running */
    memcpy(ep0, &e0, 4);
    wr(x, out, slot_ctx, 32);
    wr(x, out + 32, ep0, 32);
    init_ring_from_ctx(x, slot, 1);
    return CC_SUCCESS;
}

static int configure_endpoints(HvXhci *x, int slot, uint64_t input, int deconfigure) {
    struct HvXhciSlot *s = &x->slots[slot];
    uint64_t out = out_ctx(x, slot);
    if (deconfigure) {
        for (int dci = 2; dci < HV_XHCI_EPS; dci++) s->ep_running[dci] = 0;
        return CC_SUCCESS;
    }
    uint32_t drop = rd32(x, input), add = rd32(x, input + 4);
    for (int dci = 2; dci < HV_XHCI_EPS; dci++) {
        if (drop & (1u << dci)) {
            s->ep_running[dci] = 0;
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
        /* The slot ID is the guest's to write (8 bits): every command but these two indexes x->slots with it. */
        if (type != TRB_ENABLE_SLOT && type != TRB_NOOP_CMD && (slot < 1 || slot > HV_XHCI_SLOTS)) {
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
            cmd_completion(x, at, CC_SUCCESS, s);
            continue;
        }
        case TRB_DISABLE_SLOT:
            if (slot >= 1 && slot <= HV_XHCI_SLOTS) {
                x->slot_used[slot] = 0;
                x->slots[slot].dev = -1;
                memset(x->slots[slot].ep_running, 0, sizeof x->slots[slot].ep_running);
            }
            break;
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
            x->slots[slot].ep_running[dci] = 0;
            set_ep_state(x, slot, dci, 3);
            /* The dequeue pointer goes back into the context for the driver to read. */
            uint64_t deq = x->slots[slot].ep[dci].dequeue | (uint64_t)x->slots[slot].ep[dci].ccs;
            wr(x, out_ctx(x, slot) + 32ull * dci + 8, &deq, 8);
            break;
        }
        case TRB_SET_TR_DEQUEUE: {
            int dci = (trb[3] >> 16) & 0x1f;
            uint64_t p = (uint64_t)trb[1] << 32 | trb[0];
            x->slots[slot].ep[dci].dequeue = p & ~0xfull;
            x->slots[slot].ep[dci].ccs = (int)(p & 1);
            wr(x, out_ctx(x, slot) + 32ull * dci + 8, &p, 8);
            break;
        }
        case TRB_RESET_DEVICE: {
            for (int dci = 2; dci < HV_XHCI_EPS; dci++) x->slots[slot].ep_running[dci] = 0;
            uint32_t dw3 = (uint32_t)1 << 27; /* default state, address 0 */
            wr(x, out_ctx(x, slot) + 12, &dw3, 4);
            x->slots[slot].configured = 0;
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

/* Port 1 is the full-speed keyboard; port 2 is the high-speed trackpad, as the real
   Magic Trackpad 2 is. */
static void port_connect(HvXhci *x) {
    for (int p = 0; p < x->nports; p++) {
        x->portsc[p] = PORT_PP | PORT_CCS | PORT_CSC | (uint32_t)(p ? SPEED_HIGH : SPEED_FULL) << PORT_SPEED_SHIFT | 7u << PORT_PLS_SHIFT;
        port_event(x, p);
    }
}

static void port_write(HvXhci *x, int p, uint32_t v) {
    if (xlog()) fprintf(stderr, "xhci: portsc write %08x (was %08x)\n", v, x->portsc[p]);
    uint32_t s = x->portsc[p];
    s &= ~(v & PORT_CHANGES); /* write 1 to clear */
    if (v & PORT_PED) s &= ~PORT_PED;
    if (v & PORT_PR) { /* reset: enabled, in U0 */
        s = (s & ~(0xfu << PORT_PLS_SHIFT)) | PORT_PED | PORT_PRC;
        x->portsc[p] = s;
        port_event(x, p);
        return;
    }
    if ((v & PORT_LWS) && (s & PORT_CCS)) {
        uint32_t pls = (v >> PORT_PLS_SHIFT) & 0xf;
        s = (s & ~(0xfu << PORT_PLS_SHIFT)) | pls << PORT_PLS_SHIFT;
        if (pls == 0 || pls == 15) { /* U0 or resume: report the link change */
            s = (s & ~(0xfu << PORT_PLS_SHIFT)) | PORT_PLC;
            x->portsc[p] = s;
            port_event(x, p);
            return;
        }
    }
    s = (s & ~PORT_PP) | (v & PORT_PP);
    x->portsc[p] = s;
}

/* ---- registers ---- */

static void reset(HvXhci *x) {
    x->usbcmd = 0;
    x->usbsts = 1; /* halted */
    x->dnctrl = x->config = 0;
    x->dcbaap = 0;
    memset(&x->cmd, 0, sizeof x->cmd);
    memset(x->intr, 0, sizeof x->intr);
    for (int i = 0; i < HV_XHCI_INTRS; i++) x->intr[i].pcs = 1;
    memset(x->slot_used, 0, sizeof x->slot_used);
    memset(x->slots, 0, sizeof x->slots);
    for (int s = 1; s <= HV_XHCI_SLOTS; s++) x->slots[s].dev = -1;
    memset(x->reports, 0, sizeof x->reports);
    x->rhead = x->rtail = 0;
    memset(x->state, 0, sizeof x->state);
    memset(x->tp_fingers, 0, sizeof x->tp_fingers);
    x->tp_button = 0;
    x->tp_ts = 0;
    x->tp_rhead = x->tp_rtail = 0;
    x->tp_mt = 0;
    x->tp_delivered = 0;
    x->tp_reqs = 0;
    x->tp_db = 0;
    x->tp_multi_logs = 0;
    x->tp_f1_len = 0;
    x->tab_rhead = x->tab_rtail = 0;
    memset(x->tab_state, 0, sizeof x->tab_state);
    x->tab_delivered = 0;
    for (int p = 0; p < x->nports; p++)
        x->portsc[p] = PORT_PP | PORT_CCS | PORT_CSC | (uint32_t)(p ? SPEED_HIGH : SPEED_FULL) << PORT_SPEED_SHIFT | 7u << PORT_PLS_SHIFT;
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
            1u << 4,                                             /* ERST max 2 segments */
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
        static const uint32_t ext[4] = {2u | 0x0200u << 16, 0x20425355, 1u | ((1u << HV_XHCI_PORTS) - 1u) << 8, 0};
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
    } else if (off >= PORTREGS && off < PORTREGS + 16u * (uint32_t)x->nports) {
        int p = (int)(off - PORTREGS) / 16;
        v = (off - PORTREGS) % 16 == 0 ? x->portsc[p] : 0;
    } else if (off >= RT && off < RT + 0x20 + 32u * HV_XHCI_INTRS) {
        if (off - RT < 0x20) return 0; /* MFINDEX */
        struct HvXhciIntr *it = &x->intr[(off - RT - 0x20) / 32];
        switch ((off - RT - 0x20) % 32) {
        case 0x00: v = it->iman; break;
        case 0x04: v = it->imod; break;
        case 0x08: v = it->erstsz; break;
        case 0x10: v = size == 8 ? it->erstba : (uint32_t)it->erstba; break;
        case 0x14: v = it->erstba >> 32; break;
        case 0x18: v = it->erdp; break;
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
                port_connect(x);
                x->usbsts |= 1;
                return;
            }
            if ((v & 1) && !(x->usbcmd & 1)) { /* run */
                x->usbcmd = (uint32_t)v;
                x->usbsts &= ~1u;
                for (int p = 0; p < x->nports; p++)
                    if (x->portsc[p] & PORT_CSC) port_event(x, p);
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
    } else if (off >= PORTREGS && off < PORTREGS + 16u * (uint32_t)x->nports) {
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
        } else if (slot <= HV_XHCI_SLOTS && x->slots[slot].dev >= 0) {
            int dci = (int)(v & 0xff);
            if (dci < 1 || dci >= HV_XHCI_EPS) return;
            if (x->slots[slot].dev == 1 && x->tp_db < 8) {
                x->tp_db++;
                fprintf(stderr, "hengevm: xhci: trackpad: doorbell dci %d\n", dci);
            }
            /* A doorbell starts a stopped endpoint again (a halted one waits for Reset Endpoint). */
            uint32_t state = rd32(x, out_ctx(x, slot) + 32ull * dci) & 7;
            if (state == 2) return;
            if (state != 1) set_ep_state(x, slot, dci, 1);
            x->slots[slot].ep_running[dci] = 1;
            if (dci == 1) run_ep0(x, slot);
            else run_ep_in(x, slot);
        }
    }
}

/* ---- the keyboard's reports ---- */

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

/* ---- the Magic Trackpad 2's reports ---- */

/* Build the current report into out: a 12-byte prefix (report ID 2, the press,
   flags, the timestamp) plus nine bytes a finger. The finger's x and y are 13
   bits, centred on the sensor, with y inverted; byte 3 then holds the finger
   type and the 3-bit state (0 none, 3 near, 4 contact, 7 stop), then the touch
   major and minor axes, size and pressure, and finally the touch ID and angle. */
static int tp_build_report(HvXhci *x, uint8_t *out) {
    int n = 0;
    memset(out, 0, (size_t)(12 + 9 * HV_TP_FINGERS));
    out[0] = 0x02;
    out[1] = (uint8_t)(x->tp_button & 1);
    out[8] = 0x31;
    for (int i = 0; i < HV_TP_FINGERS; i++) {
        const struct HvTpFinger *f = &x->tp_fingers[i];
        if (!f->in) continue;
        int16_t X = (int16_t)(f->x - HV_TP_SENSOR_X / 2);
        int16_t Y = (int16_t)(-(f->y - HV_TP_SENSOR_Y / 2));
        uint8_t off = f->state == 0 || f->state == 7; /* none and stop carry no touch data */
        uint8_t b[9];
        b[0] = (uint8_t)(X & 0xff);
        b[1] = (uint8_t)(((X >> 8) & 0x1f) | ((Y & 0x7) << 5)); /* x's high five bits, then y's low three */
        b[2] = (uint8_t)((Y >> 3) & 0xff);
        b[3] = (uint8_t)(((Y >> 11) & 0x3) | (off ? 0 : (2u << 2)) | (f->state << 5));
        b[4] = off ? 0 : 20;   /* touch major */
        b[5] = off ? 0 : 20;   /* touch minor */
        b[6] = off ? 0 : 10;   /* size */
        b[7] = off ? 0 : (uint8_t)(x->tp_button ? 120 : 5); /* pressure */
        b[8] = (uint8_t)((f->id & 0xf) | (4u << 5)); /* angle: straight */
        memcpy(out + 12 + 9 * n, b, 9);
        n++;
    }
    if (n || x->tp_button) out[7] = 0x03; else out[7] = 0x02; /* touch active */
    out[9] = (uint8_t)((x->tp_ts << 3) | 4);
    out[10] = (uint8_t)((x->tp_ts >> 5) & 0xff);
    out[11] = (uint8_t)((x->tp_ts >> 13) & 0xff);
    return 12 + 9 * n;
}

/* The report timestamp is milliseconds, as the real pad sends it: the guest divides
   position deltas by timestamp deltas to get finger speed. It must be the touch sample's
   own clock, not the moment we process it -- if several samples are handled in one burst,
   wall-clock stamps them all ~1 ms apart and the guest reads a flick as enormous speed,
   which is what sends scroll inertia flying on release. tp_now_ms is only the fallback
   for the physical button, which has no sample clock. */
static uint32_t tp_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000);
}

/* Queue the current state as a report, stamped with the touch sample's clock (ms);
   ts_ms 0 falls back to the current time (the physical button). */
static void tp_queue(HvXhci *x, uint32_t ts_ms) {
    uint8_t r[HV_TP_REPORT_LEN];
    /* Strictly increasing: two frames with the same timestamp would give the guest a
       zero time delta (speed = distance / 0) and it can throw the frame away. */
    uint32_t now = ts_ms ? ts_ms : tp_now_ms();
    x->tp_ts = now > x->tp_ts ? now : x->tp_ts + 1;
    int len = tp_build_report(x, r);
    if (x->tp_rtail - x->tp_rhead < HV_TP_REPORTS) {
        unsigned at = x->tp_rtail % HV_TP_REPORTS;
        memcpy(x->tp_reports[at], r, (size_t)len);
        x->tp_rlen[at] = (uint8_t)len;
        x->tp_rtail++;
    }
}

static struct HvTpFinger *tp_new_finger(HvXhci *x) {
    for (int i = 0; i < HV_TP_FINGERS; i++) if (!x->tp_fingers[i].in) return &x->tp_fingers[i];
    return NULL;
}

void hv_xhci_touch(HvXhci *x, int id, int phase, int px, int py, uint32_t ts_ms) {
    if (!x || id < 1 || id > 15) return;
    pthread_mutex_lock(&x->mu);
    if (phase == 0) {
        struct HvTpFinger *f = tp_new_finger(x);
        if (f) {
            f->in = 1;
            f->id = id;
            f->x = px;
            f->y = py;
            f->state = 4; /* contact: a touch, not a hover, so the guest tracks it at once */
            tp_queue(x, ts_ms);
        }
    } else {
        for (int i = 0; i < HV_TP_FINGERS; i++) {
            struct HvTpFinger *f = &x->tp_fingers[i];
            if (!f->in || f->id != id) continue;
            if (phase == 1) {
                f->x = px;
                f->y = py;
                if (f->state == 3) f->state = 4;
                /* no report yet: the caller commits one frame holding every finger that
                   moved, so the guest sees them move together instead of one at a time */
            } else {
                f->state = 7; /* stop */
                tp_queue(x, ts_ms);
                f->in = 0;
                f->state = 0; /* none */
                tp_queue(x, ts_ms);
            }
            break;
        }
    }
    pthread_mutex_unlock(&x->mu);
    hv_machine_wake(x->m);
}

/* Commit one frame with the current state of all the fingers: a move batch ends
   here, so a gesture's fingers all land in the same report. */
void hv_xhci_touch_frame(HvXhci *x, uint32_t ts_ms) {
    if (!x) return;
    pthread_mutex_lock(&x->mu);
    tp_queue(x, ts_ms);
    pthread_mutex_unlock(&x->mu);
    hv_machine_wake(x->m);
}

void hv_xhci_touch_button(HvXhci *x, int down) {
    if (!x) return;
    pthread_mutex_lock(&x->mu);
    if (x->tp_button != (down & 1)) {
        x->tp_button = down & 1;
        tp_queue(x, 0);
    }
    pthread_mutex_unlock(&x->mu);
    hv_machine_wake(x->m);
}

int hv_xhci_has_tablet(HvXhci *x) {
    return x && x->nports > HV_XHCI_PORTS;
}

void hv_xhci_tablet(HvXhci *x, int px, int py, int buttons) {
    if (!hv_xhci_has_tablet(x)) return;
    if (px < 0) px = 0;
    if (px > HV_TAB_MAX) px = HV_TAB_MAX;
    if (py < 0) py = 0;
    if (py > HV_TAB_MAX) py = HV_TAB_MAX;
    uint8_t rep[6] = {(uint8_t)(buttons & 7), (uint8_t)px, (uint8_t)(px >> 8), (uint8_t)py, (uint8_t)(py >> 8), 0};
    pthread_mutex_lock(&x->mu);
    if (!memcmp(x->tab_state, rep, 6) && x->tab_delivered) { /* nothing new */
        pthread_mutex_unlock(&x->mu);
        return;
    }
    memcpy(x->tab_state, rep, 6);
    /* Only the newest place matters: a report still waiting with the same buttons is replaced, so a busy
       guest gets where the pointer is now, not a queue of where it has been. Button changes all go through. */
    uint32_t last = x->tab_rtail - 1;
    if (x->tab_rhead != x->tab_rtail && x->tab_reports[last % HV_TAB_REPORTS][0] == rep[0]) {
        memcpy(x->tab_reports[last % HV_TAB_REPORTS], rep, 6);
    } else {
        if (x->tab_rtail - x->tab_rhead == HV_TAB_REPORTS) x->tab_rhead++; /* full: the oldest goes */
        memcpy(x->tab_reports[x->tab_rtail % HV_TAB_REPORTS], rep, 6);
        x->tab_rtail++;
    }
    pthread_mutex_unlock(&x->mu);
    hv_machine_wake(x->m);
}

void hv_xhci_poll(HvXhci *x) {
    if (!x) return;
    for (int slot = 1; slot <= HV_XHCI_SLOTS; slot++)
        if (x->slots[slot].dev >= 0) run_ep_in(x, slot);
}

HvXhci *hv_xhci_create(HvMachine *m, HvPcie *bus) {
    HvXhci *x = calloc(1, sizeof *x);
    x->m = m;
    pthread_mutex_init(&x->mu, NULL);
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
    /* HV_TABLET=1: an absolute pointing device on a third port (Settings > Instant pointer). */
    const char *tablet = getenv("HV_TABLET");
    x->nports = tablet && *tablet == '1' ? HV_XHCI_PORTS + 1 : HV_XHCI_PORTS;
    if (x->nports > HV_XHCI_PORTS) fprintf(stderr, "hengevm: xhci: tablet on port %d (HV_TABLET)\n", x->nports);
    reset(x);
    return x;
}
