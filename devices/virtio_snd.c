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

#define CHMAP_MONO 2
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

static void put32(uint8_t *p, uint32_t v) {
    memcpy(p, &v, 4);
}

/* ---- the input stream (HV_MIC=1) ---- */

/* Hand a capture buffer back: `bytes` of PCM at the start of its writable part, the status in its last 8 bytes. */
static void give_rx(HvVirtioSnd *d, const HvVirtqElem *e, uint32_t status, uint32_t bytes, uint32_t latency) {
    HvVirtio *v = &d->vio;
    uint32_t w = hv_virtq_writable(e);
    uint8_t st[8];
    put32(st, status);
    put32(st + 4, latency);
    if (w >= 8) hv_virtq_write(v, e, w - 8, st, 8);
    hv_virtq_push(v, RXQ, e, w >= 8 ? bytes + 8 : 0);
}

static void put_zero(HvVirtio *v, const HvVirtqElem *e, uint32_t off, uint32_t len) {
    static const uint8_t zero[1024];
    while (len) {
        uint32_t n = len > sizeof zero ? (uint32_t)sizeof zero : len;
        hv_virtq_write(v, e, off, zero, n);
        off += n;
        len -= n;
    }
}

/* Fill waiting capture buffers, oldest first, while the microphone has delivered enough for a whole one. */
static int fill_rx(HvVirtioSnd *d) {
    HvVirtio *v = &d->vio;
    int any = 0;
    while (d->in_running && d->in_head != d->in_tail) {
        HvVirtqElem *e = &d->in_pending[d->in_head % HV_VSND_PENDING];
        uint32_t w = hv_virtq_writable(e), cap = w > 8 ? w - 8 : 0;
        if (d->in_frame_bytes) cap -= cap % d->in_frame_bytes;
        uint32_t need = cap > HV_VSND_IN_RING / 2 ? HV_VSND_IN_RING / 2 : cap; /* a buffer the ring cannot hold: the rest is silence */
        uint64_t c = atomic_load_explicit(&d->in_consumed, memory_order_relaxed);
        uint64_t wr = atomic_load_explicit(&d->in_written, memory_order_acquire);
        if (wr - c < need) break;
        if (d->in_live) {
            uint32_t off = 0;
            while (off < need) {
                uint32_t at = (uint32_t)((c + off) % HV_VSND_IN_RING), n = need - off;
                if (n > HV_VSND_IN_RING - at) n = HV_VSND_IN_RING - at;
                hv_virtq_write(v, e, off, d->in_ring + at, n);
                off += n;
            }
            put_zero(v, e, need, cap - need);
        } else {
            put_zero(v, e, 0, cap);
        }
        atomic_store_explicit(&d->in_consumed, c + need, memory_order_release);
        give_rx(d, e, S_OK, cap, (uint32_t)(wr - c - need));
        d->in_head++;
        d->in_xfers++;
        any = 1;
    }
    return any;
}

/* Take the capture buffers the guest has offered. Returns 1 if any went back (the caller interrupts). */
static int take_rx(HvVirtioSnd *d) {
    HvVirtio *v = &d->vio;
    int any = 0;
    if (!(v->status & 4) || !v->q[RXQ].enabled) return 0;
    while (d->in_tail - d->in_head < HV_VSND_PENDING) {
        HvVirtqElem *e = &d->in_pending[d->in_tail % HV_VSND_PENDING];
        if (hv_virtq_pop(v, RXQ, e) <= 0) break;
        uint32_t stream = ~0u;
        hv_virtq_read(v, e, 0, &stream, 4);
        if (stream != 1 || !d->in_prepared) {
            give_rx(d, e, S_BAD_MSG, 0, 0);
            any = 1;
            continue;
        }
        d->in_tail++;
    }
    return any | fill_rx(d);
}

static void in_halt(HvVirtioSnd *d) {
    if (d->in_running && d->in_live && d->backend.in_stop) d->backend.in_stop(d->backend.opaque);
    d->in_live = 0;
    d->in_running = 0;
}

/* The input stream's PCM requests. */
static uint32_t control_in(HvVirtioSnd *d, uint32_t code, const uint8_t *req) {
    switch (code) {
    case R_PCM_SET_PARAMS: {
        unsigned ch = req[20], fmt = req[21], rate = req[22];
        if (ch != (unsigned)d->mic || (fmt != HV_VSND_FMT_S16 && fmt != HV_VSND_FMT_S32 && fmt != HV_VSND_FMT_FLOAT) ||
            (rate != HV_VSND_RATE_44100 && rate != HV_VSND_RATE_48000) || d->in_running)
            return S_NOT_SUPP;
        d->in_channels = ch;
        d->in_format = fmt;
        d->in_rate_hz = rate == HV_VSND_RATE_44100 ? 44100 : 48000;
        d->in_frame_bytes = ch * format_bytes(fmt);
        return S_OK;
    }
    case R_PCM_PREPARE:
        if (!d->in_frame_bytes) return S_BAD_MSG;
        d->in_prepared = 1;
        return S_OK;
    case R_PCM_START:
        if (!d->in_prepared) return S_BAD_MSG;
        if (!d->in_running) {
            /* Nothing is recording yet, so both ends of the ring are this thread's: start it empty. */
            atomic_store(&d->in_consumed, atomic_load(&d->in_written));
            d->in_live = d->backend.in_start &&
                         d->backend.in_start(d->backend.opaque, d->in_channels, d->in_format, d->in_rate_hz) == 0;
            d->in_clock_ns = now_ns();
            d->in_running = 1;
            fprintf(stderr, "hengevm: sound: microphone %s (%u ch, format %u, %u Hz)\n",
                    d->in_live ? "recording" : "not available, the Mac hears silence", d->in_channels, d->in_format, d->in_rate_hz);
        }
        return S_OK;
    case R_PCM_STOP:
        in_halt(d);
        return S_OK;
    default: /* R_PCM_RELEASE: every buffer still held goes back now (the spec requires it) */
        in_halt(d);
        d->in_prepared = 0;
        for (; d->in_head != d->in_tail; d->in_head++) give_rx(d, &d->in_pending[d->in_head % HV_VSND_PENDING], S_OK, 0, 0);
        return S_OK;
    }
}

/* ---- control queue ---- */

static uint32_t control(HvVirtioSnd *d, const HvVirtqElem *e) {
    HvVirtio *v = &d->vio;
    uint8_t req[32] = {0}, resp[4 + 2 * 32];
    uint32_t n = d->mic ? 2 : 1; /* streams, jacks and channel maps: the output's, then the input's */
    int answered = 0;
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
    if (d->mic && got >= 8 && stream == 1 && code >= R_PCM_SET_PARAMS && code <= R_PCM_STOP) {
        status = control_in(d, code, req);
        answered = 1;
    }
    if (!answered) switch (code) {
    case R_PCM_INFO:
    case R_CHMAP_INFO:
    case R_JACK_INFO: /* a line-out jack, and with the input stream a microphone; always connected */
        if (count < 1 || start >= n || count > n - start || size == 0 || size > 32) {
            status = S_BAD_MSG;
            break;
        }
        for (uint32_t i = 0; i < count; i++) {
            uint8_t info[32] = {0};
            int input = start + i == 1;
            if (code == R_PCM_INFO) {
                uint64_t formats = 1ull << HV_VSND_FMT_S16 | 1ull << HV_VSND_FMT_S32 | 1ull << HV_VSND_FMT_FLOAT;
                uint64_t rates = 1ull << HV_VSND_RATE_44100 | 1ull << HV_VSND_RATE_48000;
                memcpy(info + 8, &formats, 8);
                memcpy(info + 16, &rates, 8);
                info[24] = (uint8_t)input;              /* direction: 0 output, 1 input */
                info[25] = input ? (uint8_t)d->mic : 1; /* channels min */
                info[26] = input ? (uint8_t)d->mic : 2; /* channels max */
            } else if (code == R_CHMAP_INFO) {
                info[4] = (uint8_t)input;
                if (input && d->mic == 1) {
                    info[5] = 1;
                    info[6] = CHMAP_MONO;
                } else {
                    info[5] = 2;
                    info[6] = CHMAP_FL;
                    info[7] = CHMAP_FR;
                }
            } else if (input) {
                put32(info + 8, 0x90a00020); /* HDA pin default config: built-in microphone */
                put32(info + 12, 0x20);      /* pin capabilities: input */
                info[16] = 1;                /* connected */
            } else {
                put32(info + 8, 0x01014010); /* HDA pin default config: line out, rear, green jack */
                put32(info + 12, 0x10);      /* pin capabilities: output */
                info[16] = 1;                /* connected */
            }
            memcpy(resp + 4 + i * size, info, size);
        }
        rlen = 4 + count * size;
        break;
    case R_JACK_REMAP:
        break;
    case R_PCM_SET_PARAMS: {
        uint32_t buffer_bytes, period_bytes;
        memcpy(&buffer_bytes, req + 8, 4);
        memcpy(&period_bytes, req + 12, 4);
        unsigned ch = req[20], fmt = req[21], rate = req[22];
        if (stream != 0 || ch < 1 || ch > 2 || (fmt != HV_VSND_FMT_S16 && fmt != HV_VSND_FMT_S32 && fmt != HV_VSND_FMT_FLOAT) ||
            (rate != HV_VSND_RATE_44100 && rate != HV_VSND_RATE_48000) || d->running) {
            status = S_NOT_SUPP;
            break;
        }
        d->channels = ch;
        d->format = fmt;
        d->rate = rate;
        d->rate_hz = rate == HV_VSND_RATE_44100 ? 44100 : 48000;
        d->frame_bytes = ch * format_bytes(fmt);
        d->buffer_bytes = buffer_bytes;
        d->period_bytes = period_bytes;
        break;
    }
    case R_PCM_PREPARE:
        if (stream != 0 || !d->frame_bytes) status = S_BAD_MSG;
        else d->prepared = 1;
        break;
    case R_PCM_START:
        if (stream != 0 || !d->prepared) {
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
        if (stream != 0) {
            status = S_BAD_MSG;
            break;
        }
        if (d->running && d->backend.stop) d->backend.stop(d->backend.opaque);
        d->running = 0;
        break;
    case R_PCM_RELEASE:
        if (stream != 0) {
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
                d->channels, d->format, d->rate_hz, d->buffer_bytes, d->period_bytes);
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
    if (!d->mic) return;
    if (d->in_running && d->in_frame_bytes) {
        uint64_t w = atomic_load(&d->in_written), c = atomic_load(&d->in_consumed);
        if (!d->in_live) { /* no host microphone: silence falls due at the stream's rate by the wall clock */
            uint64_t now = now_ns(), frames = (now - d->in_clock_ns) * d->in_rate_hz / 1000000000ull;
            if (frames) {
                w += frames * d->in_frame_bytes;
                d->in_clock_ns += frames * 1000000000ull / d->in_rate_hz;
                atomic_store(&d->in_written, w);
            }
        }
        /* The guest has no buffer waiting: keep a tenth of a second at most, so what it records next is not old. */
        uint64_t keep = (uint64_t)d->in_rate_hz / 10 * d->in_frame_bytes;
        if (d->in_head == d->in_tail && w - c > keep) atomic_store(&d->in_consumed, w - keep);
    }
    if (take_rx(d)) hv_virtio_interrupt(v);
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

void hv_virtio_snd_push(HvVirtioSnd *d, const void *buf, uint32_t len) {
    uint64_t w = atomic_load_explicit(&d->in_written, memory_order_relaxed);
    uint64_t c = atomic_load_explicit(&d->in_consumed, memory_order_acquire);
    if (!d->in_ring || len > HV_VSND_IN_RING || w - c + len > HV_VSND_IN_RING) { /* the guest is behind: this much is lost */
        d->in_dropped++;
        return;
    }
    uint32_t off = 0;
    while (off < len) {
        uint32_t at = (uint32_t)((w + off) % HV_VSND_IN_RING), k = len - off;
        if (k > HV_VSND_IN_RING - at) k = HV_VSND_IN_RING - at;
        memcpy(d->in_ring + at, (const uint8_t *)buf + off, k);
        off += k;
    }
    atomic_store_explicit(&d->in_written, w + len, memory_order_release);
    hv_machine_wake(d->vio.m);
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
    } else if (q == RXQ && d->mic) {
        if (take_rx(d)) hv_virtio_interrupt(v);
    } else if (q == RXQ) { /* no input stream: hand capture buffers straight back */
        int any = 0;
        while (hv_virtq_pop(v, q, &e) > 0) {
            uint8_t st[8] = {0};
            put32(st, S_BAD_MSG);
            hv_virtq_write(v, &e, 0, st, 8);
            hv_virtq_push(v, q, &e, 8);
            any = 1;
        }
        if (any) hv_virtio_interrupt(v);
    } /* event queue: kept for events we never send */
}

HvVirtioSnd *hv_virtio_snd_create(HvMachine *m, HvPcie *bus) {
    HvVirtioSnd *d = calloc(1, sizeof *d);
    HvVirtio *v = &d->vio;
    hv_virtio_init(v, m, 25, 0x040100, 4, 0); /* sound device, multimedia audio */
    v->dev = d;
    v->notify = notify;
    /* HV_MIC=1 (or 2, for a stereo stream): the Mac gets a microphone, the host's. Off, the device is output only. */
    const char *mic = getenv("HV_MIC");
    d->mic = mic ? (*mic == '2' ? 2 : *mic == '1' ? 1 : 0) : 0;
    if (d->mic) {
        d->in_ring = calloc(1, HV_VSND_IN_RING);
        fprintf(stderr, "hengevm: sound: input stream offered (%d ch)\n", d->mic);
    }
    uint32_t jacks = d->mic ? 2 : 1, streams = jacks, chmaps = jacks;
    memcpy(v->config, &jacks, 4);
    memcpy(v->config + 4, &streams, 4);
    memcpy(v->config + 8, &chmaps, 4);
    v->config_len = 12;
    d->ring = calloc(1, HV_VSND_RING);
    hv_pcie_attach(bus, &v->pci);
    return d;
}

/* After a pause or a restore: the host's player again for a stream that was playing, and the clock that drains it
   without one starts from now (host times from before mean nothing). */
void hv_virtio_snd_resume(HvVirtioSnd *d) {
    if (!d) return;
    d->clock_start_ns = now_ns();
    d->clock_start_consumed = atomic_load(&d->consumed);
    if (d->running && d->backend.start && d->backend.start(d->backend.opaque, d->channels, d->format, d->rate_hz) != 0)
        d->backend.start = NULL;
}
