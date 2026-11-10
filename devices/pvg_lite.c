/* The guest driver's view of the graphics device, worked out from its symbols
   and from tracing ParavirtualizedGraphics.framework (HV_PVG_TRACE=2):

   gfx registers
     +1000 enable, +1004 FIFO buffer size, +1010 ring offset in it (0x4000),
     +1030 FIFO buffer page (<< 14), +101c root page (<< 14)
     +1008 root ring write pointer (doorbell), +100c read pointer
     +1020 channel doorbell (the value is the channel)
     +1018 stamps-updated bits (1 << channel), +1014 display bits, +102c faults
     +1034 protocol version (the driver offers one, the device answers),
     +122c number of displays

   FIFO buffer: a control area where stamp[channel] is at +4 * channel, then
   the root ring. Root page: channel records at 0x400 + 0x14 * (channel - 1)
   of {write, read, -1, id, page table page}; the table lists the ring's 16 KB
   pages as 32-bit page numbers, zero after the last.

   Every command, on every ring, starts {op, length, stamp}. It is complete
   when the device has stored its stamp and moved the read pointer.

   Display: channel 4 op 1 names a shared state page the device fills in
   (modes, seq at +200, ack pending at +fa9, state at +fc0); the guest reads
   +100 once it has set its mask at +104, acks with op 2. Swaps are op 8:
   {port, 0, u64 mapping id, u64 gamma offset, u64 gamma length, count, sum},
   and the device counts them at +f98 with the mapping id at +fb0.

   A command with bit 16 of its op set first carries a barrier: {channel, stamp}.

   iosurface mapper: +1028 queue count, +1030 enable, +1000 ring, +1010
   descriptor table, +1008 size, +1018 submitted index, +1020 completed index.
   Ring entries are {op (1 map, 2 unmap, 3 test), mapping id, result, 0}. */

#include <unistd.h>
#include <CoreFoundation/CoreFoundation.h>
#include <os/lock.h>
#include <stdatomic.h>
#include "pvg_lite.h"
#include <mach/mach_time.h>

int hv_pvg_trace_on(void) {
    static const char *tf;
    static int read, on;
    static _Atomic uint64_t next;
    if (!read) read = 1, tf = getenv("HV_PVG_TRACE_FILE");
    if (!tf) return 0;
    uint64_t now = mach_absolute_time();
    if (now >= atomic_load_explicit(&next, memory_order_relaxed)) {
        on = access(tf, F_OK) == 0;
        atomic_store_explicit(&next, now + 2400000, memory_order_relaxed); /* 100 ms of 24 MHz ticks */
    }
    return on;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/machine.h"

#define MAX_CHANNELS 16

struct HvPvgLite {
    HvMachine *m;
    HvGic *gic;
    int gfx_irq, iosfc_irq;
    int log;
    uint32_t regs[0x1000 / 4]; /* +1000.. */
    uint64_t io_regs[0x40 / 8];
    uint32_t root_rp, status, chan_pending;
    int root_pending, iosfc_pending;
    uint64_t display_pa;
    uint64_t swaps;
    /* The last stamp handed to the worker per channel, and who is being drained. */
    uint32_t submitted[MAX_CHANNELS];
    uint32_t draining;
    uint32_t barrier_waits[MAX_CHANNELS];
    uint64_t barriers, barrier_drains, barrier_retries, barrier_timeouts;
    CFTypeRef layer; /* retained until replaced; layer_lock serializes lazy replay handoff */
    os_unfair_lock layer_lock;
    int layer_set; /* an explicit NULL also overrides the renderer's Mac test layer */
    uint32_t task_root[512]; /* page of each task's root, by the id in commands */
    uint64_t task_objects[512], task_placement[512]; /* task offsets of its object lists */
    int old_driver; /* macOS 14's (and older?) GPU driver: object lists at a fixed place, compact surface descriptions */
    uint8_t obj_seen[512][8192];
    int tally;
    uint64_t counts[6][256];
    uint64_t execs, bad;
    double last_tally;
    _Atomic(HvPvgMetal *) metal; /* release-published after the cached layer is applied */
    int metal_tried;
    uint32_t stamps_pending; /* channels Metal finished, for the next poll */
};

/* What the guest's GetDeviceInfo asks for: key, value pairs, as the framework
   answers on an Apple GPU. */
static const uint32_t device_info[][2] = {
    {0x01, 4},      {0x02, 0},     {0x03, 0x400},  {0x04, 0x400},  {0x05, 0x400},  {0x06, 0x8000}, {0x07, 1},
    {0x08, 1},      {0x09, 1},     {0x0a, 8},      {0x0b, 0x1ff},  {0x0c, 1},      {0x0d, 0x10},   {0x0e, 1},
    {0x0f, 0x100},  {0x10, 1},     {0x11, 1},      {0x12, 0x20007}, {0x13, 1},     {0x14, 2},      {0x15, 1},
    {0x16, 2},      {0x17, 1},     {0x18, 1},      {0x19, 1},      {0x1a, 1},      {0x1b, 1},      {0x1c, 7},
    {0x21, 0xfff},  {0x24, 0xffffffff}, {0x1d, 0x400}, {0x1e, 0x1f}, {0x1f, 0x10000}, {0x20, 0x10}, {0x22, 5},
    {0x29, 7},      {0x23, 0x800}, {0x25, 0x3f1},  {0x28, 0x10},
};

static uint32_t *guest32(HvPvgLite *p, uint64_t pa) { return hv_phys_ptr(p->m, pa, 4); }

static uint32_t reg(HvPvgLite *p, uint32_t off) { return p->regs[(off - 0x1000) / 4]; }

static void do_root(HvPvgLite *p) {
    uint64_t buf = (uint64_t)reg(p, 0x1030) << 14;
    uint32_t off = reg(p, 0x1010), size = reg(p, 0x1004), wp = reg(p, 0x1008);
    if (!buf || size <= off) return;
    uint32_t ring = size - off;
    while (p->root_rp != wp) {
        uint32_t at = p->root_rp % ring, hdr[3];
        for (int i = 0; i < 3; i++) {
            uint32_t *w = guest32(p, buf + off + (at + 4u * i) % ring);
            hdr[i] = w ? *w : 0;
        }
        if (hdr[1] < 12 || hdr[1] > ring) {
            fprintf(stderr, "hengevm: pvg-lite: bad root command %x length %x\n", hdr[0], hdr[1]);
            p->root_rp = wp;
            break;
        }
        if (p->log) fprintf(stderr, "hengevm: pvg-lite: root op %x len %x stamp %x\n", hdr[0], hdr[1], hdr[2]);
        if ((hdr[0] & 0xffff) == 0x38 && hdr[1] >= 28) {
            uint32_t *id = guest32(p, buf + off + (at + 12) % ring), *page = guest32(p, buf + off + (at + 24) % ring);
            if (id && page && *id >> 1 < 512) p->task_root[*id >> 1] = *page;
        }
        if ((hdr[0] & 0xffff) == 0x3a && hdr[1] >= 24) {
            uint32_t *count = guest32(p, buf + off + (at + 12) % ring), *page = guest32(p, buf + off + (at + 20) % ring);
            uint32_t *dst = page ? hv_phys_ptr(p->m, (uint64_t)*page << 14, sizeof device_info) : NULL;
            if (dst && count) {
                uint32_t n = sizeof device_info / sizeof device_info[0];
                memcpy(dst, device_info, sizeof device_info);
                if (p->log) fprintf(stderr, "hengevm: pvg-lite: device info: %u keys (asked %u)\n", n, *count);
            }
        }
        p->root_rp += hdr[1];
        uint32_t *stamp = guest32(p, buf);
        if (stamp) __atomic_store_n(stamp, hdr[2], __ATOMIC_RELEASE);
    }
}

/* Reads a command's bytes out of a ring made of scattered pages. */
static void ring_read(HvPvgLite *p, const uint32_t *table, uint32_t size, uint32_t at, void *to, uint32_t n) {
    uint8_t *d = to;
    while (n) {
        uint32_t o = at % size, c = 0x4000 - (o & 0x3fff);
        if (c > n) c = n;
        uint8_t *q = hv_phys_ptr(p->m, ((uint64_t)table[o >> 14] << 14) + (o & 0x3fff), c);
        if (q) memcpy(d, q, c);
        else memset(d, 0, c);
        d += c, at += c, n -= c;
    }
}

/* The one mode the display offers. macOS takes the size it is given at boot. */
static uint32_t display_w = 1920, display_h = 1080;
/* The panel's real size, when known: macOS works out the pixel density from it and
   picks HiDPI (2x) for a Retina-dense panel, as on a real Mac. 0 = the old 400 mm. */
static uint32_t display_mm_w, display_mm_h;

void hv_pvg_lite_set_size_mm(uint32_t width_mm, uint32_t height_mm) {
    display_mm_w = width_mm;
    display_mm_h = height_mm;
}

void hv_pvg_lite_set_mode(uint32_t width, uint32_t height) {
    display_w = width;
    display_h = height;
}

void hv_pvg_lite_get_mode(uint32_t *width, uint32_t *height) {
    *width = display_w;
    *height = display_h;
}

void hv_pvg_lite_cursor_override(HvPvgLite *p, uint32_t where) {
    hv_pvg_metal_cursor_override(p ? atomic_load_explicit(&p->metal, memory_order_acquire) : NULL, where);
}

static void display_set_page(HvPvgLite *p, uint64_t pa) {
    uint8_t *d = hv_phys_ptr(p->m, pa, 0x4000);
    if (!d) return;
    p->display_pa = pa;
#define PUT32(off, v) (*(uint32_t *)(d + (off)) = (uint32_t)(v))
#define PUTF(off, v) (*(float *)(d + (off)) = (v))
    PUT32(0x00, 1);
    uint32_t width_mm = 400, height_mm = 400 * display_h / display_w; /* the same pixel density at any shape */
    if (display_mm_w && display_mm_h) width_mm = display_mm_w, height_mm = display_mm_h;
    PUT32(0x14, height_mm << 16 | width_mm);
    PUT32(0x18, 0x01000100);
    PUT32(0x1c, 0x400);
    PUT32(0x20, 3);
    PUTF(0x24, (float)width_mm);
    PUTF(0x28, (float)height_mm);
    PUT32(0x208, 1); /* one mode: */
    PUT32(0x210, display_h << 16 | display_w);
    {   /* m4 fork: 120 Hz on ProMotion (HV_HZ), so WindowServer animates at the panel's rate */
        const char *hz = getenv("HV_HZ");
        uint32_t r = hz && atoi(hz) > 0 ? (uint32_t)atoi(hz) : 60;
        PUT32(0x214, r << 16);
    }
    PUTF(0xa10, 1.0f); /* identity colour matrix */
    PUTF(0xa24, 1.0f);
    PUTF(0xa38, 1.0f);
    d[0xa73] = 1;
    PUT32(0x200, 1); /* the mode list's sequence number, which the guest acks */
    d[0xfa9] = 1;    /* ack pending */
    PUT32(0xfc0, 1); /* state: told the guest, waiting for the ack */
    __atomic_store_n((uint32_t *)(d + 0x100), 4, __ATOMIC_RELEASE); /* mode change: read when the guest sets its mask */
#undef PUT32
#undef PUTF
}

static void display_ack(HvPvgLite *p, uint32_t seq) {
    uint8_t *d = p->display_pa ? hv_phys_ptr(p->m, p->display_pa, 0x4000) : NULL;
    if (!d) return;
    *(uint32_t *)(d + 0xfbc) = seq;
    d[0xfa9] = 0;
    *(uint32_t *)(d + 0xfc0) = 2;
}

static int task_read(HvPvgLite *p, uint32_t task, uint64_t off, void *to, uint32_t n);
static void display_swap(HvPvgLite *p, uint64_t mapping, const uint32_t *args) {
    uint8_t *d = p->display_pa ? hv_phys_ptr(p->m, p->display_pa, 0x4000) : NULL;
    if (!d) return;
    memcpy(d + 0xf80, args + 4, 16); /* gamma table offset, length */
    memcpy(d + 0xf90, args + 8, 8);  /* gamma entries, sum */
    {   /* m4 fork: say what the gamma table is whenever it changes (colour is washed out without it) */
        static uint32_t last[6];
        if (memcmp(last, args + 4, sizeof last)) {
            memcpy(last, args + 4, sizeof last);
            uint64_t off = (uint64_t)args[5] << 32 | args[4], len = (uint64_t)args[7] << 32 | args[6];
            fprintf(stderr, "hvgamma: swap %llu port %u w1 %#x off %#llx len %#llx count %u sum %#x\n",
                    (unsigned long long)p->swaps, args[0], args[1], (unsigned long long)off, (unsigned long long)len, args[8], args[9]);
            fprintf(stderr, "hvgamma: args");
            for (int i = 0; i < 10; i++) fprintf(stderr, " %08x", args[i]);
            fprintf(stderr, "\n");
            const uint8_t *g = off ? hv_phys_ptr(p->m, off, 64) : NULL;
            if (g) {
                fprintf(stderr, "hvgamma: phys@off");
                for (int i = 0; i < 64; i++) fprintf(stderr, "%s%02x", i % 4 ? "" : " ", g[i]);
                fprintf(stderr, "\n");
            }
            if (off && off + 64 <= 0x4000) {
                fprintf(stderr, "hvgamma: page@off");
                for (int i = 0; i < 64; i++) fprintf(stderr, "%s%02x", i % 4 ? "" : " ", d[off + i]);
                fprintf(stderr, "\n");
            }
            for (uint32_t t = 0, shown = 0; off && t < 512 && shown < 8; t++) {
                uint8_t g[96];
                if (task_read(p, t, off, g, sizeof g) != 0) continue;
                shown++;
                fprintf(stderr, "hvgamma: task %u@off", t);
                for (int i = 0; i < 96; i++) fprintf(stderr, "%s%02x", i % 4 ? "" : " ", g[i]);
                fprintf(stderr, "\n");
                uint8_t mid[48]; /* the middle and end of the table too, whatever its layout */
                if (task_read(p, t, off + 0x600, mid, sizeof mid) == 0) {
                    fprintf(stderr, "hvgamma: task %u@off+600", t);
                    for (int i = 0; i < 48; i++) fprintf(stderr, "%s%02x", i % 4 ? "" : " ", mid[i]);
                    fprintf(stderr, "\n");
                }
                if (task_read(p, t, off + 0xbf0, mid, sizeof mid) == 0) {
                    fprintf(stderr, "hvgamma: task %u@off+bf0", t);
                    for (int i = 0; i < 48; i++) fprintf(stderr, "%s%02x", i % 4 ? "" : " ", mid[i]);
                    fprintf(stderr, "\n");
                }
            }
            fprintf(stderr, "hvgamma: state a00..a80");
            for (int i = 0xa00; i < 0xa80; i += 4) fprintf(stderr, " %08x", *(uint32_t *)(d + i));
            fprintf(stderr, "\n");
        }
    }
    p->swaps++;
    *(uint32_t *)(d + 0xf98) = (uint32_t)p->swaps;
    *(uint32_t *)(d + 0xfb0) = (uint32_t)mapping;
    __atomic_fetch_or((uint32_t *)(d + 0x100), 2, __ATOMIC_RELEASE);
    if (p->log) fprintf(stderr, "hengevm: pvg-lite: swap %llu: mapping %llu\n", (unsigned long long)p->swaps, (unsigned long long)mapping);
}

/* Task memory: the root page holds {table page, depth}; each level has 4096
   32-bit page numbers, so one level maps 64 MB. Returns a host pointer valid
   to the end of the guest's 16 KB page. */
static uint8_t *task_ptr(HvPvgLite *p, uint32_t task, uint64_t off, uint32_t *avail) {
    if (task >= 512 || !p->task_root[task]) return NULL;
    uint32_t *root = hv_phys_ptr(p->m, (uint64_t)p->task_root[task] << 14, 0x4000);
    if (!root) return NULL;
    uint32_t page = root[0], depth = root[1];
    if (depth < 1 || depth > 3) return NULL;
    uint64_t vpn = off >> 14;
    for (int level = (int)depth - 1; level >= 0; level--) {
        uint32_t *t = page ? hv_phys_ptr(p->m, (uint64_t)page << 14, 0x4000) : NULL;
        if (!t) return NULL;
        page = t[(vpn >> (12 * level)) & 0xfff] & 0x7fffffff; /* bit 31 is a flag */
    }
    if (!page) return NULL;
    uint32_t in = (uint32_t)(off & 0x3fff);
    if (avail) *avail = 0x4000 - in;
    return hv_phys_ptr(p->m, ((uint64_t)page << 14) + in, 0x4000 - in);
}

static int task_read(HvPvgLite *p, uint32_t task, uint64_t off, void *to, uint32_t n) {
    uint8_t *d = to;
    while (n) {
        uint32_t avail = 0;
        uint8_t *q = task_ptr(p, task, off, &avail);
        if (!q) return -1;
        uint32_t c = n < avail ? n : avail;
        memcpy(d, q, c);
        d += c, off += c, n -= c;
    }
    return 0;
}

int hv_pvg_lite_task_read(HvPvgLite *p, uint32_t task, uint64_t offset, void *to, uint32_t length) {
    return task_read(p, task, offset, to, length);
}

uint32_t hv_pvg_lite_task_page(HvPvgLite *p, uint32_t task, uint64_t offset) {
    if (task >= 512 || !p->task_root[task]) return 0;
    uint32_t *root = hv_phys_ptr(p->m, (uint64_t)p->task_root[task] << 14, 0x4000);
    if (!root) return 0;
    uint32_t page = root[0], depth = root[1];
    if (depth < 1 || depth > 3) return 0;
    uint64_t vpn = offset >> 14;
    for (int level = (int)depth - 1; level >= 0; level--) {
        uint32_t *t = page ? hv_phys_ptr(p->m, (uint64_t)page << 14, 0x4000) : NULL;
        if (!t) return 0;
        page = t[(vpn >> (12 * level)) & 0xfff] & 0x7fffffff;
    }
    return page;
}

/* n pages' numbers from a task page on, 0 for those not mapped: a table at a time, for it is asked often. */
void hv_pvg_lite_task_pages(HvPvgLite *p, uint32_t task, uint64_t first, uint64_t n, uint32_t *out) {
    memset(out, 0, n * 4);
    if (task >= 512 || !p->task_root[task]) return;
    uint32_t *root = hv_phys_ptr(p->m, (uint64_t)p->task_root[task] << 14, 0x4000);
    if (!root) return;
    uint32_t top = root[0], depth = root[1];
    if (depth < 1 || depth > 3) return;
    for (uint64_t i = 0; i < n;) {
        uint64_t vpn = first + i;
        uint32_t page = top;
        for (int level = (int)depth - 1; level >= 1 && page; level--) {
            uint32_t *t = hv_phys_ptr(p->m, (uint64_t)page << 14, 0x4000);
            page = t ? t[(vpn >> (12 * level)) & 0xfff] & 0x7fffffff : 0;
        }
        uint32_t *leaf = page ? hv_phys_ptr(p->m, (uint64_t)page << 14, 0x4000) : NULL;
        uint64_t in = vpn & 0xfff, run = 4096 - in;
        if (run > n - i) run = n - i;
        if (leaf)
            for (uint64_t k = 0; k < run; k++) out[i + k] = leaf[in + k] & 0x7fffffff;
        i += run;
    }
}

uint8_t *hv_pvg_lite_page(HvPvgLite *p, uint32_t guest_page) {
    return hv_phys_ptr(p->m, (uint64_t)guest_page << 14, 0x4000);
}

void *hv_pvg_lite_phys(HvPvgLite *p, uint64_t pa, uint64_t length) {
    return hv_phys_ptr(p->m, pa, length);
}

/* macOS 15 and later name each task's object list with command 0x44. Older guest drivers (macOS 14 Sonoma
   seen) never send it: their list is at task offset 0x4000, the same 12-byte entries (found by probing). */
#define OLD_DRIVER_OBJECTS 0x4000
uint64_t hv_pvg_lite_task_objects(HvPvgLite *p, uint32_t task) {
    if (task >= 512) return 0;
    if (!p->task_objects[task] && p->task_root[task]) {
        if (!p->old_driver) {
            p->old_driver = 1;
            fprintf(stderr, "hengevm: pvg-lite: an older guest GPU driver (no object list command): its own layouts\n");
        }
        return OLD_DRIVER_OBJECTS;
    }
    return p->task_objects[task];
}

int hv_pvg_lite_old_driver(HvPvgLite *p) { return p->old_driver; }

/* A stamp Metal finished: the guest sees it and the next poll raises the interrupt. */
void hv_pvg_lite_complete(HvPvgLite *p, uint32_t ch, uint32_t stamp) {
    uint64_t buf = (uint64_t)reg(p, 0x1030) << 14;
    uint32_t *s = buf ? guest32(p, buf + 4 * ch) : NULL;
    if (s) __atomic_store_n(s, stamp, __ATOMIC_RELEASE);
    /* The interrupt now, from this thread: the GIC wakes the CPU it goes to. Left for
       CPU 0's next poll this could wait for its timer, up to 10 ms if it is idle, and
       the guest waits for this between the many batches of work in a frame. */
    __atomic_fetch_or(&p->status, 1u << ch, __ATOMIC_ACQ_REL);
    hv_gicv3_pend(p->gic, p->gfx_irq);
    if (p->m->ncpus == 1) hv_machine_wake(p->m);
}

void hv_pvg_lite_set_layer(HvPvgLite *p, void *layer) {
    if (!p) return;
    CFTypeRef retained = layer ? CFRetain(layer) : NULL;
    os_unfair_lock_lock(&p->layer_lock);
    CFTypeRef previous = p->layer;
    p->layer = retained;
    p->layer_set = 1;
    HvPvgMetal *metal = atomic_load_explicit(&p->metal, memory_order_acquire);
    if (metal) hv_pvg_metal_set_layer(metal, layer);
    os_unfair_lock_unlock(&p->layer_lock);
    if (previous) CFRelease(previous);
}

void hv_pvg_lite_barrier_stats(HvPvgLite *p, uint64_t out[4]) {
    out[0] = p->barriers, out[1] = p->barrier_drains, out[2] = p->barrier_retries, out[3] = p->barrier_timeouts;
}

uint64_t hv_pvg_lite_gpu_frames(HvPvgLite *p) {
    return p && p->metal ? hv_pvg_metal_gpu_frames(p->metal) : 0;
}

int hv_pvg_lite_snapshot(HvPvgLite *p, uint8_t *bgra, uint32_t width, uint32_t height) {
    return p && p->metal ? hv_pvg_metal_snapshot(p->metal, bgra, width, height) : -1;
}

int hv_pvg_lite_frame(HvPvgLite *p, const uint8_t **pixels, uint32_t *width, uint32_t *height) {
    return p && p->metal ? hv_pvg_metal_frame(p->metal, pixels, width, height) : -1;
}

static void tally_object(HvPvgLite *p, uint32_t task, uint32_t ref, const char *why) {
    if (task >= 512 || ref >= 8192 || !ref || p->obj_seen[task][ref]) return;
    p->obj_seen[task][ref] = 1;
    uint32_t e[3] = {0}, pl[6] = {0};
    if (task_read(p, task, p->task_objects[task] + ref * 12ull, e, 12)) return;
    task_read(p, task, p->task_placement[task] + ref * 24ull, pl, 24);
    uint64_t off = (uint64_t)e[2] << 32 | e[1];
    uint32_t d[36] = {0};
    int have = task_read(p, task, off, d, sizeof d) == 0;
    fprintf(stderr, "pvgobj %s task %u ref %u entry %08x %08x %08x placement %08x %08x %08x %08x %08x %08x data(%d)", why, task, ref, e[0], e[1], e[2], pl[0], pl[1], pl[2], pl[3], pl[4], pl[5], have);
    if (have) for (int i = 0; i < 36; i++) fprintf(stderr, " %08x", d[i]);
    fprintf(stderr, "\n");
}

/* HV_PVG_TALLY: which serialized commands the guest uses. Encoder types: 0
   render, 1 compute, 2 blit, 4 info. */
static void tally_buffer(HvPvgLite *p, uint32_t task, uint64_t off, uint64_t len) {
    uint32_t hdr[2];
    uint64_t at = off, end = off + len;
    if (task_read(p, task, at, hdr, 8)) {
        if (p->bad++ < 6) {
            uint32_t *root = task < 512 && p->task_root[task] ? hv_phys_ptr(p->m, (uint64_t)p->task_root[task] << 14, 0x4000) : NULL;
            uint32_t *t0 = root ? hv_phys_ptr(p->m, (uint64_t)root[0] << 14, 0x4000) : NULL;
            if (t0) {
                fprintf(stderr, "pvgtally top:");
                for (int i = 0; i < 8; i++) fprintf(stderr, " %x", t0[i]);
                uint32_t *t1 = t0[0] ? hv_phys_ptr(p->m, (uint64_t)(t0[0] & 0x7fffffff) << 14, 0x4000) : NULL;
                if (t1) {
                    fprintf(stderr, " leaf[%llx]:", (unsigned long long)(off >> 14));
                    for (int i = -2; i < 4; i++) fprintf(stderr, " %x", t1[(off >> 14) + i]);
                }
                fprintf(stderr, "\n");
            }
            fprintf(stderr, "pvgtally read failed: task %u off %llx len %llx root page %x root %x %x %x %x %x %x\n", task, (unsigned long long)off, (unsigned long long)len,
                    task < 512 ? p->task_root[task] : 0, root ? root[0] : 0, root ? root[1] : 0, root ? root[2] : 0, root ? root[3] : 0, root ? root[4] : 0, root ? root[5] : 0);
        }
        return;
    }
    at += 8;
    uint32_t type = hdr[1] & 0xff, cont = (hdr[1] >> 8) & 0xff;
    if (type == 5) { /* protection options, then the real header */
        at += 8;
        if (task_read(p, task, at, hdr, 8)) { p->bad++; return; }
        at += 8;
        type = hdr[1] & 0xff, cont = (hdr[1] >> 8) & 0xff;
    }
    if (type > 5) { p->bad++; return; }
    uint64_t seg_end = at + hdr[0];
    if (seg_end > end) seg_end = end;
    if (type == 0 && !cont) { /* a render pass description comes first: {tag, size} records until tag 0xffffffff? */
        uint32_t r[2];
        if (task_read(p, task, at, r, 8)) { p->bad++; return; }
        p->counts[5][r[0] & 0xff]++; /* its first word, to learn its shape */
        static int shown;
        if (shown++ < 3) {
            uint32_t w[72] = {0};
            task_read(p, task, at, w, sizeof w);
            fprintf(stderr, "pvgpass");
            for (int i = 0; i < 72; i++) fprintf(stderr, " %08x", w[i]);
            fprintf(stderr, "\n");
        }
    }
    while (at + 8 <= seg_end) {
        uint32_t r[2];
        if (task_read(p, task, at, r, 8)) { p->bad++; return; }
        if (r[1] < 8 || at + r[1] > seg_end) { p->bad++; break; }
        if (r[0] < 256) p->counts[type][r[0]]++;
        if (type == 0) { /* the objects it names */
            uint32_t a[32] = {0}, c = r[1] - 8 > sizeof a ? sizeof a : r[1] - 8;
            task_read(p, task, at + 8, a, c);
            if (r[0] == 116) tally_object(p, task, a[0], "pipeline");
            if (r[0] == 114 || r[0] == 161) for (uint32_t i = 0; i < a[1] && i < 16; i++) tally_object(p, task, a[2 + i], "texture");
            if (r[0] == 112) for (uint32_t i = 0; i < a[1] && i < 16; i++) tally_object(p, task, a[2 + i], "sampler");
            if (r[0] == 125 || r[0] == 110 || r[0] == 157)
                for (uint32_t i = 0; i < a[1] && i < 6; i++) tally_object(p, task, *(uint32_t *)((uint8_t *)a + 8 + i * 12), "buffer");
            if (r[0] == 7) tally_object(p, task, a[1], "indexbuf");
        }
        at += r[1];
    }
}

static void tally_exec(HvPvgLite *p, const uint32_t *table, uint32_t size, uint32_t rd, uint32_t len, uint32_t skip) {
    uint32_t a[3];
    ring_read(p, table, size, rd + 12 + skip, a, 12);
    uint32_t task = a[0], resources = a[1], buffers = a[2];
    if (buffers > 64 || 12 + skip + 12 + resources * 8 + buffers * 16 > len) { p->bad++; return; }
    for (uint32_t i = 0; i < buffers; i++) {
        uint64_t b[2];
        ring_read(p, table, size, rd + 12 + skip + 12 + resources * 8 + i * 16, b, 16);
        tally_buffer(p, task, b[0], b[1]);
    }
    p->execs++;
    if (p->execs % 2000 == 0) {
        fprintf(stderr, "pvgtally execs %llu bad %llu\n", (unsigned long long)p->execs, (unsigned long long)p->bad);
        for (int t = 0; t < 6; t++)
            for (int i = 0; i < 256; i++)
                if (p->counts[t][i]) fprintf(stderr, "pvgtally %d %d %llu\n", t, i, (unsigned long long)p->counts[t][i]);
    }
}

/* Commands that do something even without a GPU to run them. */
static void channel_command(HvPvgLite *p, const uint32_t *table, uint32_t size, uint32_t rd, const uint32_t *hdr) {
    uint32_t op = hdr[0] & 0xffff, args[32], skip = (hdr[0] >> 16) * 8; /* the barriers come first */
    uint32_t n = hdr[1] > 12 + skip ? hdr[1] - 12 - skip : 0;
    if (n > sizeof args) n = sizeof args;
    {   /* HV_PVG_OPS=1: the first of each channel command the guest sends, with its words (a driver whose commands differ) */
        static int on = -1;
        static uint8_t seen[0x10000 / 8];
        if (on < 0) on = getenv("HV_PVG_OPS") != NULL;
        if (on && !(seen[op / 8] & (1 << (op % 8)))) {
            seen[op / 8] |= (uint8_t)(1 << (op % 8));
            uint32_t w[16] = {0};
            ring_read(p, table, size, rd + 12 + skip, w, n < sizeof w ? n : sizeof w);
            fprintf(stderr, "pvgop 0x%x len %u skip %u:", op, hdr[1], skip);
            for (uint32_t i = 0; i < 16 && i * 4 < n; i++) fprintf(stderr, " %08x", w[i]);
            fprintf(stderr, "\n");
        }
    }
    if (op == 0x38) {
        ring_read(p, table, size, rd + 12 + skip, args, 16);
        if (args[0] >> 1 < 512) p->task_root[args[0] >> 1] = args[3];
        return;
    }
    if (op == 0x44) {
        ring_read(p, table, size, rd + 12 + skip, args, 28);
        if (args[0] < 512) {
            p->task_objects[args[0]] = (uint64_t)args[2] << 32 | args[1];
            p->task_placement[args[0]] = (uint64_t)args[4] << 32 | args[3];
            if (p->tally) fprintf(stderr, "pvgobj list task %u objects %llx len %x placement %llx len %x\n", args[0], (unsigned long long)p->task_objects[args[0]], args[5], (unsigned long long)p->task_placement[args[0]], args[6]);
        }
        return;
    }
    if (op == 0x43 && p->tally) tally_exec(p, table, size, rd, hdr[1], skip);
    if (p->metal) return; /* in order, on its worker */
    if (op != 1 && op != 2 && op != 8) return;
    memset(args, 0, sizeof args);
    ring_read(p, table, size, rd + 12 + skip, args, n);
    if (op == 1) display_set_page(p, (uint64_t)args[1] << 14);
    else if (op == 2) display_ack(p, args[1]);
    else display_swap(p, (uint64_t)args[3] << 32 | args[2], args);
}

/* The mapper's table: a u32 per 16 KB of descriptors, physical address >> 12 with bit 0 meaning present. */
const uint8_t *hv_pvg_lite_surface(HvPvgLite *p, uint32_t mapping) {
    /* macOS 14's driver writes 0x200-byte descriptions, 32 to a page; later ones 0x400, 16 to a page. Reading the
       older ones as 0x400 apart put id 2k on the first half of "slot" k and never read id 2k+1 at all. */
    uint32_t per = p->old_driver ? 32 : 16, size = 0x4000 / per;
    uint64_t table = p->io_regs[0x10 / 8];
    uint32_t *e = table ? hv_phys_ptr(p->m, table + (mapping / per) * 4ull, 4) : NULL;
    if (!e || !(*e & 1)) return NULL;
    return hv_phys_ptr(p->m, ((uint64_t)(*e & ~1u) << 12) + (mapping % per) * (uint64_t)size, size);
}

uint8_t *hv_pvg_lite_display_page(HvPvgLite *p) {
    return p->display_pa ? hv_phys_ptr(p->m, p->display_pa, 0x4000) : NULL;
}

void hv_pvg_lite_command(HvPvgLite *p, const uint32_t *cmd, uint32_t len) {
    uint32_t op = cmd[0] & 0xffff, skip = (cmd[0] >> 16) * 8;
    uint32_t n = len > 12 + skip ? len - 12 - skip : 0;
    const uint32_t *args = (const uint32_t *)((const uint8_t *)cmd + 12 + skip);
    if (op == 1 && n >= 8) display_set_page(p, (uint64_t)args[1] << 14);
    else if (op == 2 && n >= 8) display_ack(p, args[1]);
    else if (op == 8 && n >= 16) display_swap(p, (uint64_t)args[3] << 32 | args[2], args);
}

static void do_channel(HvPvgLite *p, uint32_t ch) {
    uint64_t root = (uint64_t)reg(p, 0x101c) << 14, buf = (uint64_t)reg(p, 0x1030) << 14;
    if (!root || !buf || !ch) return;
    uint32_t *rec = hv_phys_ptr(p->m, root + 0x400 + 0x14 * (ch - 1), 0x14);
    if (!rec) return;
    uint32_t *table = hv_phys_ptr(p->m, (uint64_t)rec[4] << 14, 0x4000);
    if (!table) return;
    uint32_t pages = 0;
    while (pages < 0x1000 && table[pages]) pages++;
    if (!pages) return;
    if (ch >= MAX_CHANNELS || (p->draining & (1u << ch))) return; /* already being drained, further up */
    p->draining |= 1u << ch;
    uint32_t size = pages << 14, wr = __atomic_load_n(&rec[0], __ATOMIC_ACQUIRE), rd = rec[1], last = 0;
    int any = 0;
    while (rd != wr) {
        uint32_t hdr[3];
        for (int i = 0; i < 3; i++) {
            uint32_t at = (rd + 4u * i) % size;
            uint32_t *w = guest32(p, ((uint64_t)table[at >> 14] << 14) + (at & 0x3fff));
            hdr[i] = w ? *w : 0;
        }
        if (hdr[1] < 12 || hdr[1] > size) {
            fprintf(stderr, "hengevm: pvg-lite: channel %u: bad command %x length %x at %x\n", ch, hdr[0], hdr[1], rd);
            rd = wr;
            break;
        }
        if (p->log) fprintf(stderr, "hengevm: pvg-lite: ch %u op %x len %x stamp %x\n", ch, hdr[0], hdr[1], hdr[2]);
        {   /* HV_PVG_TRACE_FILE exists: the order commands reach the device in, with what a swap says */
            if (hv_pvg_trace_on()) {
                uint32_t w[12] = {0};
                ring_read(p, table, size, rd + 12, w, hdr[1] - 12 < sizeof w ? hdr[1] - 12 : sizeof w);
                fprintf(stderr, "trace: dev ch %u op %x len %u stamp %x sub %x args %x %x %x %x %x %x %x %x\n", ch, hdr[0], hdr[1], hdr[2], p->submitted[ch], w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
            }
        }
        os_unfair_lock_lock(&p->layer_lock);
        if (!p->metal_tried) {
            p->metal_tried = 1;
            HvPvgMetal *metal = getenv("HV_PVG_NO_METAL") ? NULL : hv_pvg_metal_create(p);
            if (metal && p->layer_set) hv_pvg_metal_set_layer(metal, (void *)p->layer);
            atomic_store_explicit(&p->metal, metal, memory_order_release);
        }
        os_unfair_lock_unlock(&p->layer_lock);
        int blocked = 0;
        for (uint32_t k = 0; p->metal && k < hdr[0] >> 16 && !blocked; k++) {
            /* {channel, stamp}, as many as bits 31..16 say: this waits for that channel's
               work, such as a swap for the drawing of the frame it shows. All of it goes
               through one worker in order, so everything up to the stamp has to be handed
               over first, or the screen shows a frame half drawn. */
            uint32_t b[2] = {0, 0};
            ring_read(p, table, size, rd + 12 + 8 * k, b, 8);
            if (b[0] && b[0] < MAX_CHANNELS && b[0] != ch) {
                p->barriers++;
                if ((int32_t)(p->submitted[b[0]] - b[1]) < 0) {
                    p->barrier_drains++;
                    do_channel(p, b[0]);
                }
                if ((int32_t)(p->submitted[b[0]] - b[1]) < 0) {
                    if (++p->barrier_waits[ch] < 20000) { /* not written yet: this channel waits for a later poll */
                        p->barrier_retries++;
                        p->chan_pending |= 1u << ch;
                        blocked = 1;
                        break;
                    }
                    p->barrier_timeouts++;
                    fprintf(stderr, "hengevm: pvg-lite: channel %u gave up waiting for channel %u stamp %x (at %x)\n", ch, b[0], b[1], p->submitted[b[0]]);
                }
                p->barrier_waits[ch] = 0;
            }
        }
        if (blocked) break;
        channel_command(p, table, size, rd, hdr);
        p->submitted[ch] = hdr[2];
        if (p->metal) {
            uint32_t *copy = malloc(hdr[1]);
            ring_read(p, table, size, rd, copy, hdr[1]);
            hv_pvg_metal_submit(p->metal, p, ch, copy, hdr[1]);
        } else {
            last = hdr[2], any = 1;
        }
        rd += hdr[1];
    }
    __atomic_store_n(&rec[1], rd, __ATOMIC_RELEASE);
    p->draining &= ~(1u << ch);
    if (any) {
        uint32_t *stamp = guest32(p, buf + 4 * ch);
        if (stamp) __atomic_store_n(stamp, last, __ATOMIC_RELEASE);
        __atomic_fetch_or(&p->status, 1u << ch, __ATOMIC_ACQ_REL);
    }
}

/* The mapper's ring: surfaces coming and going. The guest waits for its interrupt. */
static void do_mapper(HvPvgLite *p) {
    if (p->tally) {
        uint64_t ring = p->io_regs[0], table = p->io_regs[0x10 / 8], size = p->io_regs[0x8 / 8], want = p->io_regs[0x18 / 8];
        for (uint64_t i = p->io_regs[0x20 / 8]; i < want && size; i++) {
            uint32_t *e = hv_phys_ptr(p->m, ring + (i % size) * 16, 16);
            uint32_t *t = table ? hv_phys_ptr(p->m, table, 64) : NULL;
            if (!e || !t) continue;
            fprintf(stderr, "pvgmap op %u id %u table %x %x %x %x\n", e[0], e[1], t[0], t[1], t[2], t[3]);
            uint32_t seg = e[1] / 16, idx = e[1] % 16;
            uint32_t *ta = hv_phys_ptr(p->m, table + seg * 4, 4);
            uint32_t *d = ta ? hv_phys_ptr(p->m, ((uint64_t)(*ta & ~1u) << 12) + idx * 0x400ull, 0x400) : NULL;
            if (d && e[0] == 1) {
                fprintf(stderr, "pvgdesc");
                for (int k = 0; k < 64; k++) fprintf(stderr, " %08x", d[k]);
                fprintf(stderr, "\n");
            }
        }
    }
    if (p->metal) {
        uint64_t ring = p->io_regs[0], size = p->io_regs[0x8 / 8], want = p->io_regs[0x18 / 8];
        for (uint64_t i = p->io_regs[0x20 / 8]; i < want && size; i++) {
            uint32_t *e = hv_phys_ptr(p->m, ring + (i % size) * 16, 16);
            if (e && e[0] == 2) hv_pvg_metal_unmap(p->metal, e[1]);
        }
    }
    p->io_regs[0x20 / 8] = p->io_regs[0x18 / 8];
    hv_gicv3_pend(p->gic, p->iosfc_irq);
}

/* Device work no doorbell took care of: a channel that had to wait for another one's work. */
void hv_pvg_lite_poll(HvPvgLite *p) {
    if (!p) return;
    uint32_t chans = p->chan_pending;
    if (chans) {
        p->chan_pending = 0;
        for (uint32_t ch = 1; ch < MAX_CHANNELS; ch++)
            if (chans & (1u << ch)) do_channel(p, ch);
        if (__atomic_load_n(&p->status, __ATOMIC_ACQUIRE)) hv_gicv3_pend(p->gic, p->gfx_irq);
    }
}

static uint64_t gfx_read(void *opaque, uint64_t off, unsigned size) {
    HvPvgLite *p = opaque;
    uint64_t v = 0;
    switch (off) {
    case 0x100c:
        do_root(p); /* the driver spins on this waiting for ring space */
        v = p->root_rp;
        break;
    case 0x1018:
        v = __atomic_exchange_n(&p->status, 0, __ATOMIC_ACQ_REL);
        break;
    case 0x122c: v = 1; break;
    default:
        if (off >= 0x1000 && off < 0x2000) v = p->regs[(off - 0x1000) / 4];
        break;
    }
    (void)size;
    return v;
}

static void gfx_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    HvPvgLite *p = opaque;
    (void)size;
    if (off >= 0x1000 && off < 0x2000) p->regs[(off - 0x1000) / 4] = (uint32_t)v;
    /* Doorbells are answered here, on the CPU that rang: no waiting for CPU 0 to come by. */
    if (off == 0x1008) {
        do_root(p);
        hv_gicv3_pend(p->gic, p->gfx_irq);
    } else if (off == 0x1020 && v < MAX_CHANNELS) {
        do_channel(p, (uint32_t)v);
        if (__atomic_load_n(&p->status, __ATOMIC_ACQUIRE)) hv_gicv3_pend(p->gic, p->gfx_irq);
    } else if (p->log && off != 0x1020) {
        fprintf(stderr, "hengevm: pvg-lite: write +%llx = %llx\n", (unsigned long long)off, (unsigned long long)v);
    }
    if (off == 0x1220 && p->metal) hv_pvg_metal_cursor_moved(p->metal); /* the display's doorbell: the pointer moved */
}

static uint64_t iosfc_read(void *opaque, uint64_t off, unsigned size) {
    HvPvgLite *p = opaque;
    (void)size;
    if (off == 0x1028) return 1;
    return off >= 0x1000 && off < 0x1040 ? p->io_regs[(off - 0x1000) / 8] : 0;
}

static void iosfc_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    HvPvgLite *p = opaque;
    (void)size;
    if (off >= 0x1000 && off < 0x1040) p->io_regs[(off - 0x1000) / 8] = v;
    if (off == 0x1018) do_mapper(p);
}

HvPvgLite *hv_pvg_lite_create(HvMachine *m, HvGic *gic, uint64_t gfx_pa, uint64_t iosfc_pa, int gfx_irq, int iosfc_irq) {
    HvPvgLite *p = calloc(1, sizeof *p);
    p->layer_lock = OS_UNFAIR_LOCK_INIT;
    atomic_init(&p->metal, NULL);
    p->m = m;
    p->gic = gic;
    p->gfx_irq = gfx_irq;
    p->iosfc_irq = iosfc_irq;
    p->log = getenv("HV_PVG_LITE_LOG") != NULL;
    p->tally = getenv("HV_PVG_TALLY") != NULL;
    hv_machine_add_mmio(m, gfx_pa, 0x10000, p, gfx_read, gfx_write, "gfx");
    hv_machine_add_mmio(m, iosfc_pa, 0x10000, p, iosfc_read, iosfc_write, "iosurface");
    return p;
}

void hv_pvg_lite_destroy(HvPvgLite *p) {
    if (!p) return;
    hv_pvg_metal_destroy(atomic_exchange(&p->metal, NULL));
    if (p->layer) CFRelease(p->layer);
    free(p);
}
