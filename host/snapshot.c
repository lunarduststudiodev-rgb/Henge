/* Instant resume: the machine to a file and back (see snapshot.h).

   The file: a header, then sections {tag, length, bytes} in a fixed order, then a trailer naming the file's length,
   written last, so a file cut short (the app killed while saving) is never taken for a whole one. It is written under
   another name and renamed into place.

   RAM goes in 4 MB chunks: a bitmap of the 16 KB pages that hold anything, then those pages, LZ4-compressed. Pages
   the guest never touched (mincore: not resident and never paged out) are not read at all, and all-zero pages are
   left out, so a restore writes only pages that had something and a fresh machine's untouched RAM stays untouched.
   Chunks are compressed and decompressed several at a time, on every core. */

#include "snapshot.h"

#include <compression.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include "pvg_lite.h"
#include "sysreg.h"
#include "vm.h"

#define SNAP_MAGIC "HENGESN1"
#define SNAP_VERSION 1
#define PAGE 0x4000ull
#define CHUNK_PAGES 256u
#define CHUNK_BYTES ((uint64_t)CHUNK_PAGES * PAGE)
#define BATCH 8 /* chunks compressed at once: 2 x 32 MB of buffers */

#define TAG(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

typedef struct SnapHeader {
    char magic[8];
    uint32_t version, header_bytes;
    uint64_t fingerprint;
    char ident[1024];
    uint64_t guest_counter; /* the virtual counter when it stopped: it goes on from there */
    int64_t saved_at;       /* host wall clock, seconds */
    int64_t paused_wall_ns; /* the wall clock when the CPUs stopped */
    uint32_t ncpus, nram;
    uint64_t ram_size;
    char disk[256];         /* hv_vm_disk_identity when saved: the disk has to be exactly that again */
} SnapHeader;

typedef struct SecHdr {
    uint32_t tag, flags;
    uint64_t len;
} SecHdr;

typedef struct Trailer {
    uint32_t tag, pad;
    uint64_t total; /* the file's length, this included */
} Trailer;

typedef struct RamHdr {
    uint64_t pa, size;
    uint32_t chunk_pages, nchunks;
} RamHdr;

typedef struct ChunkHdr {
    uint32_t index, stored; /* pages in the chunk's bitmap */
    uint32_t bytes;         /* what follows */
    uint32_t raw;           /* 1: the pages as they are (LZ4 did not make them smaller) */
    uint8_t bitmap[CHUNK_PAGES / 8];
} ChunkHdr;

/* A CPU's architectural state: everything but its caches, which a new machine starts empty. */
typedef struct CpuState {
    uint64_t x[31], sp, pc, nzcv;
    __uint128_t v[32];
    uint64_t fpcr, fpsr;
    uint64_t tpidr_el0, tpidrro_el0, tpidr_el1, elr_el1, spsr_el1, sp_el0, daif, sp_el1, pstate_extra, sctlr_el1;
    uint64_t esr_el1, far_el1, tcr_el1, par_el1;
    uint32_t el0_cnt, spsel, el, off;
    uint64_t ttbr0_el1, ttbr1_el1, mair_el1, amair_el1, vbar_el1, afsr0_el1, afsr1_el1, contextidr_el1;
    uint64_t cpacr_el1, mdscr_el1, csselr_el1, cntkctl_el1, actlr_el1;
    uint64_t cntv_ctl, cntv_cval, cntp_ctl, cntp_cval;
} CpuState;

static int fail(char *err, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int fail(char *err, size_t n, const char *fmt, ...) {
    if (err && n) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, n, fmt, ap);
        va_end(ap);
    }
    return -1;
}

/* The layouts this build saves: a file from a build where any of them differs is refused. */
static uint64_t fingerprint(void) {
    uint64_t f = 0xcbf29ce484222325ull ^ SNAP_VERSION;
#define MIX(v) (f = (f ^ (uint64_t)(v)) * 0x100000001b3ull)
    MIX(sizeof(HvCpu)), MIX(sizeof(CpuState)), MIX(sizeof(HvGic)), MIX(sizeof(HvGicCpu)), MIX(offsetof(HvGic, ctlr));
    MIX(sizeof(HvXhci)), MIX(offsetof(HvXhci, usbcmd)), MIX(offsetof(HvXhci, mu)), MIX(offsetof(HvXhci, reports));
    MIX(sizeof(HvVirtio)), MIX(sizeof(HvVirtq)), MIX(sizeof(HvVirtioSnd)), MIX(sizeof(HvVirtioInput));
    MIX(sizeof(HvVirtioBalloon)), MIX(sizeof(HvPciDev)), MIX(sizeof(HvPcie)), MIX(sizeof(HvAes)), MIX(sizeof(HvBdif));
    MIX(sizeof(HvPl011)), MIX(sizeof(HvPl031)), MIX(sizeof(HvPl061)), MIX(sizeof(HvVmapple));
    MIX(sizeof(HvPowerSource));
    MIX(hv_pvg_lite_state(NULL, NULL, 0)), MIX(CHUNK_PAGES), MIX(PAGE);
#undef MIX
    return f;
}

/* ---- writing ---- */

typedef struct W {
    int fd, err;
    uint8_t *buf;
    size_t n, cap;
    uint64_t total;
} W;

static void w_flush(W *w) {
    size_t off = 0;
    while (!w->err && off < w->n) {
        ssize_t k = write(w->fd, w->buf + off, w->n - off);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) w->err = errno ? errno : EIO;
        else off += (size_t)k;
    }
    w->n = 0;
}

static void w_raw(W *w, const void *d, uint64_t len) {
    const uint8_t *p = d;
    while (len && !w->err) {
        size_t room = w->cap - w->n, k = len < room ? (size_t)len : room;
        memcpy(w->buf + w->n, p, k);
        w->n += k, p += k, len -= k, w->total += k;
        if (w->n == w->cap) w_flush(w);
    }
}

static void w_section(W *w, uint32_t tag, const void *d, uint64_t len) {
    SecHdr h = {tag, 0, len};
    w_raw(w, &h, sizeof h);
    w_raw(w, d, len);
}

/* A struct's bytes from one field to the end of another: fields between devices' host pointers and locks. */
#define RANGE(p, from, to) ((const uint8_t *)(p) + offsetof(__typeof__(*(p)), from)), \
    (offsetof(__typeof__(*(p)), to) + sizeof((p)->to) - offsetof(__typeof__(*(p)), from))

/* ---- reading ---- */

typedef struct R {
    int fd, err;
    uint8_t *buf;
    size_t pos, len, cap;
} R;

static int r_raw(R *r, void *d, uint64_t len) {
    uint8_t *p = d;
    while (len) {
        if (r->pos == r->len) {
            ssize_t k;
            do k = read(r->fd, r->buf, r->cap);
            while (k < 0 && errno == EINTR);
            if (k <= 0) return r->err = k < 0 ? errno : EIO, -1;
            r->pos = 0, r->len = (size_t)k;
        }
        size_t k = r->len - r->pos < len ? r->len - r->pos : (size_t)len;
        memcpy(p, r->buf + r->pos, k);
        r->pos += k, p += k, len -= k;
    }
    return 0;
}

static int r_section(R *r, uint32_t tag, void *d, uint64_t len) {
    SecHdr h;
    if (r_raw(r, &h, sizeof h) || h.tag != tag || h.len != len) return r->err = r->err ? r->err : EINVAL, -1;
    return r_raw(r, d, len);
}

/* A section of any length (up to max), in memory the caller frees. */
static int r_section_any(R *r, uint32_t tag, void **d, uint64_t *len, uint64_t max) {
    SecHdr h;
    *d = NULL, *len = 0;
    if (r_raw(r, &h, sizeof h) || h.tag != tag || h.len > max) return r->err = r->err ? r->err : EINVAL, -1;
    *d = malloc(h.len ? h.len : 1);
    if (!*d) return r->err = ENOMEM, -1;
    *len = h.len;
    return r_raw(r, *d, h.len);
}

#define R_RANGE(r, tag, p, from, to) r_section(r, tag, (uint8_t *)(p) + offsetof(__typeof__(*(p)), from), \
    offsetof(__typeof__(*(p)), to) + sizeof((p)->to) - offsetof(__typeof__(*(p)), from))

/* ---- CPUs ---- */

static void cpu_get(const HvCpu *c, CpuState *s) {
    memset(s, 0, sizeof *s);
    memcpy(s->x, c->x, sizeof s->x);
    s->sp = c->sp, s->pc = c->pc, s->nzcv = c->nzcv;
    memcpy(s->v, c->v, sizeof s->v);
    s->fpcr = c->fpcr, s->fpsr = c->fpsr;
    s->tpidr_el0 = c->tpidr_el0, s->tpidrro_el0 = c->tpidrro_el0, s->tpidr_el1 = c->tpidr_el1, s->elr_el1 = c->elr_el1;
    s->spsr_el1 = c->spsr_el1, s->sp_el0 = c->sp_el0, s->daif = c->daif, s->sp_el1 = c->sp_el1;
    s->pstate_extra = c->pstate_extra, s->sctlr_el1 = c->sctlr_el1, s->esr_el1 = c->esr_el1, s->far_el1 = c->far_el1;
    s->tcr_el1 = c->tcr_el1, s->par_el1 = c->par_el1, s->el0_cnt = c->el0_cnt, s->spsel = c->spsel, s->el = c->el;
    s->off = (uint32_t)c->off;
    s->ttbr0_el1 = c->ttbr0_el1, s->ttbr1_el1 = c->ttbr1_el1, s->mair_el1 = c->mair_el1, s->amair_el1 = c->amair_el1;
    s->vbar_el1 = c->vbar_el1, s->afsr0_el1 = c->afsr0_el1, s->afsr1_el1 = c->afsr1_el1, s->contextidr_el1 = c->contextidr_el1;
    s->cpacr_el1 = c->cpacr_el1, s->mdscr_el1 = c->mdscr_el1, s->csselr_el1 = c->csselr_el1, s->cntkctl_el1 = c->cntkctl_el1;
    s->actlr_el1 = c->actlr_el1;
    s->cntv_ctl = c->cntv_ctl, s->cntv_cval = c->cntv_cval, s->cntp_ctl = c->cntp_ctl, s->cntp_cval = c->cntp_cval;
}

static void cpu_put(HvCpu *c, const CpuState *s) {
    memcpy(c->x, s->x, sizeof s->x);
    c->sp = s->sp, c->pc = s->pc, c->nzcv = s->nzcv;
    memcpy(c->v, s->v, sizeof s->v);
    c->fpcr = s->fpcr, c->fpsr = s->fpsr;
    c->tpidr_el0 = s->tpidr_el0, c->tpidrro_el0 = s->tpidrro_el0, c->tpidr_el1 = s->tpidr_el1, c->elr_el1 = s->elr_el1;
    c->spsr_el1 = s->spsr_el1, c->sp_el0 = s->sp_el0, c->daif = s->daif, c->sp_el1 = s->sp_el1;
    c->pstate_extra = s->pstate_extra, c->sctlr_el1 = s->sctlr_el1, c->esr_el1 = s->esr_el1, c->far_el1 = s->far_el1;
    c->tcr_el1 = s->tcr_el1, c->par_el1 = s->par_el1, c->el0_cnt = s->el0_cnt, c->spsel = s->spsel, c->el = s->el;
    c->off = (int)s->off;
    c->parked_off = 0;
    c->ttbr0_el1 = s->ttbr0_el1, c->ttbr1_el1 = s->ttbr1_el1, c->mair_el1 = s->mair_el1, c->amair_el1 = s->amair_el1;
    c->vbar_el1 = s->vbar_el1, c->afsr0_el1 = s->afsr0_el1, c->afsr1_el1 = s->afsr1_el1, c->contextidr_el1 = s->contextidr_el1;
    c->cpacr_el1 = s->cpacr_el1, c->mdscr_el1 = s->mdscr_el1, c->csselr_el1 = s->csselr_el1, c->cntkctl_el1 = s->cntkctl_el1;
    c->actlr_el1 = s->actlr_el1;
    c->cntv_ctl = s->cntv_ctl, c->cntv_cval = s->cntv_cval, c->cntp_ctl = s->cntp_ctl, c->cntp_cval = s->cntp_cval;
    c->excl_addr = ~0ull; /* an LDXR before the save: its STXR fails and the guest tries again, as after any interrupt */
    c->exit_req = 0;
    atomic_store(&c->halted, HV_HALT_NONE);
}

/* ---- RAM ---- */

static int page_zero(const uint8_t *p) {
    const uint64_t *q = (const uint64_t *)p;
    for (size_t i = 0; i < PAGE / 8; i += 8)
        if (q[i] | q[i + 1] | q[i + 2] | q[i + 3] | q[i + 4] | q[i + 5] | q[i + 6] | q[i + 7]) return 0;
    return 1;
}

typedef struct Slot {
    ChunkHdr h;
    uint8_t *stage; /* the chunk's stored pages, one after another */
    uint8_t *out;   /* compressed */
    void *scratch;
} Slot;

static int slots_alloc(Slot *s, int n) {
    size_t scratch = compression_encode_scratch_buffer_size(COMPRESSION_LZ4);
    size_t dscratch = compression_decode_scratch_buffer_size(COMPRESSION_LZ4);
    if (dscratch > scratch) scratch = dscratch;
    for (int i = 0; i < n; i++) {
        s[i].stage = malloc(CHUNK_BYTES);
        s[i].out = malloc(CHUNK_BYTES);
        s[i].scratch = malloc(scratch ? scratch : 1);
        if (!s[i].stage || !s[i].out || !s[i].scratch) return -1;
    }
    return 0;
}

static void slots_free(Slot *s, int n) {
    for (int i = 0; i < n; i++) free(s[i].stage), free(s[i].out), free(s[i].scratch);
}

/* One chunk into a slot: which pages have something, and those pages compressed. */
static void chunk_pack(Slot *s, const uint8_t *base, uint64_t size, uint32_t index, int use_mincore) {
    uint64_t from = (uint64_t)index * CHUNK_BYTES;
    uint32_t pages = (uint32_t)((size - from < CHUNK_BYTES ? size - from : CHUNK_BYTES) / PAGE);
    memset(&s->h, 0, sizeof s->h);
    s->h.index = index;
    size_t host_page = (size_t)getpagesize();
    size_t vec_n = (size_t)(pages * PAGE + host_page - 1) / host_page;
    char vec[CHUNK_BYTES / 4096];
    /* Untouched anonymous pages are neither resident nor paged out. A file's page can be written back and dropped, and
       then looks the same while it holds data: file-backed RAM is read whole. */
    int have_vec = use_mincore && mincore((void *)(base + from), (size_t)pages * PAGE, vec) == 0;
    for (uint32_t i = 0; i < pages; i++) {
        const uint8_t *pg = base + from + (uint64_t)i * PAGE;
        if (have_vec) {
            int touched = 0;
            size_t first = (size_t)(i * PAGE) / host_page, last = (size_t)((i + 1) * PAGE - 1) / host_page;
            for (size_t k = first; k <= last && k < vec_n; k++)
                if (vec[k] & (MINCORE_INCORE | MINCORE_PAGED_OUT)) touched = 1;
            if (!touched) continue;
        }
        if (page_zero(pg)) continue;
        memcpy(s->stage + (uint64_t)s->h.stored * PAGE, pg, PAGE);
        s->h.bitmap[i / 8] |= (uint8_t)(1u << (i % 8));
        s->h.stored++;
    }
    size_t raw = (size_t)s->h.stored * PAGE;
    if (!raw) return;
    size_t got = compression_encode_buffer(s->out, CHUNK_BYTES, s->stage, raw, s->scratch, COMPRESSION_LZ4);
    if (got && got < raw) s->h.bytes = (uint32_t)got;
    else s->h.raw = 1, s->h.bytes = (uint32_t)raw;
}

static double pack_secs, write_secs, wait_secs; /* for the log: compressing, writing, and waiting for a batch to be compressed */

static double mono(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* Chunks are compressed a batch at a time on every core, the next batch while the last one is written. */
static int save_ram(W *w, HvRam *ram, uint64_t *stored_pages, int file_backed) {
    uint32_t nchunks = (uint32_t)((ram->size + CHUNK_BYTES - 1) / CHUNK_BYTES);
    RamHdr rh = {ram->pa, ram->size, CHUNK_PAGES, nchunks};
    w_raw(w, &(SecHdr){TAG('R', 'A', 'M', ' '), 0, sizeof rh}, sizeof(SecHdr));
    w_raw(w, &rh, sizeof rh);
    Slot set[2][BATCH];
    memset(set, 0, sizeof set);
    if (slots_alloc(set[0], BATCH) || slots_alloc(set[1], BATCH)) {
        slots_free(set[0], BATCH), slots_free(set[1], BATCH);
        return w->err = ENOMEM, -1;
    }
    dispatch_queue_t q = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
    dispatch_group_t packing = dispatch_group_create();
    const uint8_t *base = ram->host;
    uint64_t size = ram->size;
    __block double packed = 0;
    void (^pack)(Slot *, uint32_t, uint32_t) = ^(Slot *sl, uint32_t first, uint32_t n) {
        dispatch_group_async(packing, q, ^{
            double t0 = mono();
            dispatch_apply(n, q, ^(size_t k) { chunk_pack(&sl[k], base, size, first + (uint32_t)k, !file_backed); });
            packed += mono() - t0;
        });
    };
    uint32_t n0 = nchunks < BATCH ? nchunks : BATCH;
    if (n0) pack(set[0], 0, n0);
    int cur = 0;
    for (uint32_t first = 0; first < nchunks && !w->err; first += BATCH, cur ^= 1) {
        uint32_t n = nchunks - first < BATCH ? nchunks - first : BATCH;
        double t0 = mono();
        dispatch_group_wait(packing, DISPATCH_TIME_FOREVER);
        double t1 = mono();
        uint32_t next = first + BATCH;
        if (next < nchunks) pack(set[cur ^ 1], next, nchunks - next < BATCH ? nchunks - next : BATCH);
        Slot *sl = set[cur];
        /* The batch's chunks at once, each at its own place in the file: past the page cache a write waits for
           the flash, and one at a time leaves it mostly idle. */
        w_flush(w);
        uint64_t at[BATCH], pos = w->total;
        for (uint32_t k = 0; k < n; k++) {
            at[k] = pos;
            pos += sizeof sl[k].h + sl[k].h.bytes;
            *stored_pages += sl[k].h.stored;
        }
        __block _Atomic int failed = 0;
        int fd = w->fd;
        const uint64_t *atp = at;
        dispatch_apply(n, q, ^(size_t k) {
            struct iovec v[2] = {{&sl[k].h, sizeof sl[k].h}, {sl[k].h.raw ? sl[k].stage : sl[k].out, sl[k].h.bytes}};
            size_t want = v[0].iov_len + v[1].iov_len, done = 0;
            while (done < want) {
                ssize_t got = pwritev(fd, v, v[1].iov_len ? 2 : 1, (off_t)(atp[k] + done));
                if (got < 0 && errno == EINTR) continue;
                if (got <= 0) {
                    failed = errno ? errno : EIO;
                    return;
                }
                done += (size_t)got;
                for (int i = 0; i < 2; i++) { /* what is left of the two pieces */
                    size_t take = (size_t)got < v[i].iov_len ? (size_t)got : v[i].iov_len;
                    v[i].iov_base = (uint8_t *)v[i].iov_base + take, v[i].iov_len -= take, got -= (ssize_t)take;
                }
            }
        });
        if (failed) w->err = failed;
        w->total = pos;
        if (lseek(fd, (off_t)pos, SEEK_SET) != (off_t)pos && !w->err) w->err = errno;
        write_secs += mono() - t1;
        wait_secs += t1 - t0;
    }
    dispatch_group_wait(packing, DISPATCH_TIME_FOREVER);
    pack_secs += packed;
    slots_free(set[0], BATCH), slots_free(set[1], BATCH);
    return w->err ? -1 : 0;
}

static int chunk_unpack(Slot *s, uint8_t *base, uint64_t size) {
    uint64_t from = (uint64_t)s->h.index * CHUNK_BYTES;
    if (from >= size) return -1;
    uint32_t pages = (uint32_t)((size - from < CHUNK_BYTES ? size - from : CHUNK_BYTES) / PAGE);
    size_t raw = (size_t)s->h.stored * PAGE;
    const uint8_t *src = s->out;
    if (!s->h.raw) {
        if (compression_decode_buffer(s->stage, CHUNK_BYTES, s->out, s->h.bytes, s->scratch, COMPRESSION_LZ4) != raw) return -1;
        src = s->stage;
    } else if (s->h.bytes != raw) return -1;
    uint32_t k = 0;
    for (uint32_t i = 0; i < pages && k < s->h.stored; i++)
        if (s->h.bitmap[i / 8] & (1u << (i % 8))) memcpy(base + from + (uint64_t)i * PAGE, src + (uint64_t)k++ * PAGE, PAGE);
    return k == s->h.stored ? 0 : -1;
}

static double read_secs, unpack_secs; /* for the log: reading, and decompressing alongside */

/* A batch is read while the one before it is decompressed into RAM on the other cores. */
static int load_ram(R *r, HvRam *ram) {
    RamHdr rh;
    if (r_section(r, TAG('R', 'A', 'M', ' '), &rh, sizeof rh)) return -1;
    uint32_t nchunks = (uint32_t)((ram->size + CHUNK_BYTES - 1) / CHUNK_BYTES);
    if (rh.pa != ram->pa || rh.size != ram->size || rh.chunk_pages != CHUNK_PAGES || rh.nchunks != nchunks) return r->err = EINVAL, -1;
    Slot set[2][BATCH];
    memset(set, 0, sizeof set);
    if (slots_alloc(set[0], BATCH) || slots_alloc(set[1], BATCH)) {
        slots_free(set[0], BATCH), slots_free(set[1], BATCH);
        return r->err = ENOMEM, -1;
    }
    dispatch_queue_t q = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
    dispatch_group_t unpacking = dispatch_group_create();
    __block _Atomic int bad = 0;
    __block double unpacked = 0;
    uint8_t *base = ram->host;
    uint64_t size = ram->size;
    int cur = 0;
    for (uint32_t first = 0; first < nchunks && !bad; first += BATCH, cur ^= 1) {
        uint32_t n = nchunks - first < BATCH ? nchunks - first : BATCH;
        Slot *sl = set[cur];
        double t0 = mono();
        for (uint32_t k = 0; k < n; k++) {
            if (r_raw(r, &sl[k].h, sizeof sl[k].h) || sl[k].h.index != first + k || sl[k].h.bytes > CHUNK_BYTES ||
                sl[k].h.stored > CHUNK_PAGES || r_raw(r, sl[k].out, sl[k].h.bytes)) {
                bad = 1;
                break;
            }
        }
        read_secs += mono() - t0;
        dispatch_group_wait(unpacking, DISPATCH_TIME_FOREVER); /* the other set is free again */
        if (bad) break;
        dispatch_group_async(unpacking, q, ^{
            double t1 = mono();
            dispatch_apply(n, q, ^(size_t k) {
                if (sl[k].h.stored && chunk_unpack(&sl[k], base, size)) bad = 1;
            });
            unpacked += mono() - t1;
        });
    }
    dispatch_group_wait(unpacking, DISPATCH_TIME_FOREVER);
    unpack_secs += unpacked;
    slots_free(set[0], BATCH), slots_free(set[1], BATCH);
    if (bad && !r->err) r->err = EINVAL;
    return bad ? -1 : 0;
}

/* ---- devices ---- */

static void save_pci(W *w, uint32_t tag, const HvPciDev *d) {
    w_section(w, tag, d->config, sizeof d->config);
    w_section(w, tag, &d->intx, sizeof d->intx);
}

static int load_pci(R *r, uint32_t tag, HvPciDev *d) {
    return r_section(r, tag, d->config, sizeof d->config) || r_section(r, tag, &d->intx, sizeof d->intx) ? -1 : 0;
}

static void save_virtio(W *w, uint32_t tag, const HvVirtio *v) {
    save_pci(w, tag, &v->pci);
    w_section(w, tag, RANGE(v, features, config_len));
}

static int load_virtio(R *r, uint32_t tag, HvVirtio *v) {
    return load_pci(r, tag, &v->pci) || R_RANGE(r, tag, v, features, config_len) ? -1 : 0;
}

#define VBLK TAG('V', 'B', 'L', 'K')
#define VNET TAG('V', 'N', 'E', 'T')
#define VSND TAG('V', 'S', 'N', 'D')
#define VBAL TAG('V', 'B', 'A', 'L')
#define VINP TAG('V', 'I', 'N', 'P')
#define XHCI TAG('X', 'H', 'C', 'I')
#define POWR TAG('P', 'O', 'W', 'R')

static void save_devices(W *w, HvVmapple *b) {
    w_section(w, TAG('B', 'O', 'R', 'D'), b->avp_rtc, sizeof b->avp_rtc);
    w_section(w, TAG('B', 'O', 'R', 'D'), &b->panic_events, sizeof b->panic_events);
    w_section(w, TAG('G', 'I', 'C', ' '), (const uint8_t *)b->gic + offsetof(HvGic, ctlr), sizeof(HvGic) - offsetof(HvGic, ctlr));
    w_section(w, TAG('U', 'A', 'R', 'T'), RANGE(b->uart, cr, rx_count));
    w_section(w, TAG('R', 'T', 'C', ' '), RANGE(b->rtc, offset, ris));
    w_section(w, TAG('G', 'P', 'I', 'O'), RANGE(b->gpio, data, old_in));
    w_section(w, TAG('A', 'E', 'S', ' '), RANGE(b->aes, status, cbc));
    w_section(w, TAG('B', 'D', 'I', 'F'), RANGE(b->bdif, usb_qsel, usb_q));
    w_section(w, TAG('C', 'F', 'G', ' '), b->cfg, 0x10000);
    w_section(w, TAG('A', 'V', 'P', 'R'), hv_vmapple_avp_regs(b->ctrr), 0x10000);
    w_section(w, TAG('A', 'V', 'P', 'R'), hv_vmapple_avp_regs(b->sealed), 0x10000);
    w_section(w, TAG('P', 'C', 'I', 'E'), b->pcie->line_count, sizeof b->pcie->line_count);
    save_pci(w, TAG('P', 'C', 'I', 'E'), &b->pcie->bridge);
    if (b->root_blk) save_virtio(w, VBLK, &b->root_blk->vio);
    if (b->aux_blk) save_virtio(w, VBLK, &b->aux_blk->vio);
    if (b->net) save_virtio(w, VNET, &b->net->vio);
    if (b->snd) {
        HvVirtioSnd *d = b->snd;
        save_virtio(w, VSND, &d->vio);
        w_section(w, VSND, RANGE(d, channels, running));
        w_section(w, VSND, d->ring, HV_VSND_RING);
        uint64_t wc[4] = {atomic_load(&d->written), atomic_load(&d->consumed), d->underruns, d->xfers};
        w_section(w, VSND, wc, sizeof wc);
        w_section(w, VSND, RANGE(d, pending, ptail));
        /* the input stream (HV_MIC): its parameters and the capture buffers it holds; what was recorded is not kept */
        w_section(w, VSND, RANGE(d, in_channels, in_running));
        w_section(w, VSND, RANGE(d, in_pending, in_tail));
    }
    if (b->balloon) {
        save_virtio(w, VBAL, &b->balloon->vio);
        w_section(w, VBAL, RANGE(b->balloon, target, told_pattern));
    }
    HvVirtioInput *in[2] = {b->keyboard, b->mouse};
    for (int i = 0; i < 2; i++)
        if (in[i]) {
            pthread_mutex_lock(&in[i]->mu);
            save_virtio(w, VINP, &in[i]->vio);
            w_section(w, VINP, RANGE(in[i], ring, dropped));
            pthread_mutex_unlock(&in[i]->mu);
        }
    if (b->usb) {
        HvXhci *x = b->usb;
        save_pci(w, XHCI, &x->pci);
        pthread_mutex_lock(&x->mu);
        w_section(w, XHCI, (const uint8_t *)x + offsetof(HvXhci, usbcmd), offsetof(HvXhci, mu) - offsetof(HvXhci, usbcmd));
        w_section(w, XHCI, (const uint8_t *)x + offsetof(HvXhci, reports), sizeof(HvXhci) - offsetof(HvXhci, reports));
        pthread_mutex_unlock(&x->mu);
    }
    /* The battery: only its queue. What it last told the guest is not kept: a restored one says it again. */
    if (b->power) save_virtio(w, POWR, &b->power->vio);
    size_t pn = hv_pvg_lite_state(b->pvg, NULL, 0);
    uint8_t *pb = malloc(pn);
    if (pb && b->pvg) hv_pvg_lite_state(b->pvg, pb, pn);
    w_section(w, TAG('P', 'V', 'G', 'L'), pb, pb && b->pvg ? pn : 0);
    free(pb);
    size_t dn = b->pvg ? hv_pvg_lite_display_state(b->pvg, NULL, 0) : 0;
    uint8_t *db = dn ? malloc(dn) : NULL;
    if (db) hv_pvg_lite_display_state(b->pvg, db, dn);
    w_section(w, TAG('P', 'V', 'G', 'D'), db, db ? dn : 0);
    free(db);
}

static int load_devices(R *r, HvVmapple *b, void **display, uint64_t *display_len) {
    int bad = 0;
    bad |= r_section(r, TAG('B', 'O', 'R', 'D'), b->avp_rtc, sizeof b->avp_rtc);
    bad |= r_section(r, TAG('B', 'O', 'R', 'D'), &b->panic_events, sizeof b->panic_events);
    bad |= r_section(r, TAG('G', 'I', 'C', ' '), (uint8_t *)b->gic + offsetof(HvGic, ctlr), sizeof(HvGic) - offsetof(HvGic, ctlr));
    bad |= R_RANGE(r, TAG('U', 'A', 'R', 'T'), b->uart, cr, rx_count);
    bad |= R_RANGE(r, TAG('R', 'T', 'C', ' '), b->rtc, offset, ris);
    bad |= R_RANGE(r, TAG('G', 'P', 'I', 'O'), b->gpio, data, old_in);
    bad |= R_RANGE(r, TAG('A', 'E', 'S', ' '), b->aes, status, cbc);
    bad |= R_RANGE(r, TAG('B', 'D', 'I', 'F'), b->bdif, usb_qsel, usb_q);
    bad |= r_section(r, TAG('C', 'F', 'G', ' '), b->cfg, 0x10000);
    bad |= r_section(r, TAG('A', 'V', 'P', 'R'), hv_vmapple_avp_regs(b->ctrr), 0x10000);
    bad |= r_section(r, TAG('A', 'V', 'P', 'R'), hv_vmapple_avp_regs(b->sealed), 0x10000);
    bad |= r_section(r, TAG('P', 'C', 'I', 'E'), b->pcie->line_count, sizeof b->pcie->line_count);
    bad |= load_pci(r, TAG('P', 'C', 'I', 'E'), &b->pcie->bridge);
    if (bad) return -1;
    if (b->root_blk && load_virtio(r, VBLK, &b->root_blk->vio)) return -1;
    if (b->aux_blk && load_virtio(r, VBLK, &b->aux_blk->vio)) return -1;
    if (b->net && load_virtio(r, VNET, &b->net->vio)) return -1;
    if (b->snd) {
        HvVirtioSnd *d = b->snd;
        uint64_t wc[4];
        if (load_virtio(r, VSND, &d->vio) || R_RANGE(r, VSND, d, channels, running) || r_section(r, VSND, d->ring, HV_VSND_RING) ||
            r_section(r, VSND, wc, sizeof wc) || R_RANGE(r, VSND, d, pending, ptail) ||
            R_RANGE(r, VSND, d, in_channels, in_running) || R_RANGE(r, VSND, d, in_pending, in_tail))
            return -1;
        atomic_store(&d->written, wc[0]), atomic_store(&d->consumed, wc[1]);
        d->underruns = wc[2], d->xfers = wc[3];
    }
    if (b->balloon && (load_virtio(r, VBAL, &b->balloon->vio) || R_RANGE(r, VBAL, b->balloon, target, told_pattern))) return -1;
    HvVirtioInput *in[2] = {b->keyboard, b->mouse};
    for (int i = 0; i < 2; i++)
        if (in[i] && (load_virtio(r, VINP, &in[i]->vio) || R_RANGE(r, VINP, in[i], ring, dropped))) return -1;
    if (b->usb) {
        HvXhci *x = b->usb;
        if (load_pci(r, XHCI, &x->pci) ||
            r_section(r, XHCI, (uint8_t *)x + offsetof(HvXhci, usbcmd), offsetof(HvXhci, mu) - offsetof(HvXhci, usbcmd)) ||
            r_section(r, XHCI, (uint8_t *)x + offsetof(HvXhci, reports), sizeof(HvXhci) - offsetof(HvXhci, reports)))
            return -1;
    }
    if (b->power) {
        if (load_virtio(r, POWR, &b->power->vio)) return -1;
        b->power->resend = 1; /* the battery as it is now, not as it was when saved */
    }
    void *blob;
    uint64_t len;
    if (r_section_any(r, TAG('P', 'V', 'G', 'L'), &blob, &len, 1u << 20)) return free(blob), -1;
    int pvg_bad = b->pvg && len && hv_pvg_lite_set_state(b->pvg, blob, (size_t)len);
    free(blob);
    if (pvg_bad) return r->err = EINVAL, -1;
    /* The screen once RAM is back: its surfaces are described in guest memory. */
    if (r_section_any(r, TAG('P', 'V', 'G', 'D'), display, display_len, 8u << 20)) return -1;
    return 0;
}

/* ---- the file ---- */

int hv_snapshot_save(const char *path, const char *ident, char *err, size_t errlen) {
    HvMachine *m = hv_vm_machine();
    HvVmapple *b = hv_vm_board();
    int ncpus = hv_vm_ncpus();
    if (!m || !b || !b->ram || ncpus < 1) return fail(err, errlen, "No machine to save.");
    if (!hv_vm_pause_clean()) return fail(err, errlen, "The machine is not paused, or has disk work in flight.");
    if (b->vusb) return fail(err, errlen, "An installer cannot be saved.");
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.part", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return fail(err, errlen, "Cannot write %s: %s", tmp, strerror(errno));
    fcntl(fd, F_NOCACHE, 1);
    W w = {.fd = fd, .cap = 4u << 20};
    w.buf = malloc(w.cap);
    if (!w.buf) {
        close(fd);
        unlink(tmp);
        return fail(err, errlen, "Out of memory.");
    }
    SnapHeader h = {.version = SNAP_VERSION, .header_bytes = sizeof h, .fingerprint = fingerprint(),
                    .guest_counter = hv_vm_paused_counter(), .saved_at = (int64_t)time(NULL), .paused_wall_ns = hv_vm_paused_wall_ns(),
                    .ncpus = (uint32_t)ncpus,
                    .nram = (uint32_t)m->nram, .ram_size = b->ram_size};
    memcpy(h.magic, SNAP_MAGIC, 8);
    snprintf(h.ident, sizeof h.ident, "%s", ident ? ident : "");
    if (hv_vm_disk_identity(h.disk, sizeof h.disk) != 0) {
        free(w.buf);
        close(fd);
        unlink(tmp);
        return fail(err, errlen, "Cannot read the disk's details.");
    }
    w_raw(&w, &h, sizeof h);
    for (int i = 0; i < ncpus; i++) {
        CpuState s;
        cpu_get(hv_vm_cpu(i), &s);
        w_section(&w, TAG('C', 'P', 'U', ' '), &s, sizeof s);
        if (s.cntp_ctl & 1) fprintf(stderr, "hengevm: snapshot: cpu %d has its physical timer on (host counter based)\n", i);
    }
    save_devices(&w, b);
    uint64_t stored = 0;
    pack_secs = write_secs = wait_secs = 0;
    for (int i = 0; i < m->nram && !w.err; i++) save_ram(&w, &m->ram[i], &stored, m->ram[i].host == b->ram && b->ram_fd >= 0);
    Trailer t = {TAG('E', 'N', 'D', ' '), 0, w.total + sizeof(Trailer)};
    w_raw(&w, &t, sizeof t);
    w_flush(&w);
    free(w.buf);
    int bad = w.err;
    if (!bad && fsync(fd) != 0) bad = errno;
    close(fd);
    if (!bad && rename(tmp, path) != 0) bad = errno;
    if (bad) {
        unlink(tmp);
        return fail(err, errlen, "Cannot write the saved Mac: %s", strerror(bad));
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9;
    fprintf(stderr, "hengevm: snapshot: saved %d CPUs and %llu MB of RAM (%llu pages with data) in %llu MB, %.2f s "
                    "(compressing %.2f s alongside writing %.2f s; %.2f s waiting for compression)\n", ncpus, (unsigned long long)(b->ram_size >> 20),
            (unsigned long long)stored, (unsigned long long)(t.total >> 20), secs, pack_secs, write_secs, wait_secs);
    return 0;
}

static int read_header(int fd, SnapHeader *h, const char *ident, uint64_t *bytes) {
    struct stat st;
    Trailer t;
    if (fstat(fd, &st) != 0 || (uint64_t)st.st_size < sizeof *h + sizeof t) return -1;
    if (pread(fd, h, sizeof *h, 0) != (ssize_t)sizeof *h || memcmp(h->magic, SNAP_MAGIC, 8) || h->version != SNAP_VERSION ||
        h->header_bytes != sizeof *h || h->fingerprint != fingerprint())
        return -1;
    h->ident[sizeof h->ident - 1] = 0;
    if (strcmp(h->ident, ident ? ident : "")) return -2;
    if (pread(fd, &t, sizeof t, st.st_size - (off_t)sizeof t) != (ssize_t)sizeof t || t.tag != TAG('E', 'N', 'D', ' ') ||
        t.total != (uint64_t)st.st_size)
        return -1;
    if (bytes) *bytes = (uint64_t)st.st_size;
    return 0;
}

int hv_snapshot_check(const char *path, const char *ident, uint64_t *bytes, int64_t *saved_at) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    SnapHeader h;
    int r = read_header(fd, &h, ident, bytes);
    close(fd);
    if (!r && saved_at) *saved_at = h.saved_at;
    return r;
}

int hv_snapshot_restore(const char *path, const char *ident, char *err, size_t errlen) {
    HvMachine *m = hv_vm_machine();
    HvVmapple *b = hv_vm_board();
    int ncpus = hv_vm_ncpus();
    if (!m || !b || !b->ram) return fail(err, errlen, "No machine to restore into.");
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return fail(err, errlen, "No saved Mac: %s", strerror(errno));
    fcntl(fd, F_NOCACHE, 1);
    SnapHeader h;
    int hr = read_header(fd, &h, ident, NULL);
    if (hr) {
        close(fd);
        return fail(err, errlen, hr == -2 ? "The saved Mac is from another build of Henge or another disk." : "The saved Mac is incomplete or damaged.");
    }
    char disk_now[256];
    h.disk[sizeof h.disk - 1] = 0;
    if (hv_vm_disk_identity(disk_now, sizeof disk_now) != 0 || strcmp(disk_now, h.disk)) {
        close(fd);
        fprintf(stderr, "hengevm: snapshot: the disk changed since the save (%s, now %s)\n", h.disk, disk_now);
        return fail(err, errlen, "The Mac's disk has changed since it was saved.");
    }
    if (h.ncpus != (uint32_t)ncpus || h.nram != (uint32_t)m->nram || h.ram_size != b->ram_size) {
        close(fd);
        return fail(err, errlen, "The saved Mac had %u CPUs and %llu MB of RAM; this one is set up differently.", h.ncpus,
                    (unsigned long long)(h.ram_size >> 20));
    }
    R r = {.fd = fd, .cap = 4u << 20};
    r.buf = malloc(r.cap);
    if (!r.buf || lseek(fd, sizeof h, SEEK_SET) != (off_t)sizeof h) {
        free(r.buf);
        close(fd);
        return fail(err, errlen, "Out of memory.");
    }
    int bad = 0;
    CpuState cs[HV_MAX_CPUS];
    for (int i = 0; i < ncpus && !bad; i++) bad = r_section(&r, TAG('C', 'P', 'U', ' '), &cs[i], sizeof cs[i]);
    void *display = NULL;
    uint64_t display_len = 0;
    if (!bad) bad = load_devices(&r, b, &display, &display_len);
    read_secs = unpack_secs = 0;
    for (int i = 0; i < m->nram && !bad; i++) bad = load_ram(&r, &m->ram[i]);
    Trailer t;
    if (!bad) bad = r_raw(&r, &t, sizeof t) || t.tag != TAG('E', 'N', 'D', ' ');
    free(r.buf);
    close(fd);
    if (!bad && b->pvg && display_len) hv_pvg_lite_set_display_state(b->pvg, display, (size_t)display_len);
    free(display);
    if (bad) return fail(err, errlen, "The saved Mac could not be read (%s).", strerror(r.err ? r.err : EINVAL));
    /* The guest's clock goes on as far as the wall clock went since it stopped (see hv_vm_resume_counter). */
    m->cntvoff = hv_counter() - hv_vm_resume_counter(h.guest_counter, h.paused_wall_ns);
    for (int i = 0; i < ncpus; i++) {
        HvCpu *c = hv_vm_cpu(i);
        cpu_put(c, &cs[i]);
        c->cntvoff = m->cntvoff;
    }
    hv_gicv3_update(b->gic);
    if (b->snd) hv_virtio_snd_resume(b->snd);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    fprintf(stderr, "hengevm: snapshot: restored a Mac saved %lld s ago, in %.2f s (reading RAM %.2f s, decompressing %.2f s alongside)\n",
            (long long)(time(NULL) - h.saved_at), (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9,
            read_secs, unpack_secs);
    return 0;
}
