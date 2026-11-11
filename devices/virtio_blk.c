#include "virtio_blk.h"

#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define F_SEG_MAX (1ull << 2)
#define F_RO (1ull << 5)
#define F_BLK_SIZE (1ull << 6)
#define F_FLUSH (1ull << 9)

#define T_IN 0
#define T_OUT 1
#define T_FLUSH 4
#define T_GET_ID 8
#define T_APPLE_BARRIER 0x10000

#define S_OK 0
#define S_IOERR 1
#define S_UNSUPP 2

/* Disk I/O straight between the file and the guest's buffers: the
   segments of one direction, skipping `off` bytes of them, `len` bytes in all.
   Returns 0, or -1 on a host error or a buffer outside guest RAM. */
static int seg_io(HvVirtioBlk *b, const HvVirtqElem *e, int writable, uint32_t off, uint64_t disk_off, uint32_t len) {
    for (int s = 0; s < e->nseg && len; s++) {
        const HvVirtqSeg *g = &e->seg[s];
        if (g->write != writable) continue;
        if (off >= g->len) {
            off -= g->len;
            continue;
        }
        uint32_t n = g->len - off;
        if (n > len) n = len;
        void *p = hv_phys_ptr(b->vio.m, g->addr + off, n);
        if (!p) return -1;
        int r = writable ? hv_disk_read(b->disk, disk_off, p, n) : hv_disk_write(b->disk, disk_off, p, n);
        if (writable && r == 0) hv_dma_written(b->vio.m, g->addr + off, n);
        if (r != 0) return -1;
        disk_off += n;
        len -= n;
        off = 0;
    }
    return len ? -1 : 0;
}

static uint32_t readable(const HvVirtqElem *e) {
    uint32_t n = 0;
    for (int s = 0; s < e->nseg; s++)
        if (!e->seg[s].write) n += e->seg[s].len;
    return n;
}

/* Worker thread. */
static int handle(HvVirtioBlk *b, HvVirtqElem *e, uint32_t *written) {
    HvVirtio *v = &b->vio;
    struct { uint32_t type, reserved; uint64_t sector; } hdr;
    uint32_t wlen = hv_virtq_writable(e);
    *written = 0;
    if (wlen < 1 || hv_virtq_read(v, e, 0, &hdr, sizeof hdr) != sizeof hdr) return -1;
    uint32_t data = wlen - 1; /* the status byte is the last writable one */
    uint8_t status = S_OK;
    switch (hdr.type) {
    case T_IN:
        if (seg_io(b, e, 1, 0, hdr.sector * 512, data) != 0) status = S_IOERR;
        else *written = data;
        break;
    case T_OUT: {
        uint32_t rlen = readable(e);
        if (rlen < sizeof hdr || seg_io(b, e, 0, sizeof hdr, hdr.sector * 512, rlen - (uint32_t)sizeof hdr) != 0)
            status = S_IOERR;
        break;
    }
    case T_FLUSH:
        if (hv_disk_flush(b->disk) != 0) status = S_IOERR;
        break;
    case T_GET_ID: {
        char id[20] = "hengevm-disk";
        uint32_t n = data < 20 ? data : 20;
        hv_virtq_write(v, e, 0, id, n);
        *written = n;
        break;
    }
    case T_APPLE_BARRIER: /* a no-op here, as in QEMU: requests complete in order */
        break;
    default:
        status = S_UNSUPP;
        break;
    }
    if (status != S_OK) {
        __atomic_fetch_add(&b->errors, 1, __ATOMIC_RELAXED);
        static int reported;
        if (reported++ < 20)
            fprintf(stderr, "virtio-blk %s: request type %#x sector %llu (%u bytes) -> status %u\n", b->disk->offset ? "aux" : "root",
                    hdr.type, (unsigned long long)hdr.sector, data, status);
    }
    hv_virtq_write(v, e, data, &status, 1);
    *written += 1;
    return 0;
}

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

/* Workers take requests in order and run them in parallel; a flush or an
   Apple barrier first waits for every request before it to finish, and nothing
   after it starts until it is done, so their ordering meaning holds. */
static void *worker(void *opaque) {
    HvVirtioBlk *b = opaque;
    /* HV_BLK_STATS: requests, bytes, flushes and how busy this worker was, every 10 s. */
    int stats = getenv("HV_BLK_STATS") != NULL;
    uint64_t every = stats && atoi(getenv("HV_BLK_STATS")) > 0 ? (uint64_t)atoi(getenv("HV_BLK_STATS")) * 1000000 : 10000000;
    uint64_t t_last = now_us(), busy = 0, reqs = 0, bytes = 0, flushes = 0, flush_us = 0;
    for (;;) {
        pthread_mutex_lock(&b->mu);
        while (b->fence || b->claimed == __atomic_load_n(&b->submitted, __ATOMIC_ACQUIRE)) {
            if (b->stopping && !b->fence && b->claimed == b->submitted) {
                pthread_mutex_unlock(&b->mu);
                return NULL;
            }
            pthread_cond_wait(&b->cv, &b->mu);
        }
        HvVblkReq *r = &b->ring[b->claimed % HV_VBLK_RING];
        uint32_t type = 0;
        hv_virtq_read(&b->vio, &r->e, 0, &type, 4);
        int fence = type == T_FLUSH || type == T_APPLE_BARRIER;
        b->claimed++;
        if (fence) {
            b->fence = 1;
            while (b->inflight) pthread_cond_wait(&b->done_cv, &b->mu);
        }
        b->inflight++;
        pthread_mutex_unlock(&b->mu);

        uint64_t t0 = stats ? now_us() : 0;
        if (handle(b, &r->e, &r->written) != 0) r->written = 0;
        if (stats) {
            uint64_t t1 = now_us();
            busy += t1 - t0, reqs++, bytes += r->written > 1 ? r->written : readable(&r->e);
            if (type == T_FLUSH) flushes++, flush_us += t1 - t0;
            if (t1 - t_last > every) {
                fprintf(stderr, "virtio-blk %s worker: %.0f req/s %.1f MB/s, %.0f flushes/s (%.1f ms each), busy %.0f%%\n",
                        b->disk->offset ? "aux" : "root", reqs * 1e6 / every, bytes * 1e6 / every / (1 << 20), flushes * 1e6 / every,
                        flushes ? flush_us / 1000.0 / flushes : 0.0, busy * 100.0 / every);
                t_last = t1, busy = reqs = bytes = flushes = flush_us = 0;
            }
        }

        pthread_mutex_lock(&b->mu);
        __atomic_store_n(&r->done, 1, __ATOMIC_RELEASE);
        b->inflight--;
        if (fence) b->fence = 0;
        __atomic_add_fetch(&b->completed, 1, __ATOMIC_RELEASE);
        pthread_cond_broadcast(&b->done_cv);
        if (fence) pthread_cond_broadcast(&b->cv);
        pthread_mutex_unlock(&b->mu);
        hv_machine_wake(b->vio.m);
    }
    return NULL;
}

/* CPU thread: hand everything the driver made available to the worker, while the ring has room. */
static void notify(HvVirtio *v, int q) {
    HvVirtioBlk *b = v->dev;
    int any = 0;
    while (b->submitted - b->published < HV_VBLK_RING) {
        HvVblkReq *r = &b->ring[b->submitted % HV_VBLK_RING];
        int got = hv_virtq_pop(v, q, &r->e);
        if (got < 0) {
            /* A chain this model cannot follow (too many segments, or outside
               RAM): it is already taken from the ring, so it must still be
               completed, or the guest loses that request slot for good. */
            fprintf(stderr, "virtio-blk: broken request chain (%d segments seen), failing it\n", r->e.nseg);
            uint8_t st = S_IOERR;
            if (hv_virtq_writable(&r->e)) hv_virtq_write(v, &r->e, hv_virtq_writable(&r->e) - 1, &st, 1);
            hv_virtq_push(v, q, &r->e, 1);
            hv_virtio_interrupt(v);
            continue;
        }
        if (got == 0) break;
        r->q = q;
        pthread_mutex_lock(&b->mu);
        __atomic_store_n(&b->submitted, b->submitted + 1, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&b->mu);
        any = 1;
    }
    if (any) pthread_cond_broadcast(&b->cv);
}

void hv_virtio_blk_drain(HvVirtioBlk *b) {
    while (__atomic_load_n(&b->completed, __ATOMIC_ACQUIRE) != b->submitted) usleep(100);
    hv_virtio_blk_poll(b);
}

void hv_virtio_blk_stop(HvVirtioBlk *b) {
    if (!b) return;
    pthread_mutex_lock(&b->mu);
    b->stopping = 1;
    pthread_cond_broadcast(&b->cv);
    pthread_mutex_unlock(&b->mu);
    for (int i = 0; i < b->workers_started; i++) pthread_join(b->worker[i], NULL);
    b->workers_started = 0;
}

/* Workers again after hv_virtio_blk_stop (a machine paused to be saved runs on). 0, or -1 if none started. */
int hv_virtio_blk_start(HvVirtioBlk *b) {
    if (!b || b->workers_started) return 0;
    pthread_mutex_lock(&b->mu);
    b->stopping = 0;
    pthread_mutex_unlock(&b->mu);
    for (int i = 0; i < HV_VBLK_WORKERS; i++) {
        if (pthread_create(&b->worker[i], NULL, worker, b) != 0) break;
        b->workers_started++;
    }
    return b->workers_started ? 0 : -1;
}

/* Stopped and every request handed back: nothing of it is in flight. */
int hv_virtio_blk_idle(HvVirtioBlk *b) {
    if (!b) return 1;
    hv_virtio_blk_poll(b);
    return !b->workers_started && b->published == b->submitted && !b->inflight;
}

void hv_virtio_blk_destroy(HvVirtioBlk *b) {
    if (!b) return;
    hv_virtio_blk_stop(b);
    pthread_mutex_destroy(&b->mu);
    pthread_cond_destroy(&b->cv);
    pthread_cond_destroy(&b->done_cv);
    free(b->ring);
    free(b);
}

/* CPU thread: hand finished requests back, in whatever order they finished;
   a slot is reused once it and every slot before it are back. */
void hv_virtio_blk_poll(HvVirtioBlk *b) {
    if (!b) return;
    int q = -1;
    for (uint32_t i = b->published; i != b->submitted; i++) {
        HvVblkReq *r = &b->ring[i % HV_VBLK_RING];
        if (r->pushed || !__atomic_load_n(&r->done, __ATOMIC_ACQUIRE)) continue;
        hv_virtq_push(&b->vio, r->q, &r->e, r->written);
        r->pushed = 1;
        q = r->q;
        b->requests++;
    }
    while (b->published != b->submitted && b->ring[b->published % HV_VBLK_RING].pushed) {
        HvVblkReq *r = &b->ring[b->published % HV_VBLK_RING];
        r->pushed = 0;
        __atomic_store_n(&r->done, 0, __ATOMIC_RELAXED);
        b->published++;
    }
    if (q < 0) return;
    hv_virtio_interrupt(&b->vio);
    notify(&b->vio, q); /* requests that waited for room in the ring */
}

HvVirtioBlk *hv_virtio_blk_create(HvMachine *m, HvPcie *bus, HvDisk *disk, int apple_variant) {
    HvVirtioBlk *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    HvVirtio *v = &b->vio;
    hv_virtio_init(v, m, 2, 0x010000, 1, F_SEG_MAX | F_BLK_SIZE | F_FLUSH | (disk->readonly ? F_RO : 0));
    v->dev = b;
    v->notify = notify;
    b->disk = disk;
    b->ring = calloc(HV_VBLK_RING, sizeof *b->ring);
    pthread_mutex_init(&b->mu, NULL);
    pthread_cond_init(&b->cv, NULL);
    pthread_cond_init(&b->done_cv, NULL);
    if (!b->ring) { hv_virtio_blk_destroy(b); return NULL; }
    for (int i = 0; i < HV_VBLK_WORKERS; i++) {
        if (pthread_create(&b->worker[i], NULL, worker, b) != 0) break;
        b->workers_started++;
    }
    if (!b->workers_started) { hv_virtio_blk_destroy(b); return NULL; }
    uint64_t capacity = disk->size / 512;
    /* Apple's driver takes one descriptor per segment from the queue's 256
       and does not use indirect tables: with segments up to 126, two big
       writes used them all and the next failed (kIOReturnNoMemory, ENOMEM in
       APFS, or its cleanup asserted "Adding invalid index 65535"). 32 got a
       restore through, but a freshly installed Mac's first boot (Spotlight
       indexing while Setup Assistant starts) still saw page-in ENOMEM; with 8
       it boots to Setup Assistant cleanly. */
    uint32_t seg_max = getenv("HV_BLK_SEG_MAX") ? (uint32_t)atoi(getenv("HV_BLK_SEG_MAX")) : 8;
    if (seg_max < 1 || seg_max > HV_VIRTQ_MAX_SEGS - 2) seg_max = 8;
    uint32_t blk_size = 512, apple_type = (uint32_t)apple_variant;
    uint16_t num_queues = 1;
    memcpy(v->config + 0, &capacity, 8);
    memcpy(v->config + 12, &seg_max, 4);
    memcpy(v->config + 20, &blk_size, 4);
    memcpy(v->config + 34, &num_queues, 2);
    memcpy(v->config + 60, &apple_type, 4);
    v->config_len = 96;
    if (apple_variant) {
        /* Apple's identifiers, SCSI storage class, as vmapple-virtio-blk-pci sets them. */
        uint16_t vendor = 0x106b, device = 0x1a00;
        memcpy(v->pci.config + 0, &vendor, 2);
        memcpy(v->pci.config + 2, &device, 2);
        /* Apple vendor data (a virtio 1.2 PCI vendor-data capability, cfg_type
           9, vendor 0x106b). AppleVirtIOStorageDevice reads 8 bytes of it: if
           bit 0 of the first byte is clear, or the capability is missing, it
           reports the disk as AES-XTS inline-encrypted, and APFS then refuses
           an unencrypted Data volume. These disks are plain files, so bit 0
           is set. QEMU's model has no such capability. */
        uint8_t *cap = v->pci.config + 0x88;
        v->pci.config[0x71] = 0x88; /* chain it after the notify capability */
        cap[0] = 0x09;
        cap[1] = 0x00;
        cap[2] = 6 + 8;
        cap[3] = 9;
        memcpy(cap + 4, &vendor, 2);
        memset(cap + 6, 0, 8);
        cap[6] = 1; /* not inline-encrypted */
    }
    hv_pcie_attach(bus, &v->pci);
    return b;
}
