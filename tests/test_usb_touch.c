/* The USB touchscreen on the xHCI's port 6, driven the way macOS's xHCI and HID drivers drive it: absent
   until the host plugs it in, enumerated full speed, its descriptors (a HID multi-touch touchscreen in
   parallel mode), reports for each frame of contacts on its interrupt endpoint, moves merged while the
   guest catches up, every finger lifted when frames were lost, nothing stale after it starts again,
   unplugged again, its snapshot state, and a Mac resumed from an older snapshot (which knows five ports)
   never seeing it, even when it resets the controller. The keyboard and drives are as before. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "machine.h"
#include "pcie.h"

#include "xhci.h"

static int checks, failures;
static void check(const char *what, long long got, long long want) {
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL %s: got %lld (%#llx), want %lld (%#llx)\n", what, got, (unsigned long long)got, want, (unsigned long long)want);
    }
}

#define RAM_PA 0x40000000ull
#define RAM_SIZE (32u << 20)
static uint8_t *ram;
static HvMachine m;
static HvXhci *x;

static void *at(uint64_t pa) { return ram + (pa - RAM_PA); }
static void w32(uint64_t pa, uint32_t v) { memcpy(at(pa), &v, 4); }
static uint32_t r32(uint64_t pa) {
    uint32_t v;
    memcpy(&v, at(pa), 4);
    return v;
}
static void w64(uint64_t pa, uint64_t v) { memcpy(at(pa), &v, 8); }

static void reg_w(uint64_t off, uint32_t v) { x->pci.bar_write(&x->pci, 0, off, v, 4); }
static uint32_t reg_r(uint64_t off) { return (uint32_t)x->pci.bar_read(&x->pci, 0, off, 4); }

#define OP 0x40
#define PORTSC(p) (0x440 + 16 * ((p) - 1))
#define RT 0x1000
#define DB 0x2000

/* A producer ring of 64 TRBs with a link back at the end. */
typedef struct {
    uint64_t base;
    int idx, cycle;
} Ring;

static void ring_init(Ring *r, uint64_t base) {
    memset(at(base), 0, 64 * 16);
    r->base = base, r->idx = 0, r->cycle = 1;
}

static uint64_t ring_put(Ring *r, uint64_t param, uint32_t status, uint32_t ctl) {
    if (r->idx == 63) { /* link, toggling the cycle */
        uint64_t l = r->base + 63 * 16;
        w64(l, r->base);
        w32(l + 8, 0);
        w32(l + 12, 6u << 10 | 2u | (uint32_t)r->cycle);
        r->idx = 0, r->cycle ^= 1;
    }
    uint64_t t = r->base + 16ull * (uint64_t)r->idx++;
    w64(t, param);
    w32(t + 8, status);
    w32(t + 12, ctl | (uint32_t)r->cycle);
    return t;
}

/* The event ring. */
#define ER 0x40003000ull
static int er_idx, er_cycle = 1;
typedef struct {
    uint64_t param;
    uint32_t status, ctl;
} Ev;

static int next_event(Ev *e) {
    for (int tries = 0; tries < 2000; tries++) {
        uint64_t t = ER + 16ull * (uint64_t)er_idx;
        uint32_t ctl = r32(t + 12);
        if ((ctl & 1) == (uint32_t)er_cycle) {
            memcpy(&e->param, at(t), 8);
            e->status = r32(t + 8);
            e->ctl = ctl;
            if (++er_idx == 256) er_idx = 0, er_cycle ^= 1;
            reg_w(RT + 0x20 + 0x18, (uint32_t)(ER + 16ull * (uint64_t)er_idx) | 8); /* ERDP, EHB */
            reg_w(RT + 0x20 + 0x1c, (uint32_t)((ER + 16ull * (uint64_t)er_idx) >> 32));
            return 1;
        }
        hv_xhci_poll(x);
        usleep(500);
    }
    return 0;
}

static int no_event(void) {
    uint32_t ctl = r32(ER + 16ull * (uint64_t)er_idx + 12);
    return (ctl & 1) != (uint32_t)er_cycle;
}

static int ev_type(const Ev *e) { return (e->ctl >> 10) & 0x3f; }
static int ev_cc(const Ev *e) { return e->status >> 24; }

/* ---- commands ---- */

static Ring cmd;
static Ev command(uint64_t param, uint32_t ctl) {
    uint64_t t = ring_put(&cmd, param, 0, ctl);
    reg_w(DB, 0);
    Ev e = {0};
    if (!next_event(&e)) printf("FAIL no completion for command at %#llx\n", (unsigned long long)t), failures++;
    check("command completion event", ev_type(&e), 33);
    check("completion is for that command", (long long)e.param, (long long)t);
    return e;
}

#define DCBAA 0x40001000ull
#define INPUT 0x40020000ull
static uint64_t out_ctx(int slot) { return 0x40010000ull + 0x1000ull * (uint64_t)slot; }
static uint64_t ring_base(int slot, int dci) { return 0x40100000ull + 0x10000ull * (uint64_t)slot + 0x1000ull * (uint64_t)dci; }

typedef struct {
    int slot;
    Ring ep[32];
} Dev;

static int enumerate(Dev *d, int port, int speed) {
    Ev e = command(0, 9u << 10); /* Enable Slot */
    check("enable slot", ev_cc(&e), 1);
    d->slot = (int)(e.ctl >> 24);
    w64(DCBAA + 8ull * (uint64_t)d->slot, out_ctx(d->slot));
    memset(at(out_ctx(d->slot)), 0, 1024);
    memset(at(INPUT), 0, 33 * 32);
    w32(INPUT + 4, 3);                                        /* add slot, EP0 */
    w32(INPUT + 32, 1u << 27 | (uint32_t)speed << 20);        /* one context entry */
    w32(INPUT + 32 + 4, (uint32_t)port << 16);                /* root hub port */
    ring_init(&d->ep[1], ring_base(d->slot, 1));
    w32(INPUT + 64 + 4, 4u << 3 | 3u << 1 | 64u << 16);      /* control, CErr 3, 64 bytes */
    w64(INPUT + 64 + 8, d->ep[1].base | 1);
    e = command(INPUT, 11u << 10 | (uint32_t)d->slot << 24);  /* Address Device */
    return ev_cc(&e);
}

/* A control transfer on EP0: returns the bytes the IN data stage got (or -1 on a stall). */
static int control(Dev *d, uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint16_t len, void *buf) {
    uint64_t data = 0x41000000ull;
    uint8_t setup[8] = {type, req, (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)index, (uint8_t)(index >> 8), (uint8_t)len, (uint8_t)(len >> 8)};
    uint64_t sp;
    memcpy(&sp, setup, 8);
    int in = type & 0x80;
    ring_put(&d->ep[1], sp, 8, 2u << 10 | 1u << 6 | (uint32_t)(len ? (in ? 3 : 2) : 0) << 16);
    uint64_t dt = 0;
    if (len) {
        memset(at(data), 0xee, len);
        if (!in && buf) memcpy(at(data), buf, len);
        dt = ring_put(&d->ep[1], data, len, 3u << 10 | 1u << 2 | (uint32_t)(in ? 1 : 0) << 16);
    }
    uint64_t st = ring_put(&d->ep[1], 0, 0, 4u << 10 | 1u << 5 | (uint32_t)(in && len ? 0 : 1) << 16);
    reg_w(DB + 4u * (uint32_t)d->slot, 1);
    Ev e;
    int got = len;
    for (;;) {
        if (!next_event(&e)) return -2;
        if (ev_type(&e) != 32) continue;
        if (ev_cc(&e) == 6) return -1; /* stall */
        if (e.param == dt && ev_cc(&e) == 13) got = len - (int)(e.status & 0xffffff);
        if (e.param == st) break;
    }
    if (in && buf && got > 0) memcpy(buf, at(data), (size_t)got);
    return got;
}

static int no_event_soon(void) {
    for (int i = 0; i < 20; i++) {
        hv_xhci_poll(x);
        if (!no_event()) return 0;
        usleep(500);
    }
    return 1;
}

static void start_controller(void) {
    reg_w(OP + 0, 2); /* HCRST */
    w64(DCBAA, 0);
    reg_w(OP + 0x30, (uint32_t)DCBAA);
    reg_w(OP + 0x34, 0);
    ring_init(&cmd, 0x40002000ull);
    reg_w(OP + 0x18, (uint32_t)cmd.base | 1);
    reg_w(OP + 0x1c, 0);
    w64(0x40004000ull, ER);
    w32(0x40004008ull, 256);
    memset(at(ER), 0, 256 * 16);
    er_idx = 0, er_cycle = 1;
    reg_w(RT + 0x20 + 0x08, 1);  /* ERSTSZ */
    reg_w(RT + 0x20 + 0x18, (uint32_t)ER);
    reg_w(RT + 0x20 + 0x1c, 0);
    reg_w(RT + 0x20 + 0x10, 0x40004000u);
    reg_w(RT + 0x20 + 0x14, 0);
    reg_w(RT + 0x20 + 0x00, 2);  /* IE */
    reg_w(OP + 0, 1 | 4);        /* run, interrupts */
}

/* Port status events until one for port; 1 if it came. */
static int port_event_for(int port) {
    Ev e;
    for (int i = 0; i < 8; i++) {
        if (!next_event(&e)) return 0;
        if (ev_type(&e) == 34 && (int)(e.param >> 24) == port) return 1;
    }
    return 0;
}

/* The interrupt IN endpoint, as the HID driver configures it. */
static void configure_interrupt(Dev *d) {
    memset(at(INPUT), 0, 33 * 32);
    w32(INPUT + 4, 1u | 1u << 3);
    w32(INPUT + 32, 3u << 27 | 1u << 20);
    ring_init(&d->ep[3], ring_base(d->slot, 3));
    w32(INPUT + 32 * 4 + 4, 7u << 3 | 3u << 1 | 64u << 16); /* dci 3: interrupt IN, 64 bytes */
    w64(INPUT + 32 * 4 + 8, d->ep[3].base | 1);
    Ev e = command(INPUT, 12u << 10 | (uint32_t)d->slot << 24);
    check("configure endpoint", ev_cc(&e), 1);
}

#define REPORT_PA 0x41300000ull
static uint64_t posted[64];
static int nposted, ndone;

/* The HID driver keeps transfers posted on the interrupt endpoint; each takes one report. */
static void post(Dev *d, int n) {
    for (int i = 0; i < n; i++, nposted++) {
        uint64_t buf = REPORT_PA + 64ull * (uint64_t)(nposted % 64);
        memset(at(buf), 0xee, 64);
        posted[nposted % 64] = ring_put(&d->ep[3], buf, 64, 1u << 10 | 1u << 5 | 1u << 2);
    }
    reg_w(DB + 4u * (uint32_t)d->slot, 3);
}

/* The next report the guest got: its bytes (64 with the rest 0xee), or 0 if none came. */
static int report(uint8_t out[64]) {
    Ev e;
    for (;;) {
        if (!next_event(&e)) return 0;
        if (ev_type(&e) == 32) break;
    }
    check("report on the interrupt endpoint", (e.ctl >> 16) & 0x1f, 3);
    check("report in the transfer posted first", (long long)e.param, (long long)posted[ndone % 64]);
    check("report: a short packet of 62 bytes", ev_cc(&e) == 13 && (e.status & 0xffffff) == 2, 1);
    memcpy(out, at(REPORT_PA + 64ull * (uint64_t)(ndone % 64)), 64);
    ndone++;
    return 1;
}

static void check_contact(const char *what, const uint8_t *r, int i, int flags, int id, int cx, int cy) {
    const uint8_t *f = r + 1 + 6 * i;
    char w[160];
    snprintf(w, sizeof w, "%s: contact %d flags", what, i);
    check(w, f[0], flags);
    snprintf(w, sizeof w, "%s: contact %d id", what, i);
    check(w, f[1], id);
    snprintf(w, sizeof w, "%s: contact %d x", what, i);
    check(w, f[2] | f[3] << 8, cx);
    snprintf(w, sizeof w, "%s: contact %d y", what, i);
    check(w, f[4] | f[5] << 8, cy);
}

/* Walks a HID report descriptor: the bits each input report holds, the feature report bits, the
   collections balanced, and the first usage page/usage (what macOS calls PrimaryUsagePage/PrimaryUsage). */
typedef struct {
    int in_bits[4], feat_bits[4], depth, max_depth, bad, page0, usage0, fingers, contact_ids, confidence;
} HidWalk;

static HidWalk walk(const uint8_t *d, int len) {
    HidWalk w = {0};
    int size = 0, count = 0, id = 0, page = 0;
    for (int i = 0; i < len;) {
        uint8_t b = d[i];
        int n = b & 3;
        if (n == 3) n = 4;
        if (i + 1 + n > len) {
            w.bad = 1;
            break;
        }
        uint32_t v = 0;
        for (int k = 0; k < n; k++) v |= (uint32_t)d[i + 1 + k] << (8 * k);
        switch (b & 0xfc) {
        case 0x04: page = (int)v; if (!w.page0) w.page0 = page; break;
        case 0x08:
            if (!w.usage0) w.usage0 = (int)v;
            if (page == 0x0d && v == 0x22) w.fingers++;
            if (page == 0x0d && v == 0x51) w.contact_ids++;
            if (page == 0x0d && v == 0x47) w.confidence++;
            break;
        case 0x74: size = (int)v; break;
        case 0x94: count = (int)v; break;
        case 0x84: id = (int)v; break;
        case 0x80: if (id < 4) w.in_bits[id] += size * count; break;
        case 0xb0: if (id < 4) w.feat_bits[id] += size * count; break;
        case 0xa0: if (++w.depth > w.max_depth) w.max_depth = w.depth; break;
        case 0xc0: if (--w.depth < 0) w.bad = 1; break;
        }
        i += 1 + n;
    }
    if (w.depth) w.bad = 1;
    return w;
}

int main(void) {
    if (hv_machine_init(&m, 4 << 20) != 0) return 1;
    ram = calloc(1, RAM_SIZE);
    hv_machine_add_ram(&m, RAM_PA, RAM_SIZE, ram, 0);
    HvGic *gic = hv_gicv3_create(&m, 0x08000000, 0x080a0000);
    HvPcie *bus = hv_pcie_create(&m, gic, 0x10000000, 0x10000000, 0x50000000, 0x10000000, 32 + 4);
    x = hv_xhci_create(&m, bus);
    Ev e;

    check("every root port", reg_r(4) >> 24, HV_XHCI_ROOT_PORTS);
    check("Supported Protocol covers them", (reg_r(0x800 + 8) >> 8) & 0xff, HV_XHCI_ROOT_PORTS);
    start_controller();
    check("keyboard's port event at start", port_event_for(1), 1);
    check("speaker's port event at start", port_event_for(7), 1);
    check("touchscreen port empty", reg_r(PORTSC(6)) & 1, 0);
    check("touchscreen absent", hv_xhci_touch_state(x), HV_XHCI_TOUCH_ABSENT);
    HvXhciContact c[HV_XHCI_CONTACTS] = {{0}};
    check("a frame with no touchscreen is refused", hv_xhci_touch_frame(x, 1, c), -1);

    /* The keyboard, as before. */
    reg_w(PORTSC(1), (1u << 9) | (1u << 17) | (1u << 4));
    check("keyboard port reset event", port_event_for(1), 1);
    Dev kbd = {0};
    check("address the keyboard", enumerate(&kbd, 1, 1), 1);
    uint8_t buf[1024];
    check("keyboard device descriptor", control(&kbd, 0x80, 6, 0x100, 0, 18, buf), 18);
    check("keyboard is Apple's", buf[8] | buf[9] << 8, 0x05ac);
    check("address on port 6 with nothing there fails", enumerate(&(Dev){0}, 6, 1), 5);

    /* Plugged in while the controller runs. */
    hv_xhci_touch_plug(x, 1, 250, 180);
    check("state before the CPU thread connects it: plugged", hv_xhci_touch_state(x), HV_XHCI_TOUCH_PLUGGED);
    hv_xhci_poll(x);
    check("touchscreen arrives: port 6 status change", port_event_for(6), 1);
    uint32_t ps = reg_r(PORTSC(6));
    check("connected", ps & 1, 1);
    check("full speed", (ps >> 10) & 0xf, 1);
    reg_w(PORTSC(6), (1u << 9) | (1u << 17) | (1u << 4));
    check("touchscreen port reset event", port_event_for(6), 1);
    check("enabled", reg_r(PORTSC(6)) & 2, 2);
    Dev ts = {0};
    check("address the touchscreen", enumerate(&ts, 6, 1), 1);
    check("touchscreen device descriptor", control(&ts, 0x80, 6, 0x100, 0, 18, buf), 18);
    check("not Apple's vendor ID", buf[8] | buf[9] << 8, 0x1209);
    check("no device class (per interface)", buf[4], 0);
    check("config descriptor", control(&ts, 0x80, 6, 0x200, 0, 255, buf), 34);
    check("interface: HID", buf[9 + 5], 3);
    check("interface: no boot protocol", buf[9 + 6] | buf[9 + 7], 0);
    int rlen = buf[18 + 7] | buf[18 + 8] << 8;
    check("report descriptor longer than 255 bytes (16-bit length)", rlen > 255, 1);
    check("endpoint: EP1 IN interrupt", buf[27 + 2] == 0x81 && buf[27 + 3] == 3, 1);
    check("endpoint: 64 bytes", buf[27 + 4] | buf[27 + 5] << 8, 64);
    check("endpoint: every 1 ms", buf[27 + 6], 1);
    check("product string", control(&ts, 0x80, 6, 0x302, 0x409, 255, buf), 2 + 2 * 17);
    check("product string's first letter", buf[2], 'H');
    check("HID descriptor", control(&ts, 0x81, 6, 0x2100, 0, 9, buf), 9);
    check("HID descriptor's report length", buf[7] | buf[8] << 8, rlen);
    check("report descriptor, whole", control(&ts, 0x81, 6, 0x2200, 0, (uint16_t)rlen, buf), rlen);
    HidWalk w = walk(buf, rlen);
    check("report descriptor parses", w.bad, 0);
    check("primary usage page: Digitizer", w.page0, 0x0d);
    check("primary usage: Touch Screen", w.usage0, 0x04);
    check("ten Finger collections", w.fingers, 10);
    check("each with a Contact Identifier", w.contact_ids, 10);
    check("each with Confidence", w.confidence, 10);
    check("input report 1: 61 bytes after its ID", w.in_bits[1], 61 * 8);
    check("no other input report", w.in_bits[0] + w.in_bits[2] + w.in_bits[3], 0);
    check("feature report 2: Contact Count Maximum", w.feat_bits[2], 8);
    check("collections nest two deep", w.max_depth, 2);
    check("not ready before it is configured", hv_xhci_touch_state(x), HV_XHCI_TOUCH_PLUGGED);
    check("set configuration", control(&ts, 0x00, 9, 1, 0, 0, NULL), 0);
    check("get configuration", control(&ts, 0x80, 8, 0, 0, 1, buf) == 1 && buf[0] == 1, 1);
    check("keyboard's configuration untouched", control(&kbd, 0x80, 8, 0, 0, 1, buf) == 1 && buf[0] == 0, 1);
    check("set idle accepted", control(&ts, 0x21, 10, 0, 0, 0, NULL), 0);
    check("get feature 2", control(&ts, 0xa1, 1, 0x0302, 0, 2, buf), 2);
    check("contact count maximum 10", buf[0] == 2 && buf[1] == 10, 1);
    check("not ready before its endpoint runs", hv_xhci_touch_state(x), HV_XHCI_TOUCH_PLUGGED);
    configure_interrupt(&ts);
    check("ready", hv_xhci_touch_state(x), HV_XHCI_TOUCH_READY);

    /* Frames to reports. */
    post(&ts, 4);
    check("nothing to report yet", no_event_soon(), 1);
    c[0] = (HvXhciContact){.id = 0, .down = 1, .x = 1000, .y = 2000};
    c[1] = (HvXhciContact){.id = 3, .down = 1, .x = 32767, .y = 0};
    check("frame taken", hv_xhci_touch_frame(x, 2, c), 0);
    uint8_t r[64];
    check("report came", report(r), 1);
    check("report ID 1", r[0], 1);
    check_contact("two down", r, 0, 7, 0, 1000, 2000);
    check_contact("two down", r, 1, 7, 3, 32767, 0);
    /* An empty collection is a contact ID no finger has: macOS reads every collection that changed, whatever
       the count, so as zeros it was contact 0 lifting (at the corner), and the finger that was contact 0 lifted. */
    for (int i = 2; i < 10; i++) check_contact("two down: no finger here", r, i, 0, 0x1f, 0, 0);
    check("contact count 2", r[61], 2);
    check("nothing past the report", r[62] == 0xee && r[63] == 0xee, 1);
    check("get input report: the current state", control(&ts, 0xa1, 1, 0x0101, 0, 62, buf) == 62 && !memcmp(buf, r, 62), 1);
    c[0].down = 0;
    c[1].x = 40000; /* clamped */
    hv_xhci_touch_frame(x, 2, c);
    check("report came", report(r), 1);
    check_contact("one lifted", r, 0, 4, 0, 1000, 2000);
    check_contact("one lifted", r, 1, 7, 3, 32767, 0);
    c[0] = c[1];
    c[0].down = 0, c[0].cancel = 1;
    hv_xhci_touch_frame(x, 1, c);
    check("report came", report(r), 1);
    check_contact("cancelled", r, 0, 0, 3, 32767, 0);
    check("contact count 1", r[61], 1);
    check_contact("a collection emptied is no finger's, never contact 0", r, 1, 0, 0x1f, 0, 0);

    /* The guest not reading: moves merge into the report waiting; going down and up never do. */
    check("no transfers left", no_event_soon(), 1);
    c[0] = (HvXhciContact){.id = 1, .down = 1, .x = 10, .y = 10};
    hv_xhci_touch_frame(x, 1, c);
    check("report came (the fourth transfer)", report(r), 1);
    for (int i = 0; i < 600; i++) {
        c[0].x = (uint16_t)(20 + i);
        check("a move is taken", hv_xhci_touch_frame(x, 1, c) == 0, 1);
    }
    c[0].down = 0;
    check("a lift is taken", hv_xhci_touch_frame(x, 1, c), 0);
    post(&ts, 4);
    check("report came", report(r), 1);
    check_contact("the moves merged: the last", r, 0, 7, 1, 619, 10);
    check("report came", report(r), 1);
    check_contact("then the lift", r, 0, 4, 1, 619, 10);
    check("nothing else", no_event_soon(), 1);

    /* The queue full (the guest stopped reading): a frame that can't wait is dropped, and once there is
       room every finger macOS has down is lifted where it is, so none stays down. (Two transfers are still
       posted.) The last frame taken leaves finger 2 down at (255, 5). */
    int taken = 0;
    for (int i = 0; i < HV_XHCI_REPORTS; i++) {
        c[0] = (HvXhciContact){.id = 2, .down = (uint8_t)(i % 2 == 1), .x = (uint16_t)i, .y = 5};
        taken += hv_xhci_touch_frame(x, 1, c) == 0;
    }
    check("a full queue's worth taken", taken, HV_XHCI_REPORTS);
    c[0] = (HvXhciContact){.id = 4, .down = 1, .x = 7, .y = 7};
    check("a frame past it is dropped", hv_xhci_touch_frame(x, 1, c), -1);
    int got = 0, first_ok = 0, last_ok = 0, lift_ok = 0;
    while (got < HV_XHCI_REPORTS + 1) {
        if (nposted == ndone) post(&ts, 32);
        if (!report(r)) break;
        got++;
        if (got == 1) first_ok = r[1] == 4 && r[2] == 2 && r[3] == 0;
        if (got == HV_XHCI_REPORTS) last_ok = r[1] == 7 && r[2] == 2 && r[3] == 255;
        if (got == HV_XHCI_REPORTS + 1) {
            lift_ok = r[61] == 1;
            check_contact("then the finger down lifted where it was", r, 0, 4, 2, 255, 5);
        }
    }
    check("every report taken came, and one more", got, HV_XHCI_REPORTS + 1);
    check("the first as it was", first_ok, 1);
    check("the last taken as it was", last_ok, 1);
    check("then the finger down lifted, it alone", lift_ok, 1);
    check("a transfer left over", nposted - ndone, 1);
    check("nothing else after", no_event_soon(), 1);
    /* Again, and a frame taken once there is room: the lift goes before it. */
    for (int i = 0; i < HV_XHCI_REPORTS; i++) {
        c[0] = (HvXhciContact){.id = 2, .down = (uint8_t)(i % 2 == 1), .x = (uint16_t)i, .y = 5};
        hv_xhci_touch_frame(x, 1, c);
    }
    c[0] = (HvXhciContact){.id = 4, .down = 1, .x = 7, .y = 7};
    check("full again: dropped", hv_xhci_touch_frame(x, 1, c), -1);
    hv_xhci_poll(x); /* the one transfer left takes one report: room for one */
    check("report came", report(r), 1);
    c[0] = (HvXhciContact){.id = 8, .down = 1, .x = 70, .y = 80};
    check("no room for both the lift and it: dropped", hv_xhci_touch_frame(x, 1, c), -1);
    post(&ts, 1);
    check("report came", report(r), 1);
    check("a frame once there is room for both", hv_xhci_touch_frame(x, 1, c), 0);
    got = 0;
    int order_ok = 0;
    while (got < HV_XHCI_REPORTS) {
        if (nposted == ndone) post(&ts, 32);
        if (!report(r)) break;
        got++;
        if (got == HV_XHCI_REPORTS - 1) order_ok = r[61] == 1 && r[1] == 4 && r[2] == 2 && r[3] == 255;
        if (got == HV_XHCI_REPORTS) order_ok &= r[61] == 1 && r[1] == 7 && r[2] == 8;
    }
    check("the rest, the lift, then that frame", got, HV_XHCI_REPORTS);
    check("in that order", order_ok, 1);
    c[0].down = 0;
    hv_xhci_touch_frame(x, 1, c);
    if (nposted == ndone) post(&ts, 1);
    check("report came", report(r), 1);
    check_contact("its lift", r, 0, 4, 8, 70, 80);
    check("nothing pending", no_event_soon(), 1);
    /* Once more, room made without a frame waiting for it (two reports taken while the queue was full): the
       frame that comes next puts the lift before itself. */
    for (int i = 0; i < HV_XHCI_REPORTS; i++) {
        c[0] = (HvXhciContact){.id = 2, .down = (uint8_t)(i % 2 == 1), .x = (uint16_t)i, .y = 5};
        hv_xhci_touch_frame(x, 1, c);
    }
    c[0] = (HvXhciContact){.id = 4, .down = 1, .x = 7, .y = 7};
    check("full once more: dropped", hv_xhci_touch_frame(x, 1, c), -1);
    if (nposted == ndone) post(&ts, 2);
    else post(&ts, 1);
    check("report came", report(r), 1);
    check("report came", report(r), 1);
    c[0] = (HvXhciContact){.id = 9, .down = 1, .x = 90, .y = 91};
    check("room for both: taken", hv_xhci_touch_frame(x, 1, c), 0);
    got = 0, order_ok = 0;
    while (got < HV_XHCI_REPORTS) {
        if (nposted == ndone) post(&ts, 32);
        if (!report(r)) break;
        got++;
        if (got == HV_XHCI_REPORTS - 1) order_ok = r[61] == 1 && r[1] == 4 && r[2] == 2 && r[3] == 255;
        if (got == HV_XHCI_REPORTS) order_ok &= r[61] == 1 && r[1] == 7 && r[2] == 9;
    }
    check("the rest, the frame's lift, then the frame", got, HV_XHCI_REPORTS);
    check("in that order (the frame put the lift first)", order_ok, 1);
    c[0].down = 0;
    hv_xhci_touch_frame(x, 1, c);
    if (nposted == ndone) post(&ts, 1);
    check("report came", report(r), 1);
    check_contact("its lift", r, 0, 4, 9, 90, 91);
    check("nothing pending", no_event_soon(), 1);

    /* Its endpoint stopped for a moment (macOS aborting a transfer): frames wait for it, none dropped. */
    post(&ts, 4);
    check("nothing to report", no_event_soon(), 1);
    e = command(0, 15u << 10 | 3u << 16 | (uint32_t)ts.slot << 24); /* Stop Endpoint, DCI 3 */
    check("stop the touch endpoint", ev_cc(&e), 1);
    check("not ready while stopped", hv_xhci_touch_state(x), HV_XHCI_TOUCH_PLUGGED);
    c[0] = (HvXhciContact){.id = 5, .down = 1, .x = 300, .y = 400};
    check("a frame while stopped waits", hv_xhci_touch_frame(x, 1, c), 0);
    check("nothing while stopped", no_event_soon(), 1);
    reg_w(DB + 4u * (uint32_t)ts.slot, 3); /* the driver starts it again */
    check("ready again", hv_xhci_touch_state(x), HV_XHCI_TOUCH_READY);
    check("report came once it runs", report(r), 1);
    check_contact("the frame that waited", r, 0, 7, 5, 300, 400);
    c[0].down = 0;
    hv_xhci_touch_frame(x, 1, c);
    check("report came", report(r), 1);
    check_contact("its lift", r, 0, 4, 5, 300, 400);

    /* Started again (macOS resets the port and enumerates it anew): what waited for the old one is gone. */
    c[0] = (HvXhciContact){.id = 6, .down = 1, .x = 1, .y = 1};
    for (int i = 0; i < 3; i++) c[0].down = (uint8_t)!c[0].down, hv_xhci_touch_frame(x, 1, c);
    e = command(0, 10u << 10 | (uint32_t)ts.slot << 24); /* Disable Slot */
    check("disable the old slot", ev_cc(&e), 1);
    check("a frame with no slot is dropped", hv_xhci_touch_frame(x, 1, c), -1);
    reg_w(PORTSC(6), (1u << 9) | (1u << 17) | (1u << 4));
    check("port reset event", port_event_for(6), 1);
    check("address it again", enumerate(&ts, 6, 1), 1);
    check("set configuration again", control(&ts, 0x00, 9, 1, 0, 0, NULL), 0);
    configure_interrupt(&ts);
    check("ready again", hv_xhci_touch_state(x), HV_XHCI_TOUCH_READY);
    nposted = ndone = 0;
    post(&ts, 4);
    check("nothing stale for the new one (no lift of fingers it never saw)", no_event_soon(), 1);
    c[0] = (HvXhciContact){.id = 0, .down = 1, .x = 9, .y = 9};
    hv_xhci_touch_frame(x, 1, c);
    check("report came", report(r), 1);
    check_contact("a new touch", r, 0, 7, 0, 9, 9);
    c[0].down = 0;
    hv_xhci_touch_frame(x, 1, c);
    check("report came", report(r), 1);
    check_contact("lifted", r, 0, 4, 0, 9, 9);

    /* Snapshot state, and what a restore leaves: a finger down at (500, 600) when it was saved. */
    c[0] = (HvXhciContact){.id = 3, .down = 1, .x = 500, .y = 600};
    hv_xhci_touch_frame(x, 1, c);
    check("report came", report(r), 1);
    HvXhciTouchSnap s;
    hv_xhci_touch_get_snap(x, &s);
    check("snap: the fingers macOS has down", s.fingers_known * 100000 + (int)s.mac_down, 100008);
    check("snap: where", s.mac_x[3] * 1000 + s.mac_y[3], 500600);
    check("snap: every root port", s.nports, HV_XHCI_ROOT_PORTS);
    check("snap: present", s.present, 1);
    check("snap: its slot", s.slot, ts.slot);
    check("snap: configured", s.configured, 1);
    check("snap: size", s.w_mm * 1000 + s.h_mm, 250180);
    check("restore", hv_xhci_touch_set_snap(x, &s), 0);
    check("still ready", hv_xhci_touch_state(x), HV_XHCI_TOUCH_READY);
    check("report came after restore", report(r), 1);
    check_contact("the finger down lifted where it was", r, 0, 4, 3, 500, 600);
    check("it alone", r[61], 1);
    check("nothing more", no_event_soon(), 1);
    HvXhciTouchSnap none;
    hv_xhci_touch_get_snap(x, &none);
    check("snap after the lift: none down", none.mac_down, 0);
    check("restore: none down", hv_xhci_touch_set_snap(x, &none), 0);
    check("nothing to lift", no_event_soon(), 1);
    HvXhciTouchSnap old = s;
    old.fingers_known = 0;
    check("restore from a snapshot that didn't say", hv_xhci_touch_set_snap(x, &old), 0);
    if (nposted == ndone) post(&ts, 2);
    check("report came", report(r), 1);
    for (int i = 0; i < 10; i++) check_contact("every finger lifted as cancelled", r, i, 0, i, 0, 0);
    check("contact count 10", r[61], 10);
    /* Saved again before that lift reached macOS: still not known where they are. */
    check("restore from a snapshot that didn't say", hv_xhci_touch_set_snap(x, &old), 0);
    HvXhciTouchSnap again;
    hv_xhci_touch_get_snap(x, &again);
    check("saved before the lift: still unknown", again.fingers_known, 0);
    if (nposted == ndone) post(&ts, 2);
    check("report came", report(r), 1);
    check("the lift, as cancelled", r[61] == 10 && r[1] == 0, 1);
    HvXhciTouchSnap bad = s;
    bad.slot = kbd.slot;
    check("a touchscreen slot on the keyboard's port is refused", hv_xhci_touch_set_snap(x, &bad), -1);
    check("an older snapshot can't have a slot on port 6", hv_xhci_touch_set_snap(x, NULL), -1);
    check("restore again", hv_xhci_touch_set_snap(x, &s), 0);
    report(r);

    /* Unplugged. */
    hv_xhci_touch_plug(x, 0, 0, 0);
    hv_xhci_poll(x);
    check("unplug: port 6 status change", port_event_for(6), 1);
    check("disconnected", reg_r(PORTSC(6)) & 1, 0);
    check("absent", hv_xhci_touch_state(x), HV_XHCI_TOUCH_ABSENT);
    check("a frame after unplug is refused", hv_xhci_touch_frame(x, 1, c), -1);
    reg_w(DB + 4u * (uint32_t)ts.slot, 3); /* the driver rings it before it learns: nothing happens */
    check("no report from a gone device", no_event_soon(), 1);
    check("configure the keyboard", control(&kbd, 0x00, 9, 1, 0, 0, NULL), 0);
    e = command(0, 17u << 10 | (uint32_t)ts.slot << 24); /* Reset Device on the gone touchscreen's slot */
    check("reset the gone device's slot", ev_cc(&e), 1);
    check("the keyboard stays configured", control(&kbd, 0x80, 8, 0, 0, 1, buf) == 1 && buf[0] == 1, 1);
    e = command(0, 10u << 10 | (uint32_t)ts.slot << 24); /* Disable Slot */
    check("disable its slot", ev_cc(&e), 1);

    /* A Mac resumed from a snapshot older than the touchscreen: its driver knows five ports. */
    check("restore from an older snapshot", hv_xhci_touch_set_snap(x, NULL), 0);
    check("five ports for it", reg_r(4) >> 24, 5);
    hv_xhci_touch_plug(x, 1, 250, 180);
    hv_xhci_poll(x);
    check("unavailable", hv_xhci_touch_state(x), HV_XHCI_TOUCH_UNAVAILABLE);
    check("port 6 isn't there for it", reg_r(PORTSC(6)), 0);
    check("no port event", no_event_soon(), 1);
    HvXhciTouchSnap s5;
    hv_xhci_touch_get_snap(x, &s5);
    check("a snapshot of it keeps five", s5.nports * 10 + s5.present, 50);
    start_controller(); /* it resets the controller (waking from sleep, say): its driver may not read the count again */
    check("still five ports", reg_r(4) >> 24, 5);
    hv_xhci_poll(x);
    int got6 = 0;
    for (int i = 0; i < 6 && !got6; i++) got6 = port_event_for(6);
    check("the plug still waits", got6, 0);
    check("still unavailable", hv_xhci_touch_state(x), HV_XHCI_TOUCH_UNAVAILABLE);
    check("a frame is refused", hv_xhci_touch_frame(x, 1, c), -1);
    hv_xhci_touch_plug(x, 0, 0, 0);
    hv_xhci_poll(x);
    check("unplugged: absent", hv_xhci_touch_state(x), HV_XHCI_TOUCH_ABSENT);

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
