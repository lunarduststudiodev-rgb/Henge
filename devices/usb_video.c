#include "usb_video.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "machine.h"
#include "saveguard.h"

/* The pictures it offers (bFrameIndex 1..3, in each format), all at 30 frames a second. */
static const struct { uint16_t w, h; } frames[] = {{1280, 720}, {640, 480}, {1920, 1080}};
#define NFRAMES 3
#define NFORMATS 2
#define INTERVAL 333333u                     /* 100 ns units: 30 frames a second */
#define MAX_FRAME (1920u * 1080u * 2u)       /* the largest frame (1080p YUY2) */
#define PROBE_LEN 26                         /* UVC 1.0's probe and commit */
#define IDLE_NS 2000000000ull                /* no transfer waiting this long: macOS stopped */
#define ABORT_GRACE_NS 100000000ull          /* after a stop, queued transfers don't start it again for this long */
#define EP0_MAX 64
#define EP_BULK 0x81
#define EP_INTR 0x82
#define VID 0x1209 /* pid.codes */
#define PID 0x4868 /* beside Henge's drive (0x4867) */

/* UVC request codes and selectors. */
enum { SET_CUR = 0x01, GET_CUR = 0x81, GET_MIN = 0x82, GET_MAX = 0x83, GET_RES = 0x84, GET_LEN = 0x85, GET_INFO = 0x86, GET_DEF = 0x87 };
enum { VS_PROBE = 1, VS_COMMIT = 2, VS_STREAM_ERROR = 6, VC_REQUEST_ERROR = 2 };
enum { ERR_NONE = 0, ERR_WRONG_STATE = 2, ERR_INVALID_CONTROL = 6, ERR_INVALID_REQUEST = 7 };

struct HvUsbVideo {
    HvMachine *m;
    char name[64];
    HvUsbVideoStream on_stream;
    void *opaque;
    uint8_t dev_desc[18], cfg_desc[512];
    int cfg_len;
    /* CPU thread */
    int configured;
    uint8_t probe[PROBE_LEN], commit[PROBE_LEN];
    int committed;
    int active;               /* macOS streams: the host was told on */
    int act_w, act_h;         /* what it was told */
    uint64_t last_td, held_until, next_due;
    int hold;                 /* stopped by the host: held_until is set at the next tick */
    int in_payload;           /* a payload is under way: pos bytes of header + frame given */
    uint32_t pos, fid;
    int req_err;              /* the last class request's error (VC_REQUEST_ERROR_CODE_CONTROL) */
    /* Frames: three buffers. The producer converts into back (outside mu), then swaps it with ready; the CPU
       thread swaps ready with tx when a frame starts. gen counts the committed picture's changes: a frame made
       for an older one is dropped. Under mu: back/ready/tx indexes, ready_valid, gen, tw/th/tfmt. */
    pthread_mutex_t mu, pmu;
    uint8_t *buf[3];
    int back, ready, tx;
    int ready_valid;
    uint32_t gen, ready_gen, tx_gen; /* tx_gen belongs to the CPU thread */
    int tw, th, tfmt;                /* the committed picture; 0 before a commit */
};

/* ---- descriptors ---- */

static uint32_t frame_bytes(int fmt, int w, int h) {
    return fmt == HV_USB_VIDEO_FMT_NV12 ? (uint32_t)w * (uint32_t)h * 3u / 2u : (uint32_t)w * (uint32_t)h * 2u;
}

static uint8_t *put16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8);
    return p + 2;
}

static uint8_t *put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
    return p + 4;
}

/* The GUIDs of YUY2 and NV12 (FOURCC, then the common tail), which macOS maps to 'yuvs' and '420v'. */
static uint8_t *put_guid(uint8_t *p, const char fourcc[4]) {
    static const uint8_t tail[12] = {0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
    memcpy(p, fourcc, 4);
    memcpy(p + 4, tail, 12);
    return p + 16;
}

static void build_descriptors(HvUsbVideo *v) {
    static const uint8_t dev[18] = {
        18, 1, 0x00, 0x02, 0xef, 0x02, 0x01, EP0_MAX, /* USB 2.0, Miscellaneous / IAD: a composite video function */
        VID & 0xff, VID >> 8, PID & 0xff, PID >> 8,
        0x00, 0x01, 1, 2, 3, 1, /* bcdDevice 1.00, strings 1-3, one configuration */
    };
    memcpy(v->dev_desc, dev, sizeof dev);
    uint8_t *c = v->cfg_desc, *p = c;
    /* configuration: two interfaces, bus powered, 500 mA (wTotalLength at the end) */
    *p++ = 9, *p++ = 2, p += 2, *p++ = 2, *p++ = 1, *p++ = 0, *p++ = 0x80, *p++ = 250;
    /* interface association: interfaces 0-1, video, interface collection */
    *p++ = 8, *p++ = 11, *p++ = 0, *p++ = 2, *p++ = 0x0e, *p++ = 0x03, *p++ = 0x00, *p++ = 2;
    /* VideoControl interface: one endpoint (status interrupts) */
    *p++ = 9, *p++ = 4, *p++ = 0, *p++ = 0, *p++ = 1, *p++ = 0x0e, *p++ = 0x01, *p++ = 0x00, *p++ = 2;
    uint8_t *vc = p;
    /* VC header: UVC 1.00, wTotalLength (below), 48 MHz clock, one streaming interface (1) */
    *p++ = 13, *p++ = 0x24, *p++ = 0x01, p = put16(p, 0x0100), p += 2, p = put32(p, 48000000), *p++ = 1, *p++ = 1;
    /* input terminal 1: a camera, no controls */
    *p++ = 18, *p++ = 0x24, *p++ = 0x02, *p++ = 1, p = put16(p, 0x0201), *p++ = 0, *p++ = 0;
    p = put16(p, 0), p = put16(p, 0), p = put16(p, 0), *p++ = 3, *p++ = 0, *p++ = 0, *p++ = 0;
    /* processing unit 2, from 1: no controls */
    *p++ = 11, *p++ = 0x24, *p++ = 0x05, *p++ = 2, *p++ = 1, p = put16(p, 0), *p++ = 2, *p++ = 0, *p++ = 0, *p++ = 0;
    /* output terminal 3: USB streaming, from 2 */
    *p++ = 9, *p++ = 0x24, *p++ = 0x03, *p++ = 3, p = put16(p, 0x0101), *p++ = 0, *p++ = 2, *p++ = 0;
    put16(vc + 5, (uint32_t)(p - vc));
    /* the status interrupt endpoint (never has anything to say), and its class-specific part */
    *p++ = 7, *p++ = 5, *p++ = EP_INTR, *p++ = 0x03, p = put16(p, 16), *p++ = 8;
    *p++ = 5, *p++ = 0x25, *p++ = 0x03, p = put16(p, 16);
    /* VideoStreaming interface 1, alternate setting 0: the bulk endpoint */
    *p++ = 9, *p++ = 4, *p++ = 1, *p++ = 0, *p++ = 1, *p++ = 0x0e, *p++ = 0x02, *p++ = 0x00, *p++ = 0;
    uint8_t *vs = p;
    /* input header: two formats, endpoint 0x81, to terminal 3, no still images, a control byte per format */
    *p++ = 13 + NFORMATS, *p++ = 0x24, *p++ = 0x01, *p++ = NFORMATS, p += 2, *p++ = EP_BULK, *p++ = 0, *p++ = 3;
    *p++ = 0, *p++ = 0, *p++ = 0, *p++ = 1;
    for (int f = 0; f < NFORMATS; f++) *p++ = 0;
    for (int f = 1; f <= NFORMATS; f++) {
        int nv12 = f == HV_USB_VIDEO_FMT_NV12;
        /* uncompressed format */
        *p++ = 27, *p++ = 0x24, *p++ = 0x04, *p++ = (uint8_t)f, *p++ = NFRAMES;
        p = put_guid(p, nv12 ? "NV12" : "YUY2");
        *p++ = nv12 ? 12 : 16, *p++ = 1, *p++ = 0, *p++ = 0, *p++ = 0, *p++ = 0; /* bits per pixel, default frame 1 */
        for (int i = 0; i < NFRAMES; i++) {
            uint32_t bytes = frame_bytes(f, frames[i].w, frames[i].h);
            uint32_t rate = bytes * 8u * 30u;
            *p++ = 30, *p++ = 0x24, *p++ = 0x05, *p++ = (uint8_t)(i + 1), *p++ = 0;
            p = put16(p, frames[i].w), p = put16(p, frames[i].h);
            p = put32(p, rate), p = put32(p, rate), p = put32(p, bytes), p = put32(p, INTERVAL);
            *p++ = 1, p = put32(p, INTERVAL);
        }
        /* colour matching: BT.709 primaries and transfer, BT.601 (SMPTE 170M) matrix: what the conversion uses */
        *p++ = 6, *p++ = 0x24, *p++ = 0x0d, *p++ = 1, *p++ = 1, *p++ = 4;
    }
    put16(vs + 4, (uint32_t)(p - vs));
    /* the bulk IN endpoint, 512-byte packets */
    *p++ = 7, *p++ = 5, *p++ = EP_BULK, *p++ = 0x02, p = put16(p, 512), *p++ = 0;
    v->cfg_len = (int)(p - c);
    put16(c + 2, (uint32_t)v->cfg_len);
}

static int string_desc(const char *str, uint8_t *out) {
    int n = (int)strlen(str);
    if (n > 126) n = 126;
    out[0] = (uint8_t)(2 + 2 * n), out[1] = 3;
    for (int i = 0; i < n; i++) out[2 + 2 * i] = (uint8_t)str[i], out[3 + 2 * i] = 0;
    return 2 + 2 * n;
}

/* ---- probe and commit ---- */

/* Fill in a probe for format f, frame i (each 1-based, valid), keeping the host's hint. */
static void probe_fill(uint8_t pr[PROBE_LEN], int f, int i) {
    uint16_t hint = (uint16_t)(pr[0] | pr[1] << 8);
    uint32_t bytes = frame_bytes(f, frames[i - 1].w, frames[i - 1].h);
    memset(pr, 0, PROBE_LEN);
    uint8_t *p = put16(pr, hint);
    *p++ = (uint8_t)f, *p++ = (uint8_t)i;
    p = put32(p, INTERVAL);
    p += 2 * 5;                    /* key frame rate, P frame rate, quality, window, delay: none */
    p = put32(p, bytes);           /* dwMaxVideoFrameSize */
    put32(p, bytes + 2);           /* dwMaxPayloadTransferSize: a frame and its header in one payload */
}

/* What the host asked for, made into what it gets: a format and frame it doesn't name become the default. */
static void negotiate(uint8_t out[PROBE_LEN], const uint8_t *in, int len) {
    uint8_t req[PROBE_LEN];
    memcpy(req, out, PROBE_LEN);
    memcpy(req, in, (size_t)(len < PROBE_LEN ? len : PROBE_LEN));
    int f = req[2], i = req[3];
    if (f < 1 || f > NFORMATS) f = 1;
    if (i < 1 || i > NFRAMES) i = 1;
    memcpy(out, req, 2);
    probe_fill(out, f, i);
}

static int probe_valid(const uint8_t pr[PROBE_LEN]) {
    return pr[2] >= 1 && pr[2] <= NFORMATS && pr[3] >= 1 && pr[3] <= NFRAMES;
}

/* ---- streaming state ---- */

static void tell(HvUsbVideo *v, int on) {
    if (on) {
        int i = v->commit[3];
        v->act_w = frames[i - 1].w, v->act_h = frames[i - 1].h;
    }
    v->active = on;
    if (v->on_stream) v->on_stream(v->opaque, on, on ? v->act_w : 0, on ? v->act_h : 0, on ? 30 : 0);
}

static void stop_stream(HvUsbVideo *v) {
    v->in_payload = 0;
    v->pos = 0;
    v->next_due = 0;
    if (v->active) tell(v, 0);
}

/* The committed picture became what commit says: the frames are made for it from now. */
static void set_target(HvUsbVideo *v) {
    int f = v->commit[2], i = v->commit[3];
    pthread_mutex_lock(&v->mu);
    if (!v->buf[0])
        for (int k = 0; k < 3; k++) v->buf[k] = malloc(MAX_FRAME);
    if (v->tfmt != f || v->tw != frames[i - 1].w || v->th != frames[i - 1].h) {
        v->tfmt = f, v->tw = frames[i - 1].w, v->th = frames[i - 1].h;
        v->gen++;
        v->ready_valid = 0;
    }
    pthread_mutex_unlock(&v->mu);
}

static void black(uint8_t *b, int fmt, int w, int h) {
    if (fmt == HV_USB_VIDEO_FMT_NV12) {
        memset(b, 16, (size_t)w * (size_t)h);
        memset(b + (size_t)w * (size_t)h, 128, (size_t)w * (size_t)h / 2);
    } else {
        for (size_t k = 0; k < (size_t)w * (size_t)h * 2; k += 2) b[k] = 16, b[k + 1] = 128;
    }
}

/* A new frame starts: the newest the camera gave, or the last again, or black. */
static void start_frame(HvUsbVideo *v, uint64_t now) {
    uint64_t iv = (uint64_t)INTERVAL * 100;
    if (!v->next_due || now - v->next_due > iv) v->next_due = now + iv; /* late (or the first): from now */
    else v->next_due += iv;
    pthread_mutex_lock(&v->mu);
    if (v->ready_valid && v->ready_gen == v->gen) {
        int t = v->tx;
        v->tx = v->ready, v->ready = t;
        v->ready_valid = 0;
        v->tx_gen = v->gen;
    } else if (v->tx_gen != v->gen) {
        black(v->buf[v->tx], v->tfmt, v->tw, v->th);
        v->tx_gen = v->gen;
    }
    pthread_mutex_unlock(&v->mu);
    v->fid ^= 1;
    v->in_payload = 1;
    v->pos = 0;
}

/* ---- the controller's side ---- */

int hv_usb_video_control(HvUsbVideo *v, const uint8_t setup[8], const uint8_t *data, int data_len, uint8_t *out, int max) {
    uint8_t type = setup[0], req = setup[1];
    uint16_t value = (uint16_t)(setup[2] | setup[3] << 8), index = (uint16_t)(setup[4] | setup[5] << 8);
    uint16_t wlen = (uint16_t)(setup[6] | setup[7] << 8);
    uint8_t tmp[256];
    const uint8_t *src = tmp;
    int len = 0;
    if (type == 0x80 && req == 6) { /* GET_DESCRIPTOR */
        uint8_t dt = value >> 8, idx = value & 0xff;
        static const uint8_t qualifier[10] = {10, 6, 0x00, 0x02, 0xef, 0x02, 0x01, EP0_MAX, 1, 0};
        if (dt == 1) src = v->dev_desc, len = sizeof v->dev_desc;
        else if (dt == 2) src = v->cfg_desc, len = v->cfg_len;
        else if (dt == 6) src = qualifier, len = sizeof qualifier;
        else if (dt == 3 && idx == 0) tmp[0] = 4, tmp[1] = 3, tmp[2] = 0x09, tmp[3] = 0x04, len = 4;
        else if (dt == 3 && idx == 1) len = string_desc("Henge", tmp);
        else if (dt == 3 && idx == 2) len = string_desc(v->name, tmp);
        else if (dt == 3 && idx == 3) len = string_desc("1", tmp);
        else return -1;
    } else if ((type & 0xfc) == 0x80 && req == 0) { /* GET_STATUS */
        tmp[0] = tmp[1] = 0, len = 2;
    } else if (type == 0x80 && req == 8) { /* GET_CONFIGURATION */
        tmp[0] = (uint8_t)v->configured, len = 1;
    } else if (type == 0x00 && req == 9) { /* SET_CONFIGURATION: whatever streamed stops */
        stop_stream(v);
        v->committed = 0;
        v->configured = value & 0xff;
        return 0;
    } else if (type == 0x81 && req == 10) { /* GET_INTERFACE */
        tmp[0] = 0, len = 1;
    } else if (type == 0x01 && req == 11) { /* SET_INTERFACE: one setting each; on the streaming one, a stop */
        if (value != 0 || (index & 0xff) > 1) return -1;
        if ((index & 0xff) == 1) hv_usb_video_abort(v);
        return 0;
    } else if (type == 0x02 && (req == 1 || req == 3)) { /* CLEAR_FEATURE / SET_FEATURE on an endpoint */
        if (req == 1 && (index & 0xff) == EP_BULK) hv_usb_video_abort(v); /* a halt cleared: the stream stops */
        return 0;
    } else if ((type == 0x00 || type == 0x01) && (req == 1 || req == 3)) {
        return 0; /* device or interface features (remote wakeup): nothing to do */
    } else if ((type == 0x21 || type == 0xa1) && (index >> 8) == 0) { /* class requests to an interface itself */
        int intf = index & 0xff, sel = value >> 8, in = type == 0xa1, prev_err = v->req_err;
        v->req_err = ERR_NONE;
        if (intf == 1 && (sel == VS_PROBE || sel == VS_COMMIT)) {
            uint8_t *cur = sel == VS_PROBE ? v->probe : v->commit;
            if (!in && req == SET_CUR) {
                if (!data || data_len < 4) return v->req_err = ERR_INVALID_REQUEST, -1;
                if (sel == VS_PROBE) {
                    negotiate(v->probe, data, data_len);
                } else {
                    uint8_t c[PROBE_LEN];
                    memcpy(c, v->commit, PROBE_LEN);
                    negotiate(c, data, data_len);
                    int changed = !v->committed || memcmp(c + 2, v->commit + 2, 2) != 0;
                    memcpy(v->commit, c, PROBE_LEN);
                    v->committed = 1;
                    set_target(v);
                    v->in_payload = 0, v->pos = 0, v->next_due = 0, v->held_until = 0, v->hold = 0;
                    if (v->active && changed) tell(v, 1); /* the host's camera makes the new picture */
                }
                return 0;
            }
            if (!in) return v->req_err = ERR_INVALID_REQUEST, -1;
            switch (req) {
            case GET_CUR: memcpy(tmp, cur, PROBE_LEN); break;
            case GET_MIN:
            case GET_DEF: memset(tmp, 0, PROBE_LEN), probe_fill(tmp, 1, 1); break;
            case GET_MAX: memset(tmp, 0, PROBE_LEN), probe_fill(tmp, NFORMATS, NFRAMES); break;
            case GET_RES: memset(tmp, 0, PROBE_LEN); break;
            case GET_LEN: tmp[0] = PROBE_LEN, tmp[1] = 0, len = 2; break;
            case GET_INFO: tmp[0] = 0x03, len = 1; break; /* GET and SET */
            default: return v->req_err = ERR_INVALID_REQUEST, -1;
            }
            if (!len) { /* a probe: as long as the host asked (UVC 1.1 hosts ask 34), the rest zero */
                len = wlen < 48 ? wlen : 48;
                if (len > PROBE_LEN) memset(tmp + PROBE_LEN, 0, (size_t)(len - PROBE_LEN));
            }
        } else if (in && ((intf == 0 && sel == VC_REQUEST_ERROR) || (intf == 1 && sel == VS_STREAM_ERROR))) {
            if (req == GET_CUR) tmp[0] = (uint8_t)prev_err, len = 1; /* the request before this one's */
            else if (req == GET_INFO) tmp[0] = 0x01, len = 1;
            else return v->req_err = ERR_INVALID_REQUEST, -1;
        } else {
            v->req_err = ERR_INVALID_CONTROL;
            return -1;
        }
    } else if ((type == 0x21 || type == 0xa1) && (index & 0xff) == 0) { /* a unit's or terminal's controls: none */
        v->req_err = ERR_INVALID_CONTROL;
        return -1;
    } else {
        if (v->m && v->m->log) fprintf(stderr, "usb-video: unhandled request %02x %02x value %04x index %04x\n", type, req, value, index);
        return -1;
    }
    if (len > max) len = max;
    memcpy(out, src, (size_t)len);
    return len;
}

int hv_usb_video_configured(HvUsbVideo *v) {
    return v->configured;
}

void hv_usb_video_reset(HvUsbVideo *v) {
    stop_stream(v);
    v->configured = 0;
    v->committed = 0;
    v->held_until = 0, v->hold = 0;
    memset(v->probe, 0, PROBE_LEN);
    probe_fill(v->probe, 1, 1);
    memcpy(v->commit, v->probe, PROBE_LEN);
}

void hv_usb_video_abort(HvUsbVideo *v) {
    stop_stream(v);
    v->hold = 1;
}

int hv_usb_video_streaming(HvUsbVideo *v) {
    return v->active;
}

void hv_usb_video_tick(HvUsbVideo *v, int td_waiting, uint64_t now) {
    if (v->hold) v->hold = 0, v->held_until = now + ABORT_GRACE_NS;
    if (td_waiting && v->committed) {
        if (!v->active && now >= v->held_until) tell(v, 1);
        if (v->active) v->last_td = now;
    } else if (v->active && now - v->last_td > IDLE_NS) {
        stop_stream(v);
    }
}

int hv_usb_video_td(HvUsbVideo *v, const HvUsbSeg *seg, int nseg, uint32_t len, uint64_t now) {
    if (!v->committed || !v->buf[0]) return HV_USB_NAK;
    if (!v->in_payload) {
        if (v->next_due && now < v->next_due) return HV_USB_NAK;
        start_frame(v, now);
    }
    uint32_t total = 2 + frame_bytes(v->tfmt, v->tw, v->th);
    uint8_t hdr[2] = {2, (uint8_t)(0x80 | 0x02 | (v->fid & 1))}; /* EOH, EOF (a frame per payload), FID */
    const uint8_t *frame = v->buf[v->tx];
    uint32_t given = 0;
    for (int i = 0; i < nseg && v->pos < total; i++) {
        uint32_t k = seg[i].len, at = 0;
        if (k > total - v->pos) k = total - v->pos;
        uint8_t *dst = seg[i].p;
        if (v->m && seg[i].pa && k) hv_guard_claim_range(v->m, seg[i].pa, k); /* an auto-save's guard: before the bytes change */
        while (at < k) {
            uint32_t n;
            if (v->pos < 2) {
                n = 2 - v->pos < k - at ? 2 - v->pos : k - at;
                memcpy(dst + at, hdr + v->pos, n);
            } else {
                n = k - at;
                memcpy(dst + at, frame + (v->pos - 2), n);
            }
            at += n, v->pos += n;
        }
        if (v->m && seg[i].pa && k) hv_dma_written(v->m, seg[i].pa, k);
        given += k;
    }
    (void)len;
    if (v->pos >= total) v->in_payload = 0, v->pos = 0;
    return (int)given;
}

/* ---- frames from the camera ---- */

typedef struct Src {
    int fmt, w, h;
    const uint8_t *p0, *p1;
    size_t s0, s1;
} Src;

static inline uint8_t clamp8(int x) {
    return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x);
}

/* BT.601, video range: what NV12 from a camera is, and what the colour matching descriptor says. */
static inline void bgra_yuv(const uint8_t *px, int *y, int *u, int *vv) {
    int b = px[0], g = px[1], r = px[2];
    *y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
    *u = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
    *vv = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
}

static inline uint8_t src_y(const Src *s, int x, int y) {
    if (s->fmt == HV_USB_VIDEO_NV12) return s->p0[(size_t)y * s->s0 + (size_t)x];
    int yy, u, v;
    bgra_yuv(s->p0 + (size_t)y * s->s0 + 4 * (size_t)x, &yy, &u, &v);
    return clamp8(yy);
}

static inline void src_uv(const Src *s, int x, int y, uint8_t *u, uint8_t *v) {
    if (s->fmt == HV_USB_VIDEO_NV12) {
        const uint8_t *c = s->p1 + (size_t)(y / 2) * s->s1 + (size_t)(x / 2) * 2;
        *u = c[0], *v = c[1];
        return;
    }
    int yy, uu, vv;
    bgra_yuv(s->p0 + (size_t)y * s->s0 + 4 * (size_t)x, &yy, &uu, &vv);
    *u = clamp8(uu), *v = clamp8(vv);
}

/* Crop the source to the target's shape (centred) and scale it, sampling each target pixel's centre. */
static void convert(const Src *s, uint8_t *dst, int fmt, int tw, int th) {
    static __thread int xm[1920], ym[1080];
    int cw = s->w, ch = s->h;
    if ((int64_t)s->w * th > (int64_t)s->h * tw) cw = (int)((int64_t)s->h * tw / th);
    else ch = (int)((int64_t)s->w * th / tw);
    int ox = (s->w - cw) / 2, oy = (s->h - ch) / 2;
    for (int x = 0; x < tw; x++) xm[x] = ox + (int)(((int64_t)2 * x + 1) * cw / (2 * tw));
    for (int y = 0; y < th; y++) ym[y] = oy + (int)(((int64_t)2 * y + 1) * ch / (2 * th));
    if (fmt == HV_USB_VIDEO_FMT_NV12) {
        for (int y = 0; y < th; y++) {
            uint8_t *row = dst + (size_t)y * (size_t)tw;
            for (int x = 0; x < tw; x++) row[x] = src_y(s, xm[x], ym[y]);
        }
        uint8_t *uv = dst + (size_t)tw * (size_t)th;
        for (int y = 0; y < th / 2; y++)
            for (int x = 0; x < tw / 2; x++) src_uv(s, xm[2 * x], ym[2 * y], &uv[(size_t)y * (size_t)tw + 2 * (size_t)x], &uv[(size_t)y * (size_t)tw + 2 * (size_t)x + 1]);
    } else {
        for (int y = 0; y < th; y++) {
            uint8_t *row = dst + (size_t)y * (size_t)tw * 2;
            int sy = ym[y];
            for (int x = 0; x < tw; x += 2) {
                uint8_t u, v;
                src_uv(s, xm[x], sy, &u, &v);
                row[2 * x] = src_y(s, xm[x], sy), row[2 * x + 1] = u;
                row[2 * x + 2] = src_y(s, xm[x + 1], sy), row[2 * x + 3] = v;
            }
        }
    }
}

int hv_usb_video_frame(HvUsbVideo *v, int fmt, int width, int height, const uint8_t *plane0, size_t stride0,
                       const uint8_t *plane1, size_t stride1) {
    if (!v || width < 2 || height < 2 || width > 8192 || height > 8192 || !plane0) return -1;
    if (fmt == HV_USB_VIDEO_NV12 ? (!plane1 || stride0 < (size_t)width || stride1 < (size_t)(width & ~1)) :
        fmt == HV_USB_VIDEO_BGRA ? stride0 < 4 * (size_t)width : 1)
        return -1;
    Src s = {fmt, width & ~1, height & ~1, plane0, plane1, stride0, stride1};
    pthread_mutex_lock(&v->pmu);
    pthread_mutex_lock(&v->mu);
    uint32_t gen = v->gen;
    int tw = v->tw, th = v->th, tfmt = v->tfmt;
    uint8_t *dst = v->buf[0] ? v->buf[v->back] : NULL;
    pthread_mutex_unlock(&v->mu);
    if (dst && tw) {
        convert(&s, dst, tfmt, tw, th);
        pthread_mutex_lock(&v->mu);
        if (v->gen == gen) {
            int t = v->back;
            v->back = v->ready, v->ready = t;
            v->ready_valid = 1;
            v->ready_gen = gen;
        }
        pthread_mutex_unlock(&v->mu);
    }
    pthread_mutex_unlock(&v->pmu);
    return 0;
}

/* ---- life ---- */

HvUsbVideo *hv_usb_video_create(HvMachine *m, const char *name) {
    HvUsbVideo *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    v->m = m;
    snprintf(v->name, sizeof v->name, "%s", name && *name ? name : "Camera");
    pthread_mutex_init(&v->mu, NULL);
    pthread_mutex_init(&v->pmu, NULL);
    v->back = 0, v->ready = 1, v->tx = 2;
    build_descriptors(v);
    hv_usb_video_reset(v);
    return v;
}

void hv_usb_video_free(HvUsbVideo *v) {
    if (!v) return;
    for (int k = 0; k < 3; k++) free(v->buf[k]);
    pthread_mutex_destroy(&v->mu);
    pthread_mutex_destroy(&v->pmu);
    free(v);
}

const char *hv_usb_video_name(HvUsbVideo *v) {
    return v->name;
}

void hv_usb_video_set_callback(HvUsbVideo *v, HvUsbVideoStream on_stream, void *opaque) {
    v->on_stream = on_stream;
    v->opaque = opaque;
}

void hv_usb_video_get_state(HvUsbVideo *v, HvUsbVideoState *st) {
    memset(st, 0, sizeof *st);
    st->configured = v->configured;
    st->committed = v->committed;
    memcpy(st->probe, v->probe, PROBE_LEN);
    memcpy(st->commit, v->commit, PROBE_LEN);
    st->fid = v->fid;
}

int hv_usb_video_set_state(HvUsbVideo *v, const HvUsbVideoState *st) {
    if (st->configured < 0 || st->configured > 1 || st->committed < 0 || st->committed > 1) return -1;
    if (!probe_valid(st->probe) || (st->committed && !probe_valid(st->commit))) return -1;
    hv_usb_video_reset(v);
    v->configured = st->configured;
    memcpy(v->probe, st->probe, PROBE_LEN);
    memcpy(v->commit, st->commit, PROBE_LEN);
    if (!probe_valid(v->commit)) probe_fill(v->commit, 1, 1);
    /* What the negotiated fields say is recomputed, so a size can't be anything but this camera's. */
    probe_fill(v->probe, v->probe[2], v->probe[3]);
    probe_fill(v->commit, v->commit[2], v->commit[3]);
    v->committed = st->committed;
    v->fid = st->fid & 1;
    if (v->committed) set_target(v);
    return 0;
}
