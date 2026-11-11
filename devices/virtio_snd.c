#include "virtio_snd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CTLQ 0
#define EVENTQ 1
#define TXQ 2
#define RXQ 3

/* request codes */
#define R_JACK_INFO 1
#define R_JACK_REMAP 2
#define R_PCM_INFO 0x0100
#define R_PCM_SET_PARAMS 0x0101
#define R_PCM_PREPARE 0x0102
#define R_PCM_RELEASE 0x0103
#define R_PCM_START 0x0104
#define R_PCM_STOP 0x0105
#define R_CHMAP_INFO 0x0200
/* status codes */
#define S_OK 0x8000
#define S_BAD_MSG 0x8001
#define S_NOT_SUPP 0x8002

#define CHMAP_FL 3
#define CHMAP_FR 4

static int logging(void) {
    static int on = -1;
    if (on < 0) on = getenv("HV_SND_LOG") != NULL;
    return on;
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static unsigned format_bytes(unsigned f) {
    return f == HV_VSND_FMT_S16 ? 2 : 4;
}

static uint32_t readable(const HvVirtqElem *e) {
    uint32_t n = 0;
    for (int i = 0; i < e->nseg; i++)
        if (!e->seg[i].write) n += e->seg[i].len;
    return n;
}

static uint32_t writable(const HvVirtqElem *e) {
    uint32_t n = 0;
    for (int i = 0; i < e->nseg; i++)
        if (e->seg[i].write) n += e->seg[i].len;
    return n;
}

static void put32(uint8_t *p, uint32_t v) {
    memcpy(p, &v, 4);
}

static uint32_t cfg32(const HvVirtioSnd *d, int at) {
    uint32_t n;
    memcpy(&n, d->vio.config + at, 4);
    return n;
}

/* What the guest was told (a snapshot's config may say one stream: a Mac from before the microphone). */
int hv_virtio_snd_streams(const HvVirtioSnd *d) {
    return (int)cfg32(d, 4);
}

static int params_ok(unsigned ch, unsigned fmt, unsigned rate) {
    return ch >= 1 && ch <= 2 && (fmt == HV_VSND_FMT_S16 || fmt == HV_VSND_FMT_S32 || fmt == HV_VSND_FMT_FLOAT) &&
           (rate == HV_VSND_RATE_44100 || rate == HV_VSND_RATE_48000);
}

/* ---- the input stream ---- */

#define IN_STREAM 1
#define IN_CUSHION_MS 60  /* audio kept in hand against the host's capture arriving in lumps */
#define IN_MAX_MS 160     /* more than this waiting (the host started late, or its clock runs fast): drop to the cushion */

static uint64_t ms_bytes(const HvVirtioSndIn *c, unsigned ms) {
    return (uint64_t)c->rate_hz * ms / 1000 * c->frame_bytes;
}

/* Bytes of audio the clock says are due since start_ns. */
static uint64_t in_due(const HvVirtioSndIn *c, uint64_t now) {
    if (now <= c->start_ns) return 0;
    uint64_t dt = now - c->start_ns;
    uint64_t frames = dt / 1000000000ull * c->rate_hz + dt % 1000000000ull * c->rate_hz / 1000000000ull;
    return frames * c->frame_bytes;
}

static void in_reset_ring(HvVirtioSndIn *c) {
    atomic_store(&c->read, atomic_load(&c->written));
    c->primed = 0;
}

static void in_start_capture(HvVirtioSnd *d) {
    HvVirtioSndIn *c = &d->in;
    in_reset_ring(c);
    c->capturing = d->backend.cap_start &&
                   d->backend.cap_start(d->backend.opaque, c->channels, c->format, c->rate_hz) == 0;
}

static void in_stop_capture(HvVirtioSnd *d) {
    HvVirtioSndIn *c = &d->in;
    if (c->capturing && d->backend.cap_stop) d->backend.cap_stop(d->backend.opaque);
    c->capturing = 0;
}

void hv_virtio_snd_capture(HvVirtioSnd *d, const void *pcm, uint32_t len) {
    HvVirtioSndIn *c = &d->in;
    unsigned fb = c->frame_bytes;
    if (!c->running || !fb || !c->ring) return;
    len -= len % fb;
    uint64_t w = atomic_load_explicit(&c->written, memory_order_relaxed);
    uint64_t r = atomic_load_explicit(&c->read, memory_order_acquire);
    if (w - r + len > HV_VSND_RING) {
        c->overruns++;
        return;
    }
    for (uint32_t off = 0; off < len;) {
        uint32_t at = (uint32_t)((w + off) % HV_VSND_RING), n = len - off;
        if (n > HV_VSND_RING - at) n = HV_VSND_RING - at;
        memcpy(c->ring + at, (const uint8_t *)pcm + off, n);
        off += n;
    }
    atomic_store_explicit(&c->written, w + len, memory_order_release);
}

/* Give the guest buffer e (n bytes of data, then the status) its audio: from the ring once it holds a
   cushion, silence otherwise. */
static void in_fill(HvVirtioSnd *d, const HvVirtqElem *e, uint32_t n) {
    HvVirtioSndIn *c = &d->in;
    HvVirtio *v = &d->vio;
    uint64_t w = atomic_load_explicit(&c->written, memory_order_acquire), r = atomic_load(&c->read);
    uint64_t queued = w - r, cushion = ms_bytes(c, IN_CUSHION_MS);
    if (queued > ms_bytes(c, IN_MAX_MS) + n) { /* too far behind the microphone: catch up */
        uint64_t drop = queued - cushion - n;
        drop -= drop % c->frame_bytes;
        r += drop, queued -= drop;
    }
    if (!c->primed && queued >= cushion + n) c->primed = 1;
    uint32_t k = 0;
    if (c->primed) {
        k = queued < n ? (uint32_t)queued : n;
        k -= k % c->frame_bytes;
        for (uint32_t off = 0; off < k;) {
            uint32_t at = (uint32_t)((r + off) % HV_VSND_RING), m = k - off;
            if (m > HV_VSND_RING - at) m = HV_VSND_RING - at;
            hv_virtq_write(v, e, off, c->ring + at, m);
            off += m;
        }
        r += k;
        if (k < n) c->underruns++, c->primed = 0; /* ran dry: build the cushion up again */
    }
    atomic_store_explicit(&c->read, r, memory_order_release);
    static const uint8_t zero[4096];
    for (uint32_t off = k; off < n;) {
        uint32_t m = n - off < sizeof zero ? n - off : (uint32_t)sizeof zero;
        hv_virtq_write(v, e, off, zero, m);
        off += m;
    }
}

/* Hand a capture buffer back: its data (silence unless filled), then the status. */
static void in_return(HvVirtioSnd *d, const HvVirtqElem *e, uint32_t status, int fill) {
    HvVirtio *v = &d->vio;
    uint32_t wl = writable(e);
    if (wl < 8) {
        hv_virtq_push(v, RXQ, e, 0);
        return;
    }
    uint32_t n = wl - 8;
    if (fill) in_fill(d, e, n);
    else {
        static const uint8_t zero[4096];
        for (uint32_t off = 0; off < n;) {
            uint32_t m = n - off < sizeof zero ? n - off : (uint32_t)sizeof zero;
            hv_virtq_write(v, e, off, zero, m);
            off += m;
        }
    }
    uint8_t st[8] = {0};
    put32(st, status);
    uint64_t q = atomic_load(&d->in.written) - atomic_load(&d->in.read);
    put32(st + 4, (uint32_t)(q > 0xffffffffu ? 0xffffffffu : q)); /* latency_bytes */
    hv_virtq_write(v, e, n, st, 8);
    hv_virtq_push(v, RXQ, e, wl);
}

/* Every buffer held goes back (silence): the stream was released, or the device reset. */
static int in_return_all(HvVirtioSnd *d) {
    HvVirtioSndIn *c = &d->in;
    int any = c->phead != c->ptail;
    for (; c->phead != c->ptail; c->phead++) in_return(d, &c->pending[c->phead % HV_VSND_PENDING], S_OK, 0);
    return any;
}

/* Take the guest's capture buffers: held while the stream is prepared, refused otherwise. */
static int take_rx(HvVirtioSnd *d) {
    HvVirtio *v = &d->vio;
    HvVirtioSndIn *c = &d->in;
    if (!(v->status & 4) || !v->q[RXQ].enabled) return 0;
    int any = 0;
    static HvVirtqElem e;
    while (c->ptail - c->phead < HV_VSND_PENDING) {
        if (hv_virtq_pop(v, RXQ, &e) <= 0) break;
        uint32_t stream = ~0u;
        if (readable(&e) >= 4) hv_virtq_read(v, &e, 0, &stream, 4);
        if (stream != IN_STREAM || hv_virtio_snd_streams(d) <= IN_STREAM || !c->prepared || writable(&e) < 8 + c->frame_bytes) {
            in_return(d, &e, S_BAD_MSG, 0);
            any = 1;
            continue;
        }
        c->pending[c->ptail++ % HV_VSND_PENDING] = e;
    }
    return any;
}

/* Each tick: return every held buffer whose audio is due by the clock. */
static void capture_poll(HvVirtioSnd *d) {
    HvVirtioSndIn *c = &d->in;
    if (!(d->vio.status & 4)) { /* the driver reset the device: its buffers are gone, and the microphone goes off */
        in_stop_capture(d);
        c->running = c->prepared = 0;
        c->phead = c->ptail;
        return;
    }
    int any = take_rx(d);
    if (c->running && c->frame_bytes) {
        uint64_t due = in_due(c, now_ns());
        while (c->phead != c->ptail) {
            HvVirtqElem *e = &c->pending[c->phead % HV_VSND_PENDING];
            uint32_t n = writable(e) - 8;
            if (c->delivered + n > due) break;
            in_return(d, e, S_OK, 1);
            c->phead++;
            c->delivered += n;
            c->xfers++;
            any = 1;
        }
        /* The guest gave no buffers for a while: don't send what it missed in a rush. */
        uint64_t slack = c->buffer_bytes > ms_bytes(c, 100) ? c->buffer_bytes : ms_bytes(c, 100);
        if (due > c->delivered + slack) c->delivered = due - slack;
        if (any) any |= take_rx(d);
    }
    if (any) hv_virtio_interrupt(&d->vio);
}

void hv_virtio_snd_capture_restart(HvVirtioSnd *d) {
    if (!d || !d->in.restart) return;
    HvVirtioSndIn *c = &d->in;
    c->restart = 0;
    c->start_ns = now_ns();
    c->delivered = 0;
    if (c->running) in_start_capture(d);
}

/* ---- control queue ---- */

/* One item of a JACK_INFO, PCM_INFO or CHMAP_INFO reply (size bytes, zero past what the spec defines). */
static void info_item(HvVirtioSnd *d, uint32_t code, uint32_t id, uint8_t *it) {
    (void)d;
    int input = id == IN_STREAM; /* jack 1, stream 1 and channel map 1 are the microphone's */
    if (code == R_PCM_INFO) {
        uint64_t formats = 1ull << HV_VSND_FMT_S16 | 1ull << HV_VSND_FMT_S32 | 1ull << HV_VSND_FMT_FLOAT;
        uint64_t rates = 1ull << HV_VSND_RATE_44100 | 1ull << HV_VSND_RATE_48000;
        memcpy(it + 8, &formats, 8);
        memcpy(it + 16, &rates, 8);
        it[24] = (uint8_t)input; /* direction: 0 output, 1 input */
        it[25] = 1;              /* channels min */
        it[26] = 2;              /* channels max */
    } else if (code == R_CHMAP_INFO) {
        it[4] = (uint8_t)input;
        it[5] = 2;
        it[6] = CHMAP_FL;
        it[7] = CHMAP_FR;
    } else { /* R_JACK_INFO: always connected */
        /* HDA pin default config: line out, rear, green jack; or a microphone in, rear, pink jack */
        put32(it + 8, input ? 0x01A19020 : 0x01014010);
        put32(it + 12, input ? 0x20 : 0x10); /* pin capabilities: input or output */
        it[16] = 1;
    }
}

static uint32_t control(HvVirtioSnd *d, const HvVirtqElem *e) {
    HvVirtio *v = &d->vio;
    HvVirtioSndIn *c = &d->in;
    uint8_t req[32] = {0}, resp[4 + 2 * 64];
    uint32_t got = hv_virtq_read(v, e, 0, req, sizeof req);
    uint32_t code, status = S_OK, rlen = 4;
    memcpy(&code, req, 4);
    memset(resp, 0, sizeof resp);
    uint32_t start, count, size, stream;
    memcpy(&start, req + 4, 4);
    memcpy(&count, req + 8, 4);
    memcpy(&size, req + 12, 4);
    stream = start; /* virtio_snd_pcm_hdr: stream_id at the same place */
    if (got < 4) code = 0;
    uint32_t nstreams = cfg32(d, 4);
    int in = stream == IN_STREAM && nstreams > IN_STREAM; /* a request for the microphone's stream */
    int out = stream == 0;
    switch (code) {
    case R_JACK_INFO:
    case R_PCM_INFO:
    case R_CHMAP_INFO: {
        uint32_t n = cfg32(d, code == R_JACK_INFO ? 0 : code == R_PCM_INFO ? 4 : 8);
        if (start >= n || count == 0 || count > n - start || size == 0 || size > 64 || count * size > sizeof resp - 4) {
            status = S_BAD_MSG;
            break;
        }
        for (uint32_t i = 0; i < count; i++) {
            uint8_t it[64] = {0};
            info_item(d, code, start + i, it);
            memcpy(resp + 4 + i * size, it, size);
        }
        rlen = 4 + count * size;
        break;
    }
    case R_JACK_REMAP:
        break;
    case R_PCM_SET_PARAMS: {
        uint32_t buffer_bytes, period_bytes;
        memcpy(&buffer_bytes, req + 8, 4);
        memcpy(&period_bytes, req + 12, 4);
        unsigned ch = req[20], fmt = req[21], rate = req[22];
        if (!(in || out) || !params_ok(ch, fmt, rate) || (out ? d->running : c->running)) {
            status = S_NOT_SUPP;
            break;
        }
        unsigned rate_hz = rate == HV_VSND_RATE_44100 ? 44100 : 48000, fb = ch * format_bytes(fmt);
        if (out) {
            d->channels = ch, d->format = fmt, d->rate = rate, d->rate_hz = rate_hz, d->frame_bytes = fb;
            d->buffer_bytes = buffer_bytes, d->period_bytes = period_bytes;
        } else {
            c->channels = ch, c->format = fmt, c->rate = rate, c->rate_hz = rate_hz, c->frame_bytes = fb;
            c->buffer_bytes = buffer_bytes, c->period_bytes = period_bytes;
        }
        break;
    }
    case R_PCM_PREPARE:
        if (out && d->frame_bytes) d->prepared = 1;
        else if (in && c->frame_bytes) {
            if (!c->ring) c->ring = calloc(1, HV_VSND_RING);
            c->prepared = c->ring != NULL;
            if (!c->prepared) status = S_BAD_MSG;
        } else status = S_BAD_MSG;
        break;
    case R_PCM_START:
        if (in) {
            if (!c->prepared) {
                status = S_BAD_MSG;
                break;
            }
            if (!c->running) {
                c->running = 1;
                c->start_ns = now_ns();
                c->delivered = 0;
                in_start_capture(d);
            }
            break;
        }
        if (!out || !d->prepared) {
            status = S_BAD_MSG;
            break;
        }
        if (!d->running) {
            d->running = 1;
            d->clock_start_ns = now_ns();
            d->clock_start_consumed = atomic_load(&d->consumed);
            if (d->backend.start && d->backend.start(d->backend.opaque, d->channels, d->format, d->rate_hz) != 0)
                d->backend.start = NULL; /* the host cannot play it: drain by the clock instead */
        }
        break;
    case R_PCM_STOP:
        if (in) {
            in_stop_capture(d);
            c->running = 0;
            break;
        }
        if (!out) {
            status = S_BAD_MSG;
            break;
        }
        if (d->running && d->backend.stop) d->backend.stop(d->backend.opaque);
        d->running = 0;
        break;
    case R_PCM_RELEASE:
        if (in) {
            in_stop_capture(d);
            c->running = 0;
            c->prepared = 0;
            if (in_return_all(d)) hv_virtio_interrupt(v); /* every buffer held goes back (the spec requires it) */
            break;
        }
        if (!out) {
            status = S_BAD_MSG;
            break;
        }
        if (d->running && d->backend.stop) d->backend.stop(d->backend.opaque);
        d->running = 0;
        d->prepared = 0;
        /* Every buffer still held goes back now (the spec requires it). */
        for (; d->pcopy != d->ptail; d->pcopy++) d->pending_end[d->pcopy % HV_VSND_PENDING] = atomic_load(&d->written);
        atomic_store(&d->consumed, atomic_load(&d->written));
        break;
    default:
        status = S_NOT_SUPP;
        break;
    }
    if (logging())
        fprintf(stderr, "virtio-snd: request %#x [%u %u %u] -> %#x (ch %u fmt %u rate %u buffer %u period %u)\n", code, start, count, size, status,
                in ? c->channels : d->channels, in ? c->format : d->format, in ? c->rate_hz : d->rate_hz,
                in ? c->buffer_bytes : d->buffer_bytes, in ? c->period_bytes : d->period_bytes);
    put32(resp, status);
    return hv_virtq_write(v, e, 0, resp, rlen);
}

/* ---- transmit ---- */

/* Copy waiting buffers' audio into the ring while there is room. Their data is
   still in guest memory, so copying can wait. */

static void fill_ring(HvVirtioSnd *d) {
    HvVirtio *v = &d->vio;
    while (d->pcopy != d->ptail) {
        HvVirtqElem *e = &d->pending[d->pcopy % HV_VSND_PENDING];
        uint32_t len = readable(e);
        len = len > 4 ? len - 4 : 0;
        uint64_t w = atomic_load(&d->written);
        if (len > HV_VSND_RING) len = HV_VSND_RING;
        if (w + len - atomic_load(&d->consumed) > HV_VSND_RING) return; /* full: later */
        uint32_t off = 0;
        while (off < len) {
            uint32_t at = (uint32_t)((w + off) % HV_VSND_RING), n = len - off;
            if (n > HV_VSND_RING - at) n = HV_VSND_RING - at;
            hv_virtq_read(v, e, 4 + off, d->ring + at, n);
            off += n;
        }
        atomic_store_explicit(&d->written, w + len, memory_order_release);
        d->pending_end[d->pcopy % HV_VSND_PENDING] = w + len;
        d->pcopy++;
        d->xfers++;
    }
}

static void take_tx(HvVirtioSnd *d) {
    HvVirtio *v = &d->vio;
    if (!(v->status & 4) || !v->q[TXQ].enabled) return;
    while (d->ptail - d->phead < HV_VSND_PENDING) {
        if (hv_virtq_pop(v, TXQ, &d->pending[d->ptail % HV_VSND_PENDING]) <= 0) break;
        d->ptail++;
    }
    fill_ring(d);
}

void hv_virtio_snd_poll(HvVirtioSnd *d) {
    if (!d) return;
    HvVirtio *v = &d->vio;
    capture_poll(d);
    /* No backend: the stream plays at its rate by the wall clock. */
    if (d->running && !d->backend.start && d->frame_bytes) {
        uint64_t now = now_ns();
        uint64_t target = d->clock_start_consumed +
                          (now - d->clock_start_ns) * d->rate_hz / 1000000000ull * d->frame_bytes;
        uint64_t w = atomic_load(&d->written);
        if (target > w) { /* ran dry: restart the clock from here */
            target = w;
            d->clock_start_ns = now;
            d->clock_start_consumed = w;
        }
        if (target > atomic_load(&d->consumed)) atomic_store(&d->consumed, target);
    }
    take_tx(d);
    uint64_t c = atomic_load_explicit(&d->consumed, memory_order_acquire);
    int any = 0;
    while (d->phead != d->pcopy && d->pending_end[d->phead % HV_VSND_PENDING] <= c) {
        HvVirtqElem *e = &d->pending[d->phead % HV_VSND_PENDING];
        uint8_t st[8] = {0};
        put32(st, S_OK);
        uint64_t queued = atomic_load(&d->written) - c;
        put32(st + 4, (uint32_t)queued); /* latency_bytes */
        hv_virtq_write(v, e, 0, st, 8);
        hv_virtq_push(v, TXQ, e, 8);
        d->phead++;
        any = 1;
    }
    if (any) {
        hv_virtio_interrupt(v);
        take_tx(d); /* room for more */
    }
}

uint32_t hv_virtio_snd_pull(HvVirtioSnd *d, void *buf, uint32_t len) {
    uint64_t c = atomic_load_explicit(&d->consumed, memory_order_relaxed);
    uint64_t w = atomic_load_explicit(&d->written, memory_order_acquire);
    uint32_t n = w - c < len ? (uint32_t)(w - c) : len;
    uint32_t off = 0;
    while (off < n) {
        uint32_t at = (uint32_t)((c + off) % HV_VSND_RING), k = n - off;
        if (k > HV_VSND_RING - at) k = HV_VSND_RING - at;
        memcpy((uint8_t *)buf + off, d->ring + at, k);
        off += k;
    }
    if (n < len) {
        memset((uint8_t *)buf + n, 0, len - n);
        if (d->running) d->underruns++;
    }
    /* compare-exchange: a release on the CPU thread may have moved it on */
    atomic_compare_exchange_strong(&d->consumed, &c, c + n);
    if (n) hv_machine_wake(d->vio.m);
    return n;
}

static void notify(HvVirtio *v, int q) {
    HvVirtioSnd *d = v->dev;
    static HvVirtqElem e;
    if (q == CTLQ) {
        int any = 0;
        while (hv_virtq_pop(v, q, &e) > 0) {
            hv_virtq_push(v, q, &e, control(d, &e));
            any = 1;
        }
        if (any) hv_virtio_interrupt(v);
        hv_virtio_snd_poll(d); /* a release may have freed buffers */
    } else if (q == TXQ) {
        take_tx(d);
    } else if (q == RXQ) {
        capture_poll(d);
    } /* event queue: kept for events we never send */
}

HvVirtioSnd *hv_virtio_snd_create(HvMachine *m, HvPcie *bus) {
    HvVirtioSnd *d = calloc(1, sizeof *d);
    HvVirtio *v = &d->vio;
    hv_virtio_init(v, m, 25, 0x040100, 4, 0); /* sound device, multimedia audio */
    v->dev = d;
    v->notify = notify;
    uint32_t jacks = 2, streams = 2, chmaps = 2; /* output and the microphone */
    if (getenv("HV_NO_MIC")) jacks = streams = chmaps = 1;
    memcpy(v->config, &jacks, 4);
    memcpy(v->config + 4, &streams, 4);
    memcpy(v->config + 8, &chmaps, 4);
    v->config_len = 12;
    d->ring = calloc(1, HV_VSND_RING);
    hv_pcie_attach(bus, &v->pci);
    return d;
}
