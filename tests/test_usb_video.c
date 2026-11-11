/* The USB webcam (usb_video.c) on the xHCI's port 8, driven the way macOS's xHCI and UVC drivers drive it:
   plugged in from the start, enumerated high speed, its descriptors (a UVC 1.0 camera, bulk streaming, YUY2 and
   NV12 at three sizes) consistent to the byte, probe and commit negotiated, frames paced at 30 a second in one
   payload each (header, FID toggling, EOF), short packets where a payload ends inside a TD, the host told when
   macOS streams and when it stops (a cleared halt, the ring dropped, no transfers for a while, unconfigured,
   unplugged), frames from the host cropped, scaled and converted (BGRA and NV12 in, YUY2 and NV12 out), and its
   snapshot state, with a Mac resumed from an older snapshot never seeing it. */
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

#include "usb_video.h"

/* What the host was told. */
static struct {
    int calls, on, w, h, fps;
} told;
static void on_stream(void *opaque, int on, int w, int h, int fps) {
    (void)opaque;
    told.calls++, told.on = on, told.w = w, told.h = h, told.fps = fps;
}

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* ---- the device alone ---- */

static int ctl(HvUsbVideo *v, uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint16_t len, const void *data,
               uint8_t *out) {
    uint8_t setup[8] = {type, req, (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)index, (uint8_t)(index >> 8), (uint8_t)len, (uint8_t)(len >> 8)};
    return hv_usb_video_control(v, setup, data, data ? len : 0, out, out ? 1024 : 0);
}

static void commit(HvUsbVideo *v, int fmt, int frame) {
    uint8_t pr[26] = {1, 0, (uint8_t)fmt, (uint8_t)frame, 0x15, 0x16, 0x05, 0};
    check("probe set", ctl(v, 0x21, 0x01, 1 << 8, 1, 26, pr, NULL), 0);
    check("commit set", ctl(v, 0x21, 0x01, 2 << 8, 1, 26, pr, NULL), 0);
}

/* One payload of size bytes through TDs of td_len bytes: its bytes into out (up to cap), the TDs it took. */
static int payload(HvUsbVideo *v, uint8_t *out, uint32_t cap, uint32_t size, uint32_t td_len, uint64_t now, uint32_t *got) {
    static uint8_t tdbuf[1 << 22];
    *got = 0;
    for (int tds = 1; tds < 10000; tds++) {
        HvUsbSeg seg = {tdbuf, 0, td_len};
        int r = hv_usb_video_td(v, &seg, 1, td_len, now);
        if (r == HV_USB_NAK) return -1;
        uint32_t k = (uint32_t)r < cap - *got ? (uint32_t)r : cap - *got;
        memcpy(out + *got, tdbuf, k);
        *got += (uint32_t)r;
        if ((uint32_t)r < td_len) return *got == size ? tds : -3; /* short: the payload ended */
        if (*got == size) return tds;
    }
    return -2;
}

static void device_tests(void) {
    HvUsbVideo *v = hv_usb_video_create(NULL, "iPad Camera");
    hv_usb_video_set_callback(v, on_stream, NULL);
    uint8_t buf[1024];
    static uint8_t frame[2 + 1920 * 1080 * 2], img[1280 * 720 * 4], uvp[1280 * 360];
    uint32_t got;
    uint64_t t = 1000000000ull;

    check("unit: nothing streams before a commit", hv_usb_video_td(v, &(HvUsbSeg){buf, 0, 512}, 1, 512, t), HV_USB_NAK);
    check("unit: configure", ctl(v, 0x00, 9, 1, 0, 0, NULL, NULL), 0);
    check("unit: GET_INFO probe", ctl(v, 0xa1, 0x86, 1 << 8, 1, 1, NULL, buf), 1);
    check("unit: probe supports GET and SET", buf[0], 3);
    check("unit: GET_LEN probe", ctl(v, 0xa1, 0x85, 1 << 8, 1, 2, NULL, buf), 2);
    check("unit: probe is 26 bytes", buf[0] | buf[1] << 8, 26);
    check("unit: GET_DEF probe", ctl(v, 0xa1, 0x87, 1 << 8, 1, 26, NULL, buf), 26);
    check("unit: default format YUY2", buf[2], 1);
    check("unit: default frame 1280x720", buf[3], 1);
    check("unit: default interval 30 fps", buf[4] | buf[5] << 8 | buf[6] << 16, 333333);
    check("unit: default frame size", buf[18] | buf[19] << 8 | buf[20] << 16 | buf[21] << 24, 1280 * 720 * 2);
    check("unit: GET_MAX probe", ctl(v, 0xa1, 0x83, 1 << 8, 1, 26, NULL, buf), 26);
    check("unit: max format", buf[2] * 10 + buf[3], 23);
    uint8_t want[26] = {0, 0, 2, 2};
    check("unit: SET_CUR probe NV12 640x480", ctl(v, 0x21, 0x01, 1 << 8, 1, 26, want, NULL), 0);
    check("unit: GET_CUR probe (UVC 1.1 length)", ctl(v, 0xa1, 0x81, 1 << 8, 1, 34, NULL, buf), 34);
    check("unit: negotiated format", buf[2], 2);
    check("unit: negotiated frame", buf[3], 2);
    check("unit: negotiated interval", buf[4] | buf[5] << 8 | buf[6] << 16 | buf[7] << 24, 333333);
    check("unit: NV12 640x480 size", buf[18] | buf[19] << 8 | buf[20] << 16 | buf[21] << 24, 640 * 480 * 3 / 2);
    check("unit: payload: a frame and its header", buf[22] | buf[23] << 8 | buf[24] << 16 | buf[25] << 24, 640 * 480 * 3 / 2 + 2);
    check("unit: UVC 1.1 tail is zero", buf[26] | buf[30] | buf[33], 0);
    uint8_t bogus[26] = {0, 0, 9, 7};
    ctl(v, 0x21, 0x01, 1 << 8, 1, 26, bogus, NULL);
    ctl(v, 0xa1, 0x81, 1 << 8, 1, 26, NULL, buf);
    check("unit: a format it doesn't have becomes the default", buf[2] * 10 + buf[3], 11);
    check("unit: SET_CUR probe with no data stalls", ctl(v, 0x21, 0x01, 1 << 8, 1, 26, NULL, NULL), -1);
    check("unit: a processing unit control stalls", ctl(v, 0xa1, 0x81, 2 << 8, 2 << 8, 2, NULL, buf), -1);
    check("unit: request error code", ctl(v, 0xa1, 0x81, 2 << 8, 0, 1, NULL, buf), 1);
    check("unit: invalid control", buf[0], 6);
    check("unit: error code cleared", ctl(v, 0xa1, 0x81, 2 << 8, 0, 1, NULL, buf) == 1 && buf[0] == 0, 1);
    check("unit: SET_INTERFACE 1 alt 1 stalls (bulk: one setting)", ctl(v, 0x01, 11, 1, 1, 0, NULL, NULL), -1);
    check("unit: no stream told yet", told.calls, 0);

    /* YUY2 640x480: black until a frame comes, in one payload with its header. */
    commit(v, 1, 2);
    hv_usb_video_tick(v, 1, t);
    check("unit: streaming: the host is told", told.calls * 10 + told.on, 11);
    check("unit: told the picture", told.w * 10000 + told.h, 6400480);
    check("unit: told 30 fps", told.fps, 30);
    check("unit: streaming", hv_usb_video_streaming(v), 1);
    uint32_t psize = 2 + 640 * 480 * 2;
    check("unit: one TD, the whole payload", payload(v, frame, sizeof frame, psize, psize, t, &got), 1);
    check("unit: payload length", got, psize);
    check("unit: header length", frame[0], 2);
    check("unit: header: EOH, EOF", frame[1] & 0xfe, 0x82);
    int fid0 = frame[1] & 1;
    check("unit: black YUY2", frame[2] == 16 && frame[3] == 128 && frame[2 + 640 * 480 * 2 - 2] == 16, 1);
    check("unit: the next frame waits its turn", hv_usb_video_td(v, &(HvUsbSeg){buf, 0, 512}, 1, 512, t + 1000000), HV_USB_NAK);

    /* A BGRA frame, red on the left, blue on the right, 1280x720: cropped to 4:3 (960 wide, from x 160). */
    for (int y = 0; y < 720; y++)
        for (int x = 0; x < 1280; x++) {
            uint8_t *p = img + 4 * (y * 1280 + x);
            int red = x < 640;
            p[0] = red ? 0 : 255, p[1] = 0, p[2] = red ? 255 : 0, p[3] = 255;
        }
    check("unit: frame taken", hv_usb_video_frame(v, HV_USB_VIDEO_BGRA, 1280, 720, img, 1280 * 4, NULL, 0), 0);
    t += 34000000;
    check("unit: small TDs: the payload over several, the last short", payload(v, frame, sizeof frame, psize, 100000, t, &got), 7);
    check("unit: all of it", got, psize);
    check("unit: FID toggles", frame[1] & 1, !fid0);
    const uint8_t *row = frame + 2 + 240 * 640 * 2;
    check("unit: red Y", row[0], 82);
    check("unit: red Cb", row[1], 90);
    check("unit: red Cr", row[3], 240);
    check("unit: crop: left of centre still red", row[2 * 318], 82);
    check("unit: crop: right of centre blue", row[2 * 322], 41);
    check("unit: blue Cb", row[2 * 322 + 1], 240);
    check("unit: blue Cr", row[2 * 322 + 3], 110);
    /* No new frame: the last again. */
    t += 34000000;
    check("unit: repeat", payload(v, frame, sizeof frame, psize, psize, t, &got), 1);
    check("unit: the same picture", frame[2 + 240 * 640 * 2], 82);
    /* A TD bigger than the payload ends short. */
    t += 34000000;
    HvUsbSeg big = {frame, 0, sizeof frame};
    check("unit: a bigger TD ends short", hv_usb_video_td(v, &big, 1, sizeof frame, t), (long long)psize);

    /* NV12 at 640x480, given NV12 640x480: the same bytes. */
    commit(v, 2, 2);
    check("unit: a new picture: told again", told.calls * 10 + told.on, 21);
    static uint8_t yp[640 * 480];
    for (int i = 0; i < 640 * 480; i++) yp[i] = (uint8_t)(i * 7 + i / 640);
    for (int i = 0; i < 640 * 240; i++) uvp[i] = (uint8_t)(i * 3 + 1);
    check("unit: NV12 frame taken", hv_usb_video_frame(v, HV_USB_VIDEO_NV12, 640, 480, yp, 640, uvp, 640), 0);
    t += 34000000;
    uint32_t nsize = 2 + 640 * 480 * 3 / 2;
    check("unit: NV12 payload", payload(v, frame, sizeof frame, nsize, nsize, t, &got), 1);
    check("unit: NV12 Y plane as given", memcmp(frame + 2, yp, sizeof yp), 0);
    check("unit: NV12 CbCr plane as given", memcmp(frame + 2 + sizeof yp, uvp, 640 * 240), 0);
    /* A frame made for an older picture is dropped (NV12 into 1080p YUY2 sizes, scaled up 1.5 times). */
    commit(v, 1, 3);
    check("unit: 1080p told", told.w * 10000 + told.h, 19201080);
    check("unit: NV12 into YUY2 1080p", hv_usb_video_frame(v, HV_USB_VIDEO_NV12, 1280, 720, img, 1280, uvp, 1280), 0);
    t += 34000000;
    check("unit: 1080p payload", payload(v, frame, sizeof frame, 2 + 1920 * 1080 * 2, 2 + 1920 * 1080 * 2, t, &got) > 0 && got == 2 + 1920 * 1080 * 2, 1);
    check("unit: bad frames refused", hv_usb_video_frame(v, HV_USB_VIDEO_NV12, 0, 720, img, 1280, uvp, 1280) +
          hv_usb_video_frame(v, HV_USB_VIDEO_NV12, 1280, 720, img, 1280, NULL, 1280) + hv_usb_video_frame(v, 7, 64, 64, img, 4096, NULL, 0), -3);

    /* Stops: a cleared halt (queued TDs don't start it again for a moment), then no TDs for two seconds. */
    int calls = told.calls;
    check("unit: CLEAR_FEATURE halt on 0x81", ctl(v, 0x02, 1, 0, 0x81, 0, NULL, NULL), 0);
    check("unit: stopped: told", told.calls * 10 + told.on, (calls + 1) * 10);
    hv_usb_video_tick(v, 1, t + 1000000);
    check("unit: a TD still queued at once doesn't start it", hv_usb_video_streaming(v), 0);
    hv_usb_video_tick(v, 1, t + 200000000);
    check("unit: a TD after a moment does", told.calls * 10 + told.on, (calls + 2) * 10 + 1);
    hv_usb_video_tick(v, 0, t + 1200000000);
    check("unit: a second with nothing queued: still on", hv_usb_video_streaming(v), 1);
    hv_usb_video_tick(v, 0, t + 2300000000ull);
    check("unit: two seconds with nothing: off", told.calls * 10 + told.on, (calls + 3) * 10);
    hv_usb_video_tick(v, 1, t + 2400000000ull);
    check("unit: queued again: on", hv_usb_video_streaming(v), 1);
    check("unit: SET_CONFIGURATION 0", ctl(v, 0x00, 9, 0, 0, 0, NULL, NULL), 0);
    check("unit: unconfigured: off", told.on, 0);
    hv_usb_video_tick(v, 1, t + 2500000000ull);
    check("unit: uncommitted: stays off", hv_usb_video_streaming(v), 0);
    check("unit: uncommitted: nothing", hv_usb_video_td(v, &(HvUsbSeg){buf, 0, 512}, 1, 512, t + 2600000000ull), HV_USB_NAK);

    /* State. */
    ctl(v, 0x00, 9, 1, 0, 0, NULL, NULL);
    commit(v, 2, 3);
    HvUsbVideoState st;
    hv_usb_video_get_state(v, &st);
    HvUsbVideo *w = hv_usb_video_create(NULL, "iPad Camera");
    check("unit: state restores", hv_usb_video_set_state(w, &st), 0);
    check("unit: restored configured", hv_usb_video_configured(w), 1);
    check("unit: restored: streams the committed picture", payload(w, frame, sizeof frame, 2 + 1920 * 1080 * 3 / 2, 2 + 1920 * 1080 * 3 / 2, t, &got) > 0 && got == 2 + 1920 * 1080 * 3 / 2, 1);
    st.commit[2] = 5;
    check("unit: a state with a format it hasn't is refused", hv_usb_video_set_state(w, &st), -1);
    hv_usb_video_free(w);
    hv_usb_video_free(v);
}

/* ---- on the controller ---- */

static const uint8_t *find_desc(const uint8_t *c, int len, int type, int subtype, int nth) {
    for (int i = 0; i + 2 <= len && c[i] >= 2; i += c[i])
        if (c[i + 1] == type && (subtype < 0 || c[i + 2] == subtype) && nth-- == 0) return c + i;
    return NULL;
}

#define VIDEO_PA 0x40400000ull
static uint64_t trbs[64];
static int ntrbs;

/* A bulk IN TD of len bytes at VIDEO_PA, in TRBs of up to 64 KB (ISP on each, IOC on the last). */
static void post_td(Dev *d, uint32_t len) {
    ntrbs = 0;
    memset(at(VIDEO_PA), 0xee, len);
    for (uint32_t off = 0; off < len;) {
        uint32_t k = len - off > 65536 ? 65536 : len - off;
        int last = off + k == len;
        trbs[ntrbs++] = ring_put(&d->ep[3], VIDEO_PA + off, k, 1u << 10 | 1u << 2 | (last ? 1u << 5 : 1u << 4));
        off += k;
    }
    reg_w(DB + 4u * (uint32_t)d->slot, 3);
}

static int bulk_event(Ev *e) {
    for (;;) {
        if (!next_event(e)) return 0;
        if (ev_type(e) == 32) return 1;
    }
}

int main(void) {
    device_tests();
    memset(&told, 0, sizeof told);

    if (hv_machine_init(&m, 4 << 20) != 0) return 1;
    ram = calloc(1, RAM_SIZE);
    hv_machine_add_ram(&m, RAM_PA, RAM_SIZE, ram, 0);
    HvGic *gic = hv_gicv3_create(&m, 0x08000000, 0x080a0000);
    HvPcie *bus = hv_pcie_create(&m, gic, 0x10000000, 0x10000000, 0x50000000, 0x10000000, 32 + 4);
    x = hv_xhci_create(&m, bus);
    HvUsbVideo *cam = hv_usb_video_create(&m, "iPad Camera");
    hv_usb_video_set_callback(cam, on_stream, NULL);
    check("camera state before attaching: absent", hv_xhci_camera_state(x), HV_XHCI_TOUCH_ABSENT);
    hv_xhci_camera_attach(x, cam);
    Ev e;

    check("eight ports", reg_r(4) >> 24, 8);
    check("Supported Protocol covers eight", (reg_r(0x800 + 8) >> 8) & 0xff, 8);
    check("plugged in, before the CPU thread connects it", hv_xhci_camera_state(x), HV_XHCI_TOUCH_PLUGGED);
    start_controller();
    hv_xhci_poll(x);
    check("camera arrives: port 8 status change", port_event_for(8), 1);
    uint32_t ps = reg_r(PORTSC(8));
    check("connected", ps & 1, 1);
    check("high speed", (ps >> 10) & 0xf, 3);
    check("port 7 holds the speaker", reg_r(PORTSC(7)) & 1, 1);
    reg_w(PORTSC(8), (1u << 9) | (1u << 17) | (1u << 4));
    check("camera port reset event", port_event_for(8), 1);
    Dev c = {0};
    check("address the camera", enumerate(&c, 8, 3), 1);

    uint8_t buf[1024];
    check("device descriptor", control(&c, 0x80, 6, 0x100, 0, 18, buf), 18);
    check("class: miscellaneous, IAD", buf[4] << 16 | buf[5] << 8 | buf[6], 0xef0201);
    check("vendor pid.codes", buf[8] | buf[9] << 8, 0x1209);
    check("product", buf[10] | buf[11] << 8, 0x4868);
    check("config header", control(&c, 0x80, 6, 0x200, 0, 9, buf), 9);
    int total = buf[2] | buf[3] << 8;
    check("config descriptor", control(&c, 0x80, 6, 0x200, 0, (uint16_t)total, buf), total);
    int sum = 0, n = 0;
    for (int i = 0; i < total && buf[i]; i += buf[i]) sum += buf[i], n++;
    check("descriptors add up to wTotalLength", sum, total);
    check("two interfaces", buf[4], 2);
    const uint8_t *iad = find_desc(buf, total, 11, -1, 0);
    check("IAD: interfaces 0-1, video", iad && iad[2] == 0 && iad[3] == 2 && iad[4] == 14 && iad[5] == 3, 1);
    const uint8_t *vci = find_desc(buf, total, 4, -1, 0), *vsi = find_desc(buf, total, 4, -1, 1);
    check("VideoControl interface", vci && vci[2] == 0 && vci[5] == 14 && vci[6] == 1, 1);
    check("VideoStreaming interface", vsi && vsi[2] == 1 && vsi[3] == 0 && vsi[5] == 14 && vsi[6] == 2 && vsi[4] == 1, 1);
    const uint8_t *vch = find_desc(buf, total, 0x24, 1, 0);
    check("VC header: UVC 1.00", vch && vch[3] == 0 && vch[4] == 1, 1);
    int vclen = 0;
    for (const uint8_t *p = vch; p && p[1] == 0x24; p += p[0]) vclen += p[0];
    check("VC header's wTotalLength", vch[5] | vch[6] << 8, vclen);
    check("VC header: streaming interface 1", vch[11] * 10 + vch[12], 11);
    const uint8_t *it = find_desc(buf, total, 0x24, 2, 0), *ot = find_desc(buf, total, 0x24, 3, 0), *pu = find_desc(buf, total, 0x24, 5, 0);
    check("camera terminal 1", it && it[3] == 1 && (it[4] | it[5] << 8) == 0x0201, 1);
    check("processing unit 2 from 1", pu && pu[3] == 2 && pu[4] == 1, 1);
    check("streaming terminal 3 from 2", ot && ot[3] == 3 && (ot[4] | ot[5] << 8) == 0x0101 && ot[7] == 2, 1);
    const uint8_t *vsh = vsi + vsi[0];
    check("VS input header", vsh[1] == 0x24 && vsh[2] == 1, 1);
    int vslen = 0;
    for (const uint8_t *p = vsh; p[1] == 0x24; p += p[0]) vslen += p[0];
    check("VS header's wTotalLength", vsh[4] | vsh[5] << 8, vslen);
    check("VS header: two formats, endpoint 0x81, terminal 3", vsh[3] * 10000 + vsh[6] * 10 + vsh[8], 2 * 10000 + 0x81 * 10 + 3);
    const char *guid[2] = {"YUY2", "NV12"};
    for (int f = 0; f < 2; f++) {
        const uint8_t *fd = find_desc(vsh, total - (int)(vsh - buf), 0x24, 4, f);
        check("uncompressed format", fd && fd[0] == 27 && fd[3] == f + 1, 1);
        check("format GUID", memcmp(fd + 5, guid[f], 4) == 0 && fd[13] == 0x80 && fd[20] == 0x71, 1);
        check("three frames", fd[4], 3);
        for (int i = 0; i < 3; i++) {
            const uint8_t *fr = find_desc(vsh, total - (int)(vsh - buf), 0x24, 5, f * 3 + i); /* (not the processing unit) */
            int w = fr[5] | fr[6] << 8, h = fr[7] | fr[8] << 8;
            uint32_t bytes = (uint32_t)(fr[17] | fr[18] << 8 | fr[19] << 16 | fr[20] << 24);
            check("frame index", fr[3], i + 1);
            check("frame buffer size", bytes, f ? w * h * 3 / 2 : w * h * 2);
            check("one discrete interval: 30 fps", fr[25] == 1 && (fr[26] | fr[27] << 8 | fr[28] << 16) == 333333, 1);
        }
    }
    const uint8_t *ep = find_desc(buf, total, 5, -1, 1);
    check("bulk IN 0x81, 512 bytes", ep && ep[2] == 0x81 && ep[3] == 2 && (ep[4] | ep[5] << 8) == 512, 1);
    check("product string", control(&c, 0x80, 6, 0x302, 0x409, 255, buf), 2 + 2 * 11);
    check("named iPad Camera", buf[2] == 'i' && buf[4] == 'P' && buf[2 + 2 * 5] == 'C', 1);
    check("device qualifier", control(&c, 0x80, 6, 0x600, 0, 10, buf), 10);
    check("set configuration", control(&c, 0x00, 9, 1, 0, 0, NULL), 0);
    check("ready", hv_xhci_camera_state(x), HV_XHCI_TOUCH_READY);

    /* Probe and commit, as the UVC driver does: the data stage reaches the camera. */
    uint8_t pr[26] = {1, 0, 1, 2, 0x15, 0x16, 0x05, 0};
    check("probe SET_CUR", control(&c, 0x21, 0x01, 1 << 8, 1, 26, pr), 26);
    check("probe GET_CUR", control(&c, 0xa1, 0x81, 1 << 8, 1, 26, buf), 26);
    check("probe: YUY2 640x480", buf[2] * 10 + buf[3], 12);
    check("an unknown control stalls", control(&c, 0xa1, 0x81, 1 << 8, 2 << 8, 2, buf), -1);
    Ev r = command(0, 14u << 10 | 1u << 16 | (uint32_t)c.slot << 24); /* Reset Endpoint 1 */
    check("reset EP0", ev_cc(&r), 1);
    uint64_t deq = c.ep[1].base + 16ull * (uint64_t)c.ep[1].idx;
    r = command(deq | (uint64_t)c.ep[1].cycle, 16u << 10 | 1u << 16 | (uint32_t)c.slot << 24);
    check("move EP0's ring past it", ev_cc(&r), 1);
    check("commit SET_CUR", control(&c, 0x21, 0x01, 2 << 8, 1, 26, pr), 26);
    uint8_t bad[2] = {0, 0};
    check("a commit too short stalls in its data stage", control(&c, 0x21, 0x01, 2 << 8, 1, 2, bad), -1);
    r = command(0, 14u << 10 | 1u << 16 | (uint32_t)c.slot << 24);
    deq = c.ep[1].base + 16ull * (uint64_t)c.ep[1].idx;
    r = command(deq | (uint64_t)c.ep[1].cycle, 16u << 10 | 1u << 16 | (uint32_t)c.slot << 24);
    check("EP0 again", ev_cc(&r), 1);
    check("commit GET_CUR", control(&c, 0xa1, 0x81, 2 << 8, 1, 26, buf), 26);
    check("committed 640x480 YUY2", buf[2] * 10 + buf[3], 12);
    check("not streaming before a transfer", told.calls, 0);

    /* The bulk endpoint, and frames on it. */
    memset(at(INPUT), 0, 33 * 32);
    w32(INPUT + 4, 1u | 1u << 3);
    w32(INPUT + 32, 3u << 27 | 3u << 20);
    ring_init(&c.ep[3], ring_base(c.slot, 3));
    w32(INPUT + 32 * 4 + 4, 6u << 3 | 3u << 1 | 512u << 16); /* dci 3: bulk IN, 512 bytes */
    w64(INPUT + 32 * 4 + 8, c.ep[3].base | 1);
    e = command(INPUT, 12u << 10 | (uint32_t)c.slot << 24);
    check("configure the bulk endpoint", ev_cc(&e), 1);
    uint32_t psize = 2 + 640 * 480 * 2;
    post_td(&c, psize);
    check("a frame comes", bulk_event(&e), 1);
    check("the host is told: on, 640x480", told.calls * 100000000 + told.on * 10000000 + told.w * 10000 + told.h, 116400480);
    check("on the bulk endpoint", (e.ctl >> 16) & 0x1f, 3);
    check("whole: success", ev_cc(&e), 1);
    check("at the TD's last TRB", (long long)e.param, (long long)trbs[ntrbs - 1]);
    uint8_t *f = at(VIDEO_PA);
    check("header", f[0] == 2 && (f[1] & 0xfe) == 0x82, 1);
    check("black", f[2] == 16 && f[3] == 128 && f[psize - 2] == 16 && f[psize - 1] == 128, 1);
    int fid = f[1] & 1;
    /* A frame from the host, and a TD bigger than the payload: it ends short, in the TRB the payload ends in. */
    static uint8_t img[640 * 480 * 4];
    memset(img, 0, sizeof img);
    for (int i = 0; i < 640 * 480; i++) img[4 * i + 1] = 255; /* green */
    check("host frame", hv_usb_video_frame(cam, HV_USB_VIDEO_BGRA, 640, 480, img, 640 * 4, NULL, 0), 0);
    uint64_t t0 = now_ns();
    post_td(&c, 700000);
    check("the next frame", bulk_event(&e), 1);
    uint64_t dt = now_ns() - t0;
    check("paced: about a frame's time later", dt > 15000000 && dt < 200000000, 1);
    check("short packet", ev_cc(&e), 13);
    int last = (int)(psize / 65536);
    check("at the TRB the payload ended in", (long long)e.param, (long long)trbs[last]);
    check("residual in that TRB", e.status & 0xffffff, 65536 - (psize - 65536 * (uint32_t)last));
    check("FID toggled", f[1] & 1, !fid);
    check("green Y", f[2], 144);
    check("green Cb", f[3], 54);
    check("green Cr", f[5], 34);

    /* The ring dropped (Stop Endpoint, Set TR Dequeue): the host is told off. */
    post_td(&c, psize);
    check("a third frame", bulk_event(&e), 1);
    post_td(&c, psize);
    r = command(0, 15u << 10 | 3u << 16 | (uint32_t)c.slot << 24);
    check("stop the bulk endpoint", ev_cc(&r), 1);
    deq = c.ep[3].base + 16ull * (uint64_t)c.ep[3].idx;
    r = command(deq | (uint64_t)c.ep[3].cycle, 16u << 10 | 3u << 16 | (uint32_t)c.slot << 24);
    check("its ring moved", ev_cc(&r), 1);
    check("the host is told off", told.calls * 10 + told.on, 20);
    check("no frame after", no_event_soon(), 1);

    /* Snapshot: round trip, and what makes no sense. */
    HvXhciCamSnap s;
    hv_xhci_camera_get_snap(x, &s);
    check("snap: present, its slot", s.present * 100 + s.slot, 100 + c.slot);
    check("snap restores", hv_xhci_camera_set_snap(x, &s), 0);
    check("restored: ready", hv_xhci_camera_state(x), HV_XHCI_TOUCH_READY);
    HvXhciCamSnap b = s;
    b.slot = c.slot % HV_XHCI_SLOTS + 1; /* a slot that isn't the camera's */
    check("a camera slot on another port is refused", hv_xhci_camera_set_snap(x, &b), -1);
    b = s;
    b.dev.commit[3] = 9;
    b.dev.committed = 1;
    check("a commit it can't have is refused", hv_xhci_camera_set_snap(x, &b), -1);
    check("an older snapshot can't have a slot on port 8", hv_xhci_camera_set_snap(x, NULL), -1);
    check("restore again", hv_xhci_camera_set_snap(x, &s), 0);

    /* Unplugged: port 8 disconnects. */
    hv_xhci_camera_plug(x, 0);
    hv_xhci_poll(x);
    check("unplug: port 8 change", port_event_for(8), 1);
    check("disconnected", reg_r(PORTSC(8)) & 1, 0);
    check("absent", hv_xhci_camera_state(x), HV_XHCI_TOUCH_ABSENT);
    check("address on port 8 now fails", enumerate(&(Dev){0}, 8, 3), 5);
    hv_xhci_camera_plug(x, 1);
    hv_xhci_poll(x);
    check("plugged in again", port_event_for(8), 1);

    /* A Mac resumed from a snapshot older than the camera knows six ports: the camera never shows. */
    HvXhci *x2 = hv_xhci_create(&m, bus);
    if (x2) {
        HvUsbVideo *cam2 = hv_usb_video_create(&m, "iPad Camera");
        hv_xhci_camera_attach(x2, cam2);
        HvXhciTouchSnap ts;
        hv_xhci_touch_get_snap(x2, &ts);
        ts.nports = 6;
        check("an older snapshot's six ports", hv_xhci_touch_set_snap(x2, &ts), 0);
        check("and no camera section", hv_xhci_camera_set_snap(x2, NULL), 0);
        hv_xhci_poll(x2);
        check("the camera is unavailable this run", hv_xhci_camera_state(x2), HV_XHCI_TOUCH_UNAVAILABLE);
        check("six ports still", (uint32_t)x2->pci.bar_read(&x2->pci, 0, 4, 4) >> 24, 6);
        check("port 8 isn't connected", x2->cam.present, 0);
        check("a camera section can't be on a six-port Mac", hv_xhci_camera_set_snap(x2, &s), -1);
    } else {
        printf("FAIL a second controller\n"), failures++;
    }

    printf("test_usb_video: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
