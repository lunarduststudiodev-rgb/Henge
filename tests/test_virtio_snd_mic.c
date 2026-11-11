/* The sound device's microphone (devices/virtio_snd.c, stream 1): what the guest is told of it, its
   capture buffers filled from the host's microphone at the stream's rate (silence when the host has
   nothing, a cushion against lumpy capture, catching up when it falls behind), the host's capture on
   only while the guest records, and the stream saved and restored with the Mac (vmapple_snap.c).
   A small virtio driver plays the guest, on a real board. */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "snapshot.h"
#include "vmapple.h"

#define RAM_SIZE (64ull << 20)

static int checks, failures;
static void check(const char *what, long long got, long long want) {
    checks++;
    if (got != want) {
        printf("  FAIL %s: got %lld, want %lld\n", what, got, want);
        failures++;
    }
}

typedef struct {
    HvMachine m;
    HvCpu cpu;
    HvVmapple b;
} Board;

static Board A, B, C, D;
static Board *cur;

static void rd(uint64_t pa, void *p, size_t n) { hv_phys_read(&cur->m, pa, p, n); }
static uint32_t rd32(uint64_t pa) {
    uint32_t v = 0;
    rd(pa, &v, 4);
    return v;
}
static void wr32(uint64_t pa, uint32_t v) { hv_phys_write(&cur->m, pa, &v, 4); }
static void wr16(uint64_t pa, uint16_t v) { hv_phys_write(&cur->m, pa, &v, 2); }
static void wr8(uint64_t pa, uint8_t v) { hv_phys_write(&cur->m, pa, &v, 1); }
static void wr64(uint64_t pa, uint64_t v) { hv_phys_write(&cur->m, pa, &v, 8); }
static uint8_t *ram(uint64_t pa) { return hv_phys_ptr(&cur->m, pa, 1); }

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- the host's side: a fake microphone ---- */

static int cap_starts, cap_stops, cap_refuse;
static unsigned cap_args[3];
static int fake_cap_start(void *opaque, unsigned channels, unsigned format, unsigned rate_hz) {
    (void)opaque;
    cap_starts++, cap_args[0] = channels, cap_args[1] = format, cap_args[2] = rate_hz;
    return cap_refuse ? -1 : 0;
}
static void fake_cap_stop(void *opaque) { (void)opaque, cap_stops++; }

static int build(Board *x) {
    if (hv_machine_init(&x->m, 4 << 20) != 0) return -1;
    HvCfgParams cfg = {.nr_cpus = 1, .ecid = 0x1234, .mac_en0 = {2, 0, 0, 0x12, 0x34, 0x56}};
    static const uint8_t mac[6] = {2, 0, 0, 0x12, 0x34, 0x56};
    if (hv_vmapple_create(&x->b, &x->m, &x->cpu, RAM_SIZE, NULL, NULL, &cfg, mac) != 0) return -1;
    hv_vmapple_reset_cpu(&x->b);
    if (x->b.snd) x->b.snd->backend = (HvVirtioSndBackend){NULL, NULL, NULL, fake_cap_start, fake_cap_stop};
    return 0;
}

/* ---- a small virtio driver ---- */

#define BAR (VMAPPLE_PCIE_MMIO + 0x500000)
#define RING (VMAPPLE_MEM + 0x100000) /* queue q at RING + q * 0x4000 */
#define REQ (VMAPPLE_MEM + 0x200000)
#define RESP (VMAPPLE_MEM + 0x201000)
#define HDR (VMAPPLE_MEM + 0x202000)  /* RX buffer i's header at HDR + 16 i */
#define RXBUF (VMAPPLE_MEM + 0x210000) /* RX buffer i's data at RXBUF + 0x1000 i */
#define QSIZE 32
#define CTLQ 0
#define RXQ 3
#define PERIOD 960 /* 10 ms of 48 kHz mono 16-bit */

static uint64_t qring(int q) { return RING + 0x4000ull * (uint64_t)q; }

static int find_slot(uint16_t vendor, uint16_t device) {
    for (int s = 0; s < 32; s++)
        if (rd32(VMAPPLE_PCIE_ECAM + ((uint64_t)s << 15)) == ((uint32_t)device << 16 | vendor)) return s;
    return -1;
}

static void setup(int slot) {
    uint64_t cfg = VMAPPLE_PCIE_ECAM + ((uint64_t)slot << 15);
    wr32(cfg + 0x20, (uint32_t)BAR);
    wr32(cfg + 0x24, 0);
    wr16(cfg + 0x04, 0x6);
    wr8(BAR + 0x14, 0);
    wr8(BAR + 0x14, 1 | 2);
    wr32(BAR + 0x00, 1);
    wr32(BAR + 0x08, 1);
    wr32(BAR + 0x0c, 1);
    wr8(BAR + 0x14, 1 | 2 | 8);
    for (int q = 0; q < 4; q++) {
        uint64_t qr = qring(q);
        memset(ram(qr), 0, 0x4000);
        wr16(BAR + 0x16, (uint16_t)q);
        wr16(BAR + 0x18, QSIZE);
        wr64(BAR + 0x20, qr);
        wr64(BAR + 0x28, qr + 0x1000);
        wr64(BAR + 0x30, qr + 0x2000);
        wr16(BAR + 0x1c, 1);
    }
    wr8(BAR + 0x14, 1 | 2 | 8 | 4);
}

static void put_desc(int q, int i, uint64_t addr, uint32_t len, uint16_t flags, uint16_t next) {
    uint8_t *d = ram(qring(q) + 16u * (unsigned)i);
    memcpy(d, &addr, 8);
    memcpy(d + 8, &len, 4);
    memcpy(d + 12, &flags, 2);
    memcpy(d + 14, &next, 2);
}
static void offer(int q, uint16_t head) {
    uint16_t *avail = (uint16_t *)ram(qring(q) + 0x1000);
    avail[2 + avail[1] % QSIZE] = head;
    avail[1]++;
    wr16(BAR + 0x3000 + 4u * (unsigned)q, (uint16_t)q);
}
static uint16_t used_idx(int q) { return ((uint16_t *)ram(qring(q) + 0x2000))[1]; }
static void used(int q, uint16_t n, uint32_t *id, uint32_t *len) {
    uint32_t *u = (uint32_t *)ram(qring(q) + 0x2000 + 4 + 8u * (n % QSIZE));
    *id = u[0], *len = u[1];
}

/* A control request; returns its status (or -1 with no reply), the reply in RESP. */
static int ctl_used;
static long long request(const void *req, uint32_t len, uint32_t resp_len) {
    memcpy(ram(REQ), req, len);
    memset(ram(RESP), 0xee, 256);
    put_desc(CTLQ, 0, REQ, len, 1, 1);
    put_desc(CTLQ, 1, RESP, resp_len, 2, 0);
    offer(CTLQ, 0);
    if (used_idx(CTLQ) != (uint16_t)(ctl_used + 1)) return -1;
    ctl_used++;
    return rd32(RESP);
}
static long long info(uint32_t code, uint32_t start, uint32_t count, uint32_t size) {
    uint32_t r[4] = {code, start, count, size};
    return request(r, 16, 4 + count * size);
}
static long long pcm(uint32_t code, uint32_t stream) {
    uint32_t r[2] = {code, stream};
    return request(r, 8, 4);
}
static long long set_params(uint32_t stream, unsigned ch, unsigned fmt, unsigned rate) {
    uint8_t r[24] = {0};
    uint32_t w[4] = {0x0101, stream, 10 * PERIOD, PERIOD};
    memcpy(r, w, 16);
    r[20] = (uint8_t)ch, r[21] = (uint8_t)fmt, r[22] = (uint8_t)rate;
    return request(r, 24, 4);
}

/* RX buffer i (0..7): its header (stream), then PERIOD bytes of data and the status, as one writable
   descriptor (or, split, as two: data then status). */
static int rx_used;
static void rx_offer(int i, uint32_t stream, int split) {
    wr32(HDR + 16u * (unsigned)i, stream);
    memset(ram(RXBUF + 0x1000u * (unsigned)i), 0xcc, PERIOD + 8);
    int d = 2 * i; /* 0..15 */
    if (split) {
        /* three descriptors, from 16 (i < 4) */
        d = 16 + 3 * i;
        put_desc(RXQ, d, HDR + 16u * (unsigned)i, 4, 1, (uint16_t)(d + 1));
        put_desc(RXQ, d + 1, RXBUF + 0x1000u * (unsigned)i, PERIOD, 1 | 2, (uint16_t)(d + 2));
        put_desc(RXQ, d + 2, RXBUF + 0x1000u * (unsigned)i + PERIOD, 8, 2, 0);
    } else {
        put_desc(RXQ, d, HDR + 16u * (unsigned)i, 4, 1, (uint16_t)(d + 1));
        put_desc(RXQ, d + 1, RXBUF + 0x1000u * (unsigned)i, PERIOD + 8, 2, 0);
    }
    offer(RXQ, (uint16_t)d);
}
/* The next returned RX buffer: which (by its head), its length, status; its data in *data. */
static int rx_next(int *which, uint32_t *len, uint32_t *status, int16_t *data) {
    if (used_idx(RXQ) == (uint16_t)rx_used) return 0;
    uint32_t id;
    used(RXQ, (uint16_t)rx_used++, &id, len);
    int i = id >= 16 ? (int)(id - 16) / 3 : (int)id / 2;
    *which = i;
    *status = rd32(RXBUF + 0x1000u * (unsigned)i + PERIOD);
    if (data) rd(RXBUF + 0x1000u * (unsigned)i, data, PERIOD);
    return 1;
}
static int returned(void) { return (uint16_t)(used_idx(RXQ) - (uint16_t)rx_used); }

static HvVirtioSnd *snd(void) { return cur->b.snd; }
static void due_at(uint64_t ms) { snd()->in.start_ns = now_ns() - ms * 1000000ull; }
static void poll(void) { hv_virtio_snd_poll(snd()); }

/* The fake microphone's samples: sample k is k (mod 2^15). */
static uint32_t mic_at;
static void mic(uint32_t frames) {
    static int16_t buf[48000];
    for (uint32_t i = 0; i < frames; i++) buf[i] = (int16_t)((mic_at + i) & 0x7fff);
    hv_virtio_snd_capture(snd(), buf, frames * 2);
    mic_at += frames;
}
static int is_ramp(const int16_t *d, uint32_t from) {
    for (int i = 0; i < PERIOD / 2; i++)
        if (d[i] != (int16_t)((from + (uint32_t)i) & 0x7fff)) return 0;
    return 1;
}
static int is_silence(const int16_t *d) {
    for (int i = 0; i < PERIOD / 2; i++)
        if (d[i]) return 0;
    return 1;
}

#define S_OK 0x8000
#define S_BAD_MSG 0x8001
#define S_NOT_SUPP 0x8002

static void queries(void) {
    check("config: two jacks", rd32(BAR + 0x2000), 2);
    check("config: two streams", rd32(BAR + 0x2004), 2);
    check("config: two channel maps", rd32(BAR + 0x2008), 2);
    check("PCM_INFO of both", info(0x0100, 0, 2, 32), S_OK);
    uint8_t r[4 + 64];
    rd(RESP, r, sizeof r);
    check("stream 0 is an output", r[4 + 24], 0);
    check("stream 1 is an input", r[4 + 32 + 24], 1);
    check("stream 1: 1..2 channels", r[4 + 32 + 25] == 1 && r[4 + 32 + 26] == 2, 1);
    uint64_t fmts, rates;
    memcpy(&fmts, r + 4 + 32 + 8, 8);
    memcpy(&rates, r + 4 + 32 + 16, 8);
    check("stream 1: S16, S32, float", fmts, 1ull << 5 | 1ull << 17 | 1ull << 19);
    check("stream 1: 44.1 and 48 kHz", rates, 1ull << 6 | 1ull << 7);
    check("PCM_INFO of stream 1 alone", info(0x0100, 1, 1, 32), S_OK);
    check("  it says input", ram(RESP)[4 + 24], 1);
    check("JACK_INFO of both", info(1, 0, 2, 24), S_OK);
    check("jack 0: line out", rd32(RESP + 4 + 8), 0x01014010);
    check("jack 1: a microphone in", rd32(RESP + 4 + 24 + 8), 0x01A19020);
    check("jack 1: input capable, connected", rd32(RESP + 4 + 24 + 12) == 0x20 && ram(RESP)[4 + 24 + 16] == 1, 1);
    check("CHMAP_INFO of map 1", info(0x0200, 1, 1, 24), S_OK);
    check("  input, stereo", ram(RESP)[4 + 4] == 1 && ram(RESP)[4 + 5] == 2, 1);
    check("no stream 2", info(0x0100, 2, 1, 32), S_BAD_MSG);
    check("not two from stream 1", info(0x0100, 1, 2, 32), S_BAD_MSG);
    check("not none", info(0x0100, 0, 0, 32), S_BAD_MSG);
    check("not size 0", info(0x0100, 0, 1, 0), S_BAD_MSG);
}

static void capture(void) {
    /* Before it's prepared, a capture buffer comes straight back, refused, its status at its end */
    rx_offer(0, 1, 0);
    int which;
    uint32_t len, st;
    int16_t data[PERIOD / 2];
    check("unprepared: buffer back at once", rx_next(&which, &len, &st, data), 1);
    check("  refused", st, S_BAD_MSG);
    check("  its whole length", len, PERIOD + 8);

    check("SET_PARAMS: stream 1, mono S16 48 kHz", set_params(1, 1, 5, 7), S_OK);
    check("SET_PARAMS: no stream 2", set_params(2, 1, 5, 7), S_NOT_SUPP);
    check("SET_PARAMS: not 3 channels", set_params(1, 3, 5, 7), S_NOT_SUPP);
    check("SET_PARAMS: not 8 kHz", set_params(1, 1, 5, 1), S_NOT_SUPP);
    check("the output's parameters untouched", snd()->frame_bytes, 0);
    check("START before PREPARE refused", pcm(0x0104, 1), S_BAD_MSG);
    check("PREPARE stream 1", pcm(0x0102, 1), S_OK);

    /* Buffers wait while it is prepared but not running; a buffer for stream 0 on the capture queue is refused */
    rx_offer(0, 1, 0);
    rx_offer(1, 1, 1);
    rx_offer(2, 1, 0);
    rx_offer(3, 0, 0);
    check("a playback buffer on the capture queue: refused", rx_next(&which, &len, &st, NULL) && which == 3 && st == S_BAD_MSG, 1);
    poll();
    check("prepared: the three wait", returned(), 0);
    check("the microphone isn't on yet", cap_starts, 0);

    check("START stream 1", pcm(0x0104, 1), S_OK);
    check("the host's capture started once", cap_starts, 1);
    check("  in the stream's format", cap_args[0] == 1 && cap_args[1] == 5 && cap_args[2] == 48000, 1);
    poll();
    check("nothing due at once", returned(), 0);

    /* 25 ms in: two 10 ms buffers are due, silence (the microphone hasn't sent anything) */
    due_at(25);
    poll();
    check("25 ms: two back", returned(), 2);
    check("  the first", rx_next(&which, &len, &st, data) && which == 0 && st == S_OK && len == PERIOD + 8, 1);
    check("  silence", is_silence(data), 1);
    check("  the second (split data and status)", rx_next(&which, &len, &st, data) && which == 1 && st == S_OK && len == PERIOD + 8, 1);
    check("  silence", is_silence(data), 1);

    /* The microphone sends 100 ms: past the cushion, so the next buffer is its first samples */
    mic(4800);
    due_at(35);
    poll();
    check("35 ms: the third back", rx_next(&which, &len, &st, data) && which == 2 && st == S_OK, 1);
    check("  the microphone's first samples", is_ramp(data, 0), 1);
    rx_offer(0, 1, 0);
    due_at(45);
    poll();
    check("45 ms: the next", rx_next(&which, &len, &st, data) && which == 0, 1);
    check("  the samples after", is_ramp(data, 480), 1);
    check("  latency: what's still waiting", rd32(RXBUF + PERIOD + 4), 9600 - 2 * PERIOD);

    /* 400 ms more arrive at once (the host's capture started late): it skips to keep only the cushion */
    mic(19200);
    rx_offer(0, 1, 0);
    due_at(55);
    poll();
    uint32_t queued = 4800 + 19200 - 960, skip_to = 4800 + 19200 - 2880 - 480; /* frames: leave 60 ms + this buffer */
    (void)queued;
    check("55 ms: back", rx_next(&which, &len, &st, data), 1);
    check("  from 60 ms before the newest", is_ramp(data, skip_to), 1);
    check("  no underrun yet", snd()->in.underruns, 0);

    /* Seven more due, six buffers' worth waiting: six from the microphone, then silence (it ran dry) */
    for (int i = 0; i < 7; i++) rx_offer(i, 1, 0);
    due_at(125);
    poll();
    check("125 ms: seven back", returned(), 7);
    int ok = 1;
    for (int i = 0; i < 6; i++)
        if (!rx_next(&which, &len, &st, data) || !is_ramp(data, skip_to + 480u * (unsigned)(i + 1))) ok = 0;
    check("  six from the microphone, in order", ok, 1);
    check("  the seventh silence", rx_next(&which, &len, &st, data) && is_silence(data), 1);
    check("  an underrun counted", snd()->in.underruns, 1);
    check("  waiting for a cushion again", snd()->in.primed, 0);

    /* Far behind (the guest gave no buffers for a second): at most a buffer's worth (100 ms) is owed */
    due_at(1125);
    poll();
    uint64_t owed_from = 1125ull * 96 - 10 * PERIOD; /* 96 bytes a millisecond */
    check("after a second without buffers: 100 ms owed, not a second", snd()->in.delivered >= owed_from && snd()->in.delivered < owed_from + 2000, 1);
    rx_offer(0, 1, 0);
    rx_offer(1, 1, 0);
    check("  two buffers taken at once", returned(), 2);
    rx_next(&which, &len, &st, data);
    rx_next(&which, &len, &st, data);
    due_at(0); /* from here, nothing due for a while */
    rx_offer(0, 1, 0);
    rx_offer(1, 1, 1);
    check("  two more held", returned(), 0);

    /* The microphone sending more than the ring holds: the rest dropped */
    uint64_t w0 = atomic_load(&snd()->in.written);
    for (int i = 0; i < 8; i++) mic(48000 / 2);
    check("the ring took what fits", atomic_load(&snd()->in.written) - atomic_load(&snd()->in.read) <= HV_VSND_RING, 1);
    check("  and dropped the rest", snd()->in.overruns > 0 && atomic_load(&snd()->in.written) > w0, 1);

    /* STOP: the host's capture off; buffers stay; capture now is dropped. RELEASE: they come back. */
    check("STOP stream 1", pcm(0x0105, 1), S_OK);
    check("the host's capture stopped", cap_stops, 1);
    uint64_t w1 = atomic_load(&snd()->in.written);
    mic(480);
    check("  capture after STOP is dropped", atomic_load(&snd()->in.written), (long long)w1);
    due_at(5000);
    poll();
    check("  held buffers stay", returned(), 0);
    check("RELEASE stream 1", pcm(0x0103, 1), S_OK);
    check("  both held buffers back", returned(), 2);
    check("  as silence", rx_next(&which, &len, &st, data) && st == S_OK && is_silence(data), 1);
    rx_next(&which, &len, &st, data);
    check("  not prepared now", snd()->in.prepared, 0);
    check("  the host's capture not stopped twice", cap_stops, 1);

    /* A host that can't capture: the stream runs on silence, by the clock */
    cap_refuse = 1;
    check("PREPARE again", pcm(0x0102, 1), S_OK);
    check("START again", pcm(0x0104, 1), S_OK);
    check("  the host was asked", cap_starts, 2);
    check("  and said no", snd()->in.capturing, 0);
    rx_offer(0, 1, 0);
    due_at(15);
    poll();
    check("  buffers still come back", rx_next(&which, &len, &st, data) && st == S_OK && is_silence(data), 1);
    check("STOP", pcm(0x0105, 1), S_OK);
    check("  nothing to stop on the host", cap_stops, 1);
    cap_refuse = 0;
    check("RELEASE", pcm(0x0103, 1), S_OK);
}

/* Saved while the Mac records, with two buffers held: B carries on, its microphone on only once the load all worked. */
static void snapshots(const char *dir) {
    char snap[512], old[512];
    snprintf(snap, sizeof snap, "%s/mic.hvsnap", dir);
    snprintf(old, sizeof old, "%s/no-mic.hvsnap", dir);
    cur = &A;
    check("A: PREPARE", pcm(0x0102, 1), S_OK);
    check("A: START", pcm(0x0104, 1), S_OK);
    rx_offer(0, 1, 0);
    rx_offer(1, 1, 1);
    poll();
    check("A: two held", snd()->in.ptail - snd()->in.phead, 2);
    HvSnapWriter *w = hv_snap_begin(snap, HV_SNAP_NO_SYNC);
    check("A saved", w && hv_vmapple_snap_save(&A.b, w) == 0 && hv_snap_put_ram(w, &A.m, 4) == 0 && hv_snap_commit(w) == 0, 1);

    if (build(&B) != 0) {
        printf("cannot build board B\n");
        failures++;
        return;
    }
    int starts = cap_starts;
    char why[200];
    HvSnapReader *r = hv_snap_open(snap, why, sizeof why);
    check("B loaded", r && hv_snap_get_ram(r, &B.m, 4, HV_SNAP_DIRTY_RAM) == 0 && hv_vmapple_snap_load(&B.b, r) == 0, 1);
    if (r) hv_snap_close(r);
    cur = &B;
    check("B: running", snd()->in.running, 1);
    check("B: the stream's format", snd()->in.channels == 1 && snd()->in.format == 5 && snd()->in.rate_hz == 48000 && snd()->in.frame_bytes == 2, 1);
    check("B: two held", snd()->in.ptail - snd()->in.phead, 2);
    check("B: the microphone not on from the load alone", cap_starts, starts);
    hv_vmapple_restored(&B.b);
    check("B restored: the microphone on", cap_starts, starts + 1);
    check("  in the stream's format", cap_args[0] == 1 && cap_args[1] == 5 && cap_args[2] == 48000, 1);
    rx_used = used_idx(RXQ);
    ctl_used = used_idx(CTLQ);
    due_at(25);
    poll();
    check("B: the held buffers come back", returned(), 2);
    int which;
    uint32_t len, st;
    check("  the first", rx_next(&which, &len, &st, NULL) && which == 0 && st == S_OK && len == PERIOD + 8, 1);
    check("  the second (split)", rx_next(&which, &len, &st, NULL) && which == 1 && st == S_OK, 1);

    /* The driver resets the device while it records: the microphone goes off */
    int stops = cap_stops;
    wr8(BAR + 0x14, 0);
    poll();
    check("B reset: the microphone off", cap_stops, stops + 1);
    check("  not running", snd()->in.running, 0);

    /* A Mac from before the microphone (one stream): restored as it was, the guest still told of one stream */
    setenv("HV_NO_MIC", "1", 1);
    int built = build(&C);
    unsetenv("HV_NO_MIC");
    if (built != 0 || build(&D) != 0) {
        printf("cannot build boards C, D\n");
        failures++;
        return;
    }
    cur = &C;
    check("C (no microphone): one stream", hv_virtio_snd_streams(C.b.snd), 1);
    setup(find_slot(0x1af4, 0x1059));
    ctl_used = rx_used = 0;
    check("C: no stream 1", info(0x0100, 1, 1, 32), S_BAD_MSG);
    check("C: no SET_PARAMS for it", set_params(1, 1, 5, 7), S_NOT_SUPP);
    rx_offer(0, 1, 0);
    check("C: capture buffers refused", rx_next(&which, &len, &st, NULL) && st == S_BAD_MSG, 1);
    w = hv_snap_begin(old, HV_SNAP_NO_SYNC);
    check("C saved", w && hv_vmapple_snap_save(&C.b, w) == 0 && hv_snap_put_ram(w, &C.m, 4) == 0 && hv_snap_commit(w) == 0, 1);
    r = hv_snap_open(old, why, sizeof why);
    check("C's snapshot has no microphone section", r && hv_snap_get(r, HV_SNAP_TAG('V', 'S', 'N', 'I'), 1, &(HvSnapIn){0}) != 0, 1);
    if (r) hv_snap_close(r);
    r = hv_snap_open(old, why, sizeof why);
    check("D loads C's snapshot", r && hv_snap_get_ram(r, &D.m, 4, HV_SNAP_DIRTY_RAM) == 0 && hv_vmapple_snap_load(&D.b, r) == 0, 1);
    if (r) hv_snap_close(r);
    hv_vmapple_restored(&D.b);
    cur = &D;
    check("D: the guest still told of one stream", hv_virtio_snd_streams(D.b.snd), 1);
    check("D: the microphone idle", snd()->in.running || snd()->in.prepared, 0);
    rx_used = used_idx(RXQ);
    rx_offer(1, 1, 0);
    check("D: capture buffers refused", rx_next(&which, &len, &st, NULL) && st == S_BAD_MSG, 1);
}

int main(void) {
    char dir[512];
    const char *tmp = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    snprintf(dir, sizeof dir, "%s/henge-mic.XXXXXX", tmp);
    if (!mkdtemp(dir)) return 1;
    unsetenv("HV_NO_MIC");
    unsetenv("HV_NO_SOUND");
    if (build(&A) != 0) return printf("cannot build board A\n"), 1;
    cur = &A;
    int slot = find_slot(0x1af4, 0x1059);
    check("the sound device is there", slot >= 0, 1);
    if (slot < 0) return 1;
    setup(slot);
    queries();
    capture();
    snapshots(dir);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0) printf("  (could not remove %s)\n", dir);
    printf("test_virtio_snd_mic: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
