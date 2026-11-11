/* The USB Audio Class 2.0 speaker on the xHCI's port 7, driven the way macOS's xHCI and AppleUSBAudio drivers
   drive it: there from the start, enumerated high speed, its descriptors (an audio function with a clock, a
   feature unit with master volume and mute, an isochronous stream in two formats, a status endpoint, and a
   HID interface for the volume buttons), its class requests, isochronous OUT transfers run at the end of the
   frame they were queued for (Frame ID and SIA, late ones at once, Block Event Interrupt, chained TRBs),
   MFINDEX counting and wrapping, the volume changed on the host's side reaching macOS (an interrupt data
   message) and the buttons (HID reports), the PCM reaching the host (priming, running dry, skipping ahead,
   both formats), the volume mapping, and its snapshot state. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "machine.h"
#include "pcie.h"
#include "sysreg.h"

#include "usb_audio.h"
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

static int64_t frame_now(void) { return (int64_t)(hv_counter() / 24000); }

/* ---- rings ---- */

typedef struct {
    uint64_t base;
    int idx, cycle;
} Ring;

static void ring_init(Ring *r, uint64_t base) {
    memset(at(base), 0, 64 * 16);
    r->base = base, r->idx = 0, r->cycle = 1;
}

static uint64_t ring_put(Ring *r, uint64_t param, uint32_t status, uint32_t ctl) {
    if (r->idx == 63) {
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

#define ER 0x40003000ull
static int er_idx, er_cycle = 1;
typedef struct {
    uint64_t param;
    uint32_t status, ctl;
} Ev;

static int take_event(Ev *e) {
    uint64_t t = ER + 16ull * (uint64_t)er_idx;
    uint32_t ctl = r32(t + 12);
    if ((ctl & 1) != (uint32_t)er_cycle) return 0;
    memcpy(&e->param, at(t), 8);
    e->status = r32(t + 8);
    e->ctl = ctl;
    if (++er_idx == 256) er_idx = 0, er_cycle ^= 1;
    reg_w(RT + 0x20 + 0x18, (uint32_t)(ER + 16ull * (uint64_t)er_idx) | 8);
    reg_w(RT + 0x20 + 0x1c, (uint32_t)((ER + 16ull * (uint64_t)er_idx) >> 32));
    return 1;
}

static int next_event(Ev *e) {
    for (int tries = 0; tries < 2000; tries++) {
        if (take_event(e)) return 1;
        hv_xhci_poll(x);
        usleep(500);
    }
    return 0;
}

static int ev_type(const Ev *e) { return (e->ctl >> 10) & 0x3f; }
static int ev_cc(const Ev *e) { return e->status >> 24; }
static int ev_dci(const Ev *e) { return (e->ctl >> 16) & 0x1f; }

static Ring cmd;
static Ev command(uint64_t param, uint32_t ctl) {
    uint64_t t = ring_put(&cmd, param, 0, ctl);
    reg_w(DB, 0);
    Ev e = {0};
    while (next_event(&e) && ev_type(&e) != 33) {}
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
    Ev e = command(0, 9u << 10);
    check("enable slot", ev_cc(&e), 1);
    d->slot = (int)(e.ctl >> 24);
    w64(DCBAA + 8ull * (uint64_t)d->slot, out_ctx(d->slot));
    memset(at(out_ctx(d->slot)), 0, 1024);
    memset(at(INPUT), 0, 33 * 32);
    w32(INPUT + 4, 3);
    w32(INPUT + 32, 1u << 27 | (uint32_t)speed << 20);
    w32(INPUT + 32 + 4, (uint32_t)port << 16);
    ring_init(&d->ep[1], ring_base(d->slot, 1));
    w32(INPUT + 64 + 4, 4u << 3 | 3u << 1 | 64u << 16);
    w64(INPUT + 64 + 8, d->ep[1].base | 1);
    e = command(INPUT, 11u << 10 | (uint32_t)d->slot << 24);
    return ev_cc(&e);
}

/* A control transfer on EP0: the bytes an IN data stage got, 0 for OUT, -1 on a stall. idt: an OUT data
   stage's bytes in the TRB itself. */
static int control_x(Dev *d, uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint16_t len, void *buf, int idt) {
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
        uint64_t param = data;
        if (idt) memcpy(&param, buf, len < 8 ? len : 8);
        dt = ring_put(&d->ep[1], param, len, 3u << 10 | 1u << 2 | (uint32_t)(idt ? 1u << 6 : 0) | (uint32_t)(in ? 1 : 0) << 16);
    }
    uint64_t st = ring_put(&d->ep[1], 0, 0, 4u << 10 | 1u << 5 | (uint32_t)(in && len ? 0 : 1) << 16);
    reg_w(DB + 4u * (uint32_t)d->slot, 1);
    Ev e;
    int got = in ? len : 0;
    for (;;) {
        if (!next_event(&e)) return -2;
        if (ev_type(&e) != 32) continue;
        if (ev_cc(&e) == 6) return -1;
        if (e.param == dt && ev_cc(&e) == 13) got = len - (int)(e.status & 0xffffff);
        if (e.param == st) break;
    }
    if (in && buf && got > 0) memcpy(buf, at(data), (size_t)got);
    return got;
}

static int control(Dev *d, uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint16_t len, void *buf) {
    return control_x(d, type, req, value, index, len, buf, 0);
}

/* A halted EP0 (a stall) started again, as the driver does. */
static void unstall(Dev *d) {
    Ev e = command(0, 14u << 10 | 1u << 16 | (uint32_t)d->slot << 24); /* Reset Endpoint */
    check("reset endpoint", ev_cc(&e), 1);
    uint64_t deq = (d->ep[1].base + 16ull * (uint64_t)d->ep[1].idx) | (uint64_t)d->ep[1].cycle;
    e = command(deq, 16u << 10 | 1u << 16 | (uint32_t)d->slot << 24); /* Set TR Dequeue Pointer */
    check("set dequeue", ev_cc(&e), 1);
}

static void start_controller(void) {
    reg_w(OP + 0, 2);
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
    reg_w(RT + 0x20 + 0x08, 1);
    reg_w(RT + 0x20 + 0x18, (uint32_t)ER);
    reg_w(RT + 0x20 + 0x1c, 0);
    reg_w(RT + 0x20 + 0x10, 0x40004000u);
    reg_w(RT + 0x20 + 0x14, 0);
    reg_w(RT + 0x20 + 0x00, 2);
    reg_w(OP + 0, 1 | 4);
}

static int port_event_for(int port) {
    Ev e;
    for (int i = 0; i < 8; i++) {
        if (!next_event(&e)) return 0;
        if (ev_type(&e) == 34 && (int)(e.param >> 24) == port) return 1;
    }
    return 0;
}

/* The speaker's endpoints, as AppleUSBAudio has them configured: the status interrupt IN (DCI 5) and the
   HID's (DCI 7) at once, and the isochronous OUT (DCI 2) when it picks a stream format. */
static void configure_eps(Dev *d, uint32_t add, uint32_t drop) {
    memset(at(INPUT), 0, 33 * 32);
    w32(INPUT, drop);
    w32(INPUT + 4, 1u | add);
    w32(INPUT + 32, 7u << 27 | 3u << 20);
    for (int dci = 2; dci < 8; dci++) {
        if (!(add & (1u << dci))) continue;
        uint64_t c = INPUT + 32ull * (uint64_t)(dci + 1);
        ring_init(&d->ep[dci], ring_base(d->slot, dci));
        if (dci == 2) {
            w32(c, 3u << 16);                              /* interval 2^3 microframes: 1 ms */
            w32(c + 4, 1u << 3 | 392u << 16);              /* isochronous OUT */
        } else {
            w32(c, 4u << 16);
            w32(c + 4, 7u << 3 | 3u << 1 | 8u << 16);      /* interrupt IN */
        }
        w64(c + 8, d->ep[dci].base | 1);
    }
    Ev e = command(INPUT, 12u << 10 | (uint32_t)d->slot << 24);
    check("configure endpoints", ev_cc(&e), 1);
}

/* ---- the host's backend ---- */

static int started, start_bits, stopped, vol_calls, last_db, last_mute;
static void be_start(void *o, unsigned bits) { (void)o, started++, start_bits = (int)bits; }
static void be_stop(void *o) { (void)o, stopped++; }
static void be_volume(void *o, int db, int mute) { (void)o, vol_calls++, last_db = db, last_mute = mute; }

/* ---- descriptors ---- */

typedef struct {
    int iad, ac, as_alts, hid_if, fu_ok, iso_ep, iso_attr, iso_max16, iso_max24, status_ep, hid_ep, header_len, ac_total;
    int bad;
} Walk;

static Walk walk_config(const uint8_t *c, int len) {
    Walk w = {0};
    int intf = -1, alt = 0, ac_start = -1;
    for (int i = 0; i < len;) {
        int bl = c[i], dt = c[i + 1];
        if (bl < 2 || i + bl > len) {
            w.bad = 1;
            break;
        }
        if (dt == 0x0b) w.iad = c[i + 2] == 0 && c[i + 3] == 2 && c[i + 4] == 1 && c[i + 6] == 0x20;
        if (dt == 4) {
            intf = c[i + 2], alt = c[i + 3];
            if (c[i + 5] == 1 && c[i + 6] == 1 && c[i + 7] == 0x20) w.ac = 1;
            if (c[i + 5] == 1 && c[i + 6] == 2 && c[i + 7] == 0x20) w.as_alts++;
            if (c[i + 5] == 3) w.hid_if = intf;
        }
        if (dt == 0x24 && intf == 0) {
            if (c[i + 2] == 0x01) w.header_len = c[i + 6] | c[i + 7] << 8, ac_start = i;
            if (c[i + 2] == 0x06) w.fu_ok = bl == 18 && c[i + 3] == 2 && c[i + 4] == 1 && c[i + 5] == 0x0f;
            if (ac_start >= 0) w.ac_total = i + bl - ac_start;
        }
        if (dt == 5) {
            int addr = c[i + 2], attr = c[i + 3], max = c[i + 4] | c[i + 5] << 8;
            if (addr == 0x01) {
                w.iso_ep = 1, w.iso_attr = attr;
                if (alt == 1) w.iso_max16 = max;
                if (alt == 2) w.iso_max24 = max;
            }
            if (addr == 0x82 && attr == 3 && max == 6) w.status_ep = 1;
            if (addr == 0x83 && attr == 3) w.hid_ep = 1;
        }
        i += bl;
    }
    return w;
}

/* ---- isochronous ---- */

#define PCM_PA 0x41400000ull
static int pcm_n;

/* One 1 ms packet of 16-bit stereo: frame k is (k, -k) from base. */
static void fill16(uint64_t pa, int base, int frames) {
    for (int k = 0; k < frames; k++) {
        int16_t l = (int16_t)(base + k), r = (int16_t)-(base + k);
        memcpy(at(pa + 4ull * (uint64_t)k), &l, 2);
        memcpy(at(pa + 4ull * (uint64_t)k + 2), &r, 2);
    }
}

static uint64_t iso_td(Dev *d, uint32_t len, int sia, int frame_id, int ioc, int bei, uint64_t *pa_out) {
    uint64_t pa = PCM_PA + 512ull * (uint64_t)(pcm_n++ % 256);
    if (pa_out) *pa_out = pa;
    uint32_t ctl = 5u << 10 | (uint32_t)ioc << 5 | 1u << 2 | (uint32_t)bei << 9;
    ctl |= sia ? 1u << 31 : (uint32_t)(frame_id & 0x7ff) << 20;
    return ring_put(&d->ep[2], pa, len, ctl);
}

int main(void) {
    if (hv_machine_init(&m, 4 << 20) != 0) return 1;
    ram = calloc(1, RAM_SIZE);
    hv_machine_add_ram(&m, RAM_PA, RAM_SIZE, ram, 0);
    HvGic *gic = hv_gicv3_create(&m, 0x08000000, 0x080a0000);
    HvPcie *bus = hv_pcie_create(&m, gic, 0x10000000, 0x10000000, 0x50000000, 0x10000000, 32 + 4);
    x = hv_xhci_create(&m, bus);
    HvUsbAudio *a = hv_xhci_speaker(x);
    check("a speaker", a != NULL, 1);
    if (!a) return 1;
    hv_usb_audio_set_name(a, "Test iPad");
    a->backend = (HvUsbAudioBackend){be_start, be_stop, be_volume, NULL};
    Ev e;

    /* ---- the volume mapping ---- */
    check("0 dB is the top", (long long)(hv_usb_audio_db_to_scalar(0) * 1000), 1000);
    check("-60 dB the bottom", (long long)(hv_usb_audio_db_to_scalar(HV_USB_AUDIO_VOL_MIN) * 1000), 0);
    check("-30 dB half way", (long long)lround(hv_usb_audio_db_to_scalar(-30 * 256) * 1000), 500);
    check("below the range is the bottom", (long long)(hv_usb_audio_db_to_scalar(-100 * 256) * 1000), 0);
    check("scalar 0 is the bottom", hv_usb_audio_scalar_to_db(0), HV_USB_AUDIO_VOL_MIN);
    check("scalar 1 is 0 dB", hv_usb_audio_scalar_to_db(1), 0);
    check("scalar 0.5 is -30 dB", hv_usb_audio_scalar_to_db(0.5), -30 * 256);
    int round_trip = 1;
    for (int v = HV_USB_AUDIO_VOL_MIN; v <= HV_USB_AUDIO_VOL_MAX; v += HV_USB_AUDIO_VOL_RES)
        if (hv_usb_audio_scalar_to_db(hv_usb_audio_db_to_scalar(v)) != v) round_trip = 0;
    check("every step goes there and back", round_trip, 1);
    int steps_ok = 1; /* the iPad's 16 volume steps each land on a step of the range, in order */
    for (int k = 1; k <= 16; k++)
        if (hv_usb_audio_scalar_to_db(k / 16.0) <= hv_usb_audio_scalar_to_db((k - 1) / 16.0)) steps_ok = 0;
    check("the iPad's 16 steps are 16 volumes", steps_ok, 1);

    /* ---- the controller ---- */
    check("every root port (the camera's too)", reg_r(4) >> 24, HV_XHCI_ROOT_PORTS);
    check("isochronous scheduling threshold: 2 frames", reg_r(8) & 0xf, 0xa);
    check("MFINDEX stands still while halted", reg_r(RT), 0);
    start_controller();
    check("keyboard's port event", port_event_for(1), 1);
    check("speaker's port event", port_event_for(7), 1);
    uint32_t ps = reg_r(PORTSC(7));
    check("speaker connected", ps & 1, 1);
    check("speaker is high speed", (ps >> 10) & 0xf, 3);

    uint32_t mf0 = reg_r(RT);
    usleep(4000);
    uint32_t mf1 = reg_r(RT);
    int dmf = (int)((mf1 - mf0) & 0x3fff);
    check("MFINDEX counts microframes (4 ms: about 32)", dmf >= 28 && dmf < 400, 1);
    check("MFINDEX is the guest's time", (int)(((hv_counter() / 3000) - reg_r(RT)) & 0x3fff) < 8, 1);

    reg_w(PORTSC(7), (1u << 9) | (1u << 17) | (1u << 4));
    check("speaker port reset", port_event_for(7), 1);
    Dev spk = {0};
    check("address the speaker", enumerate(&spk, 7, 3), 1);

    uint8_t buf[1024];
    check("device descriptor", control(&spk, 0x80, 6, 0x100, 0, 18, buf), 18);
    check("class: interface association", buf[4] << 16 | buf[5] << 8 | buf[6], 0xef0201);
    check("USB 2.0", buf[2] | buf[3] << 8, 0x200);
    check("pid.codes vendor", buf[8] | buf[9] << 8, 0x1209);
    check("device qualifier", control(&spk, 0x80, 6, 0x600, 0, 10, buf), 10);
    check("config header", control(&spk, 0x80, 6, 0x200, 0, 9, buf), 9);
    int total = buf[2] | buf[3] << 8;
    check("three interfaces", buf[4], 3);
    check("whole configuration", control(&spk, 0x80, 6, 0x200, 0, (uint16_t)total, buf), total);
    Walk w = walk_config(buf, total);
    check("descriptors well formed", w.bad, 0);
    check("interface association: audio 2.0 over interfaces 0-1", w.iad, 1);
    check("audio control interface (2.0)", w.ac, 1);
    check("audio streaming: three settings", w.as_alts, 3);
    check("class-specific AC length", w.header_len, w.ac_total);
    check("feature unit: master mute and volume", w.fu_ok, 1);
    check("isochronous OUT, adaptive", w.iso_ep && w.iso_attr == 0x09, 1);
    check("16-bit: 49 frames a packet", w.iso_max16, 196);
    check("24-bit: 49 frames a packet", w.iso_max24, 392);
    check("status interrupt endpoint", w.status_ep, 1);
    check("HID interface", w.hid_if, 2);
    check("HID endpoint", w.hid_ep, 1);
    int sl = control(&spk, 0x80, 6, 0x302, 0x409, 255, buf);
    char name[64] = {0};
    for (int i = 0; 2 + 2 * i < sl && i < 63; i++) name[i] = (char)buf[2 + 2 * i];
    check("named for the host", strcmp(name, "Test iPad"), 0);
    int rl = control(&spk, 0x81, 6, 0x2200, 2, 255, buf);
    check("HID report descriptor", rl, 27);
    check("consumer control", buf[0] == 0x05 && buf[1] == 0x0c && buf[2] == 0x09 && buf[3] == 0x01, 1);
    check("HID descriptor", control(&spk, 0x81, 6, 0x2100, 2, 9, buf) == 9 && buf[1] == 0x21 && buf[7] == 27, 1);

    check("SET_CONFIGURATION", control(&spk, 0x00, 9, 1, 0, 0, NULL), 0);
    check("GET_CONFIGURATION", control(&spk, 0x80, 8, 0, 0, 1, buf) == 1 && buf[0] == 1, 1);

    /* ---- class requests ---- */
    check("clock: GET_CUR rate", control(&spk, 0xa1, 1, 0x0100, 0x1000, 4, buf), 4);
    check("48 kHz", buf[0] | buf[1] << 8 | buf[2] << 16 | buf[3] << 24, 48000);
    check("clock: GET_RANGE count first", control(&spk, 0xa1, 2, 0x0100, 0x1000, 2, buf) == 2 && buf[0] == 1, 1);
    check("clock: GET_RANGE", control(&spk, 0xa1, 2, 0x0100, 0x1000, 14, buf), 14);
    check("one rate", (buf[2] | buf[3] << 8 | buf[4] << 16) == 48000 && (buf[6] | buf[7] << 8 | buf[8] << 16) == 48000, 1);
    check("clock valid", control(&spk, 0xa1, 1, 0x0200, 0x1000, 1, buf) == 1 && buf[0] == 1, 1);
    uint8_t rate[4] = {0x80, 0xbb, 0, 0};
    check("SET_CUR 48 kHz", control(&spk, 0x21, 1, 0x0100, 0x1000, 4, rate), 0);
    uint8_t rate441[4] = {0x44, 0xac, 0, 0};
    check("SET_CUR 44.1 kHz stalls", control(&spk, 0x21, 1, 0x0100, 0x1000, 4, rate441), -1);
    unstall(&spk);
    check("volume range", control(&spk, 0xa1, 2, 0x0200, 0x0200, 8, buf), 8);
    check("volume: one range", buf[0] | buf[1] << 8, 1);
    check("volume: -60 dB", (int16_t)(buf[2] | buf[3] << 8), -60 * 256);
    check("volume: up to 0 dB", (int16_t)(buf[4] | buf[5] << 8), 0);
    check("volume: steps of 1/16 dB", buf[6] | buf[7] << 8, 16);
    check("GET_CUR volume", control(&spk, 0xa1, 1, 0x0200, 0x0200, 2, buf), 2);
    check("full volume at first", (int16_t)(buf[0] | buf[1] << 8), 0);
    int16_t v20 = -20 * 256;
    check("SET_CUR volume", control(&spk, 0x21, 1, 0x0200, 0x0200, 2, &v20), 0);
    check("the host is told the volume", vol_calls == 1 && last_db == -20 * 256 && last_mute == 0, 1);
    check("GET_CUR volume after", control(&spk, 0xa1, 1, 0x0200, 0x0200, 2, buf) == 2 && (int16_t)(buf[0] | buf[1] << 8) == -20 * 256, 1);
    int16_t v13 = -13 * 256 - 7;
    check("SET_CUR volume, in the TRB", control_x(&spk, 0x21, 1, 0x0200, 0x0200, 2, &v13, 1), 0);
    check("rounded to a step", last_db, -13 * 256);
    int16_t vlow = -100 * 256;
    control(&spk, 0x21, 1, 0x0200, 0x0200, 2, &vlow);
    check("below the range: the bottom", last_db, HV_USB_AUDIO_VOL_MIN);
    uint8_t one = 1;
    check("SET_CUR mute", control(&spk, 0x21, 1, 0x0100, 0x0200, 1, &one), 0);
    check("the host is told mute", last_mute, 1);
    check("GET_CUR mute", control(&spk, 0xa1, 1, 0x0100, 0x0200, 1, buf) == 1 && buf[0] == 1, 1);
    uint8_t zero = 0;
    control(&spk, 0x21, 1, 0x0100, 0x0200, 1, &zero);
    check("unmuted", last_mute, 0);
    check("channel 1's volume stalls (master only)", control(&spk, 0xa1, 1, 0x0201, 0x0200, 2, buf), -1);
    unstall(&spk);
    check("an entity that isn't there stalls", control(&spk, 0xa1, 1, 0x0100, 0x0900, 1, buf), -1);
    unstall(&spk);
    check("HID SET_IDLE", control(&spk, 0x21, 0x0a, 0, 2, 0, NULL), 0);
    control(&spk, 0x21, 1, 0x0200, 0x0200, 2, &v20);

    /* ---- the status and HID endpoints ---- */
    configure_eps(&spk, 1u << 5 | 1u << 7, 0);
    uint64_t stat_buf = 0x41200000ull, hid_buf = 0x41210000ull;
    uint64_t st_trb = ring_put(&spk.ep[5], stat_buf, 8, 1u << 10 | 1u << 5 | 1u << 2);
    reg_w(DB + 4u * (uint32_t)spk.slot, 5);
    for (int i = 0; i < 6; i++) hv_xhci_poll(x), usleep(300);
    check("no status while nothing changed", take_event(&e), 0);
    hv_usb_audio_host_volume(a, -30 * 256);
    check("status message", next_event(&e) && ev_type(&e) == 32 && ev_dci(&e) == 5 && e.param == st_trb, 1);
    check("6 bytes of 8: a short packet", ev_cc(&e) == 13 && (e.status & 0xffffff) == 2, 1);
    uint8_t *msg = at(stat_buf);
    check("interrupt data message: CUR of the feature unit's volume",
          msg[0] == 0 && msg[1] == 1 && msg[2] == 0 && msg[3] == 2 && msg[4] == 0 && msg[5] == 2, 1);
    check("macOS reads the new volume", control(&spk, 0xa1, 1, 0x0200, 0x0200, 2, buf) == 2 && (int16_t)(buf[0] | buf[1] << 8) == -30 * 256, 1);
    check("the host isn't told its own change", vol_calls, 6);
    hv_usb_audio_host_volume(a, -30 * 256);
    ring_put(&spk.ep[5], stat_buf, 8, 1u << 10 | 1u << 5 | 1u << 2);
    reg_w(DB + 4u * (uint32_t)spk.slot, 5);
    for (int i = 0; i < 6; i++) hv_xhci_poll(x), usleep(300);
    check("the same volume again: no message", take_event(&e), 0);
    hv_usb_audio_host_mute(a, 1);
    check("mute's status message", next_event(&e) && ev_dci(&e) == 5 && msg[3] == 1 && msg[5] == 2, 1);
    hv_usb_audio_host_mute(a, 0);
    ring_put(&spk.ep[5], stat_buf, 8, 1u << 10 | 1u << 5 | 1u << 2);
    reg_w(DB + 4u * (uint32_t)spk.slot, 5);
    check("unmute's", next_event(&e) && ev_dci(&e) == 5 && msg[3] == 1, 1);

    for (int i = 0; i < 3; i++) ring_put(&spk.ep[7], hid_buf + 8ull * (uint64_t)i, 1, 1u << 10 | 1u << 5);
    reg_w(DB + 4u * (uint32_t)spk.slot, 7);
    hv_usb_audio_host_key(a, HV_USB_AUDIO_KEY_VOLUME_UP);
    check("volume up pressed", next_event(&e) && ev_dci(&e) == 7 && ev_cc(&e) == 1, 1);
    check("let go", next_event(&e) && ev_dci(&e) == 7, 1);
    check("report: volume up, then nothing", *(uint8_t *)at(hid_buf) == 1 && *(uint8_t *)at(hid_buf + 8) == 0, 1);
    hv_usb_audio_host_key(a, HV_USB_AUDIO_KEY_VOLUME_DOWN);
    check("volume down", next_event(&e) && ev_dci(&e) == 7 && *(uint8_t *)at(hid_buf + 16) == 2, 1);
    check("GET_REPORT: what was sent last", control(&spk, 0xa1, 1, 0x0100, 2, 1, buf) == 1 && buf[0] == 2, 1);

    /* ---- the stream ---- */
    check("GET_INTERFACE: no stream", control(&spk, 0x81, 10, 0, 1, 1, buf) == 1 && buf[0] == 0, 1);
    check("SET_INTERFACE 1: 16-bit", control(&spk, 0x01, 11, 1, 1, 0, NULL), 0);
    check("the host starts playing 16-bit", started == 1 && start_bits == 16, 1);
    check("GET_INTERFACE", control(&spk, 0x81, 10, 0, 1, 1, buf) == 1 && buf[0] == 1, 1);
    check("a setting there isn't stalls", control(&spk, 0x01, 11, 3, 1, 0, NULL), -1);
    unstall(&spk);
    configure_eps(&spk, 1u << 2, 0);
    float out[2 * 4096];
    check("nothing to play yet", hv_usb_audio_pull(a, out, 256, 1.0f), 0);
    check("silence", out[0] == 0 && out[511] == 0, 1);

    /* A TD for a frame to come waits for it to end. */
    while (frame_now() % 1 != 0) {}
    int64_t now = frame_now();
    int64_t target = now + 6;
    uint64_t pa;
    uint64_t t = iso_td(&spk, 192, 0, (int)(target & 0x7ff), 1, 0, &pa);
    fill16(pa, 0, 48);
    reg_w(DB + 4u * (uint32_t)spk.slot, 2);
    uint64_t due = hv_xhci_due(x);
    check("due when its frame ends", (long long)due, (long long)((uint64_t)(target + 1) * 24000));
    int early = 0;
    while (frame_now() <= target - 1) {
        hv_xhci_poll(x);
        if (take_event(&e)) early = 1;
        usleep(200);
    }
    check("not before its frame", early, 0);
    check("its transfer event", next_event(&e) && ev_type(&e) == 32 && ev_dci(&e) == 2 && e.param == t, 1);
    check("after its frame ended", frame_now() > target, 1);
    check("all taken", ev_cc(&e) == 1 && (e.status & 0xffffff) == 0, 1);
    check("nothing due now", (long long)hv_xhci_due(x), 0);

    /* SIA TDs, back to back: one a millisecond. */
    int64_t f0 = frame_now();
    int k0 = 48;
    for (int i = 0; i < 40; i++) {
        iso_td(&spk, 192, 1, 0, 1, 0, &pa);
        fill16(pa, k0 + 48 * i, 48);
    }
    reg_w(DB + 4u * (uint32_t)spk.slot, 2);
    int got = 0, ok = 1;
    int64_t f_first = -1;
    while (got < 40) {
        if (!next_event(&e)) break;
        if (ev_type(&e) != 32 || ev_dci(&e) != 2) continue;
        if (ev_cc(&e) != 1) ok = 0;
        if (f_first < 0) f_first = frame_now();
        got++;
    }
    int64_t f_last = frame_now();
    check("40 SIA TDs done", got, 40);
    check("each one all taken", ok, 1);
    check("one a frame: 40 take about 40 ms", f_last - f0 >= 39 && f_last - f0 < 80, 1);
    check("the first in the next frames", f_first - f0 <= 3, 1);
    check("packets reached the speaker", (long long)a->packets, 41);

    /* The host pulls them: from the start, after 30 ms waits. */
    uint32_t n = hv_usb_audio_pull(a, out, 480, 1.0f);
    check("primed: 480 frames", n, 480);
    int pcm_ok = 1;
    for (int k = 0; k < 480; k++)
        if (lroundf(out[2 * k] * 32768.0f) != k || lroundf(out[2 * k + 1] * 32768.0f) != -k) pcm_ok = 0;
    check("the samples, in order, both channels", pcm_ok, 1);
    n = hv_usb_audio_pull(a, out, 480, 0.5f);
    check("gain applied", n == 480 && lroundf(out[0] * 65536.0f) == 480, 1);
    n = hv_usb_audio_pull(a, out, 1100, 1.0f);
    check("ran dry: what there was", n, 1968 - 960);
    check("then silence", out[2 * 1099] == 0 && out[2 * 1008] == 0, 1);
    check("an underrun counted", (long long)a->underruns, 1);
    check("waits for 30 ms again", hv_usb_audio_pull(a, out, 10, 1.0f), 0);

    /* A late TD (its frame has passed) runs at once. */
    now = frame_now();
    t = iso_td(&spk, 192, 0, (int)((now - 3) & 0x7ff), 1, 0, &pa);
    reg_w(DB + 4u * (uint32_t)spk.slot, 2);
    check("a late TD runs at once", take_event(&e) && e.param == t && ev_cc(&e) == 1, 1);

    /* Block Event Interrupt: the event is there, the interrupt isn't. */
    reg_w(RT + 0x20, 3); /* clear IP, keep IE */
    now = frame_now();
    t = iso_td(&spk, 192, 0, (int)((now - 1) & 0x7ff), 1, 1, &pa);
    reg_w(DB + 4u * (uint32_t)spk.slot, 2);
    check("BEI: the event", take_event(&e) && e.param == t, 1);
    check("BEI: no interrupt pending", reg_r(RT + 0x20) & 1, 0);
    t = iso_td(&spk, 192, 0, (int)((now - 1) & 0x7ff), 1, 0, &pa);
    reg_w(DB + 4u * (uint32_t)spk.slot, 2);
    check("without BEI: the event", take_event(&e) && e.param == t, 1);
    check("without BEI: interrupt pending", reg_r(RT + 0x20) & 1, 1);

    /* A TD of an Isoch TRB chained to a Normal TRB: one packet, its event at the last. */
    uint64_t before = a->written;
    now = frame_now();
    uint64_t pa1 = PCM_PA + 0x20000, pa2 = PCM_PA + 0x21000;
    fill16(pa1, 1000, 24);
    fill16(pa2, 1024, 24);
    uint64_t t1 = ring_put(&spk.ep[2], pa1, 96, 5u << 10 | 1u << 4 | (uint32_t)((now - 1) & 0x7ff) << 20);
    uint64_t t2 = ring_put(&spk.ep[2], pa2, 96, 1u << 10 | 1u << 5);
    reg_w(DB + 4u * (uint32_t)spk.slot, 2);
    check("chained TD: one event at its end", take_event(&e) && e.param == t2 && ev_cc(&e) == 1 && e.param != t1, 1);
    check("both TRBs' data", (long long)(a->written - before), 192);
    int16_t s0, s24;
    memcpy(&s0, a->ring + (before % HV_USB_AUDIO_RING), 2);
    memcpy(&s24, a->ring + ((before + 96) % HV_USB_AUDIO_RING), 2);
    check("in order", s0 == 1000 && s24 == 1024, 1);

    /* Too much waiting (the host's audio stalled): it plays the newest 30 ms. */
    int16_t pkt[2 * 48];
    for (int i = 0; i < 250; i++) {
        for (int k = 0; k < 48; k++) pkt[2 * k] = pkt[2 * k + 1] = (int16_t)(i * 48 + k);
        hv_usb_audio_iso_out(a, pkt, sizeof pkt);
    }
    long long skipped = (long long)a->skipped;
    n = hv_usb_audio_pull(a, out, 256, 1.0f);
    check("skipped ahead", (long long)a->skipped, skipped + 1);
    check("a full pull", n, 256);
    int newest = (int)lroundf(out[2 * 255] * 32768.0f);
    check("to about 30 ms from the newest", 250 * 48 - 1 - newest, 1440);
    long long dropped = (long long)a->dropped;
    for (int i = 0; i < 1000; i++) hv_usb_audio_iso_out(a, pkt, sizeof pkt);
    check("a full ring drops packets, not the host's", (long long)a->dropped > dropped, 1);

    /* 24-bit, in four bytes: a new stream, nothing left of the last. */
    check("SET_INTERFACE 2: 24-bit", control(&spk, 0x01, 11, 2, 1, 0, NULL), 0);
    check("the host stops, then starts 24-bit", stopped == 1 && started == 2 && start_bits == 24, 1);
    int32_t p24[2 * 48];
    for (int i = 0; i < 40; i++) {
        for (int k = 0; k < 48; k++) p24[2 * k] = (int32_t)((uint32_t)(i * 48 + k) << 8), p24[2 * k + 1] = -p24[2 * k];
        hv_usb_audio_iso_out(a, p24, sizeof p24);
    }
    n = hv_usb_audio_pull(a, out, 100, 1.0f);
    check("24-bit: from its start", n == 100 && lround(out[0] * 8388608.0) == 0 && lround(out[2 * 99] * 8388608.0) == 99, 1);
    check("24-bit: right channel", lround(out[2 * 99 + 1] * 8388608.0), -99);
    check("a packet that isn't whole frames: the whole frames", (hv_usb_audio_iso_out(a, p24, 13), (long long)(a->written % 8)), (long long)(a->start_at % 8));

    /* ---- snapshots ---- */
    HvXhciSpeakerSnap ss;
    hv_xhci_speaker_get_snap(x, &ss);
    check("snap: its slot", ss.slot, spk.slot);
    check("snap: configured, 24-bit, -30 dB, not muted", ss.configured == 1 && ss.alt == 2 && ss.vol == -30 * 256 && ss.mute == 0, 1);
    check("snap: restores", hv_xhci_speaker_set_snap(x, &ss), 0);
    check("snap: the host plays it again", stopped == 2 && started == 3 && start_bits == 24, 1);
    HvXhciSpeakerSnap bad = ss;
    bad.slot = spk.slot == 1 ? 2 : 1;
    check("snap: a slot that isn't the speaker's", hv_xhci_speaker_set_snap(x, &bad), -1);
    bad = ss, bad.alt = 5;
    check("snap: a setting there isn't", hv_xhci_speaker_set_snap(x, &bad), -1);
    bad = ss, bad.vol = 10 * 256;
    check("snap: a volume out of range", hv_xhci_speaker_set_snap(x, &bad), -1);
    check("snap: restores again", hv_xhci_speaker_set_snap(x, &ss), 0);
    int sv, sm;
    hv_usb_audio_get_volume(a, &sv, &sm);
    check("snap: the volume", sv, -30 * 256);
    check("snap: still sent to", (iso_td(&spk, 384, 0, (int)((frame_now() - 1) & 0x7ff), 1, 0, &pa), reg_w(DB + 4u * (uint32_t)spk.slot, 2), take_event(&e) && ev_dci(&e) == 2 && ev_cc(&e) == 1), 1);

    /* ---- the stream stops; a port reset forgets it ---- */
    check("SET_INTERFACE 0", control(&spk, 0x01, 11, 0, 1, 0, NULL), 0);
    check("the host stops", stopped, 4); /* 24-bit, and the two restores */
    check("stopped: silence", hv_usb_audio_pull(a, out, 64, 1.0f), 0);
    control(&spk, 0x01, 11, 1, 1, 0, NULL);
    reg_w(PORTSC(7), (1u << 9) | (1u << 17) | (1u << 4));
    check("port reset", port_event_for(7), 1);
    hv_usb_audio_get_state(a, &(HvUsbAudioState){0});
    HvUsbAudioState st;
    hv_usb_audio_get_state(a, &st);
    check("a reset: unconfigured, no stream", st.configured == 0 && st.alt == 0, 1);
    check("the volume stays", st.vol, -30 * 256);
    hv_usb_audio_host_volume(a, -40 * 256);
    check("unconfigured: no message waits", hv_usb_audio_pending(a), 0);

    /* MFINDEX wrap events, when the driver asks (USBCMD.EWE): one each 2.048 s. */
    reg_w(OP + 0, 1 | 4 | 1u << 10);
    int wrap = 0;
    for (int i = 0; i < 6 && !wrap; i++)
        while (next_event(&e))
            if (ev_type(&e) == 39) {
                wrap = 1;
                break;
            }
    check("an MFINDEX wrap event", wrap, 1);
    check("MFINDEX just wrapped", reg_r(RT) < 16384 / 4, 1);

    /* A Mac resumed from a snapshot from before the speaker: it isn't there. */
    hv_xhci_touch_set_snap(x, NULL); /* five ports */
    check("older snapshot: no speaker", hv_xhci_speaker_set_snap(x, NULL), -1); /* the speaker's slot is still in use */
    command(0, 10u << 10 | (uint32_t)spk.slot << 24); /* Disable Slot */
    check("older snapshot, slot gone: no speaker", hv_xhci_speaker_set_snap(x, NULL), 0);
    check("a speaker snapshot with five ports makes no sense", hv_xhci_speaker_set_snap(x, &(HvXhciSpeakerSnap){0}), -1);

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
