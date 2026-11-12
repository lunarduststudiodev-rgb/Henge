#include "snapshot.h"
#include "rosetta.h"

#include <arm_acle.h>
#include <compression.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "codestore.h"
#include "mmu.h"
#include "ramfile.h"

#define HEADER_BYTES 65536u
#define CHUNK (1u << 20)
#define MAX_SECTIONS 64
#define MAX_SECTION_BYTES (320u << 20) /* one built in memory (hv_snap_put); a streamed one has no limit but the disk */
#define STREAM_BUF (4u << 20)
#define STREAM_ALIGN 65536u /* where a streamed section starts, and the RAM after the last */
#define MAP_OVER (16u << 20) /* a section bigger than this is mapped from the file when loaded, not read into memory */
#define MAX_THREADS 16

/* A RAM chunk's entry: kind in bits 31..28, bytes stored in its slot below. */
#define KIND_ZERO 0u
#define KIND_RAW 1u
#define KIND_LZ4 2u
#define INFO(kind, len) ((kind) << 28 | (uint32_t)(len))
#define INFO_KIND(i) ((i) >> 28)
#define INFO_LEN(i) ((i) & 0x0fffffffu)

/* The file's own layout, fixed: every field has its width and place (asserted below). */
typedef struct SnapSec {
    uint32_t tag, version;
    uint64_t off, len;
    uint32_t crc, pad;
} SnapSec;

typedef struct SnapHeader {
    char magic[8];          /* "HENGESNP" */
    uint32_t format;        /* HV_SNAP_FORMAT */
    uint32_t header_bytes;  /* sizeof(SnapHeader) */
    uint64_t file_bytes;    /* the file is at least this long */
    uint64_t saved_unix_us; /* when it was committed */
    uint32_t nsections, pad;
    SnapSec sec[MAX_SECTIONS];
    uint32_t crc;           /* CRC32C of everything above */
    uint32_t pad2;
} SnapHeader;

_Static_assert(sizeof(SnapSec) == 32, "snapshot file layout");
_Static_assert(offsetof(SnapHeader, sec) == 40, "snapshot file layout");
_Static_assert(offsetof(SnapHeader, crc) == 40 + 32 * MAX_SECTIONS, "snapshot file layout");
_Static_assert(sizeof(SnapHeader) <= HEADER_BYTES, "snapshot file layout");

static const char MAGIC[8] = {'H', 'E', 'N', 'G', 'E', 'S', 'N', 'P'};

/* ---- Small pieces ---- */

__attribute__((target("crc"))) uint32_t hv_crc32c(uint32_t crc, const void *data, size_t len) {
    const uint8_t *p = data;
    crc = ~crc;
    while (len && ((uintptr_t)p & 7)) crc = __crc32cb(crc, *p++), len--;
    for (; len >= 32; p += 32, len -= 32) {
        const uint64_t *w = (const uint64_t *)p;
        crc = __crc32cd(crc, w[0]);
        crc = __crc32cd(crc, w[1]);
        crc = __crc32cd(crc, w[2]);
        crc = __crc32cd(crc, w[3]);
    }
    for (; len >= 8; p += 8, len -= 8) crc = __crc32cd(crc, *(const uint64_t *)p);
    while (len--) crc = __crc32cb(crc, *p++);
    return ~crc;
}

static int all_zero(const uint8_t *p, size_t len) {
    const uint64_t *w = (const uint64_t *)p;
    size_t n = len / 8, i = 0;
    for (; i + 8 <= n; i += 8)
        if (w[i] | w[i + 1] | w[i + 2] | w[i + 3] | w[i + 4] | w[i + 5] | w[i + 6] | w[i + 7]) return 0;
    for (; i < n; i++)
        if (w[i]) return 0;
    for (size_t b = n * 8; b < len; b++)
        if (p[b]) return 0;
    return 1;
}

static int write_all(int fd, const void *buf, size_t len, uint64_t off) {
    const uint8_t *p = buf;
    while (len) {
        ssize_t n = pwrite(fd, p, len, (off_t)off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n, len -= (size_t)n, off += (uint64_t)n;
    }
    return 0;
}

static int read_all(int fd, void *buf, size_t len, uint64_t off) {
    uint8_t *p = buf;
    while (len) {
        ssize_t n = pread(fd, p, len, (off_t)off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n, len -= (size_t)n, off += (uint64_t)n;
    }
    return 0;
}

/* Everything written so far onto storage itself, not only into the drive's cache. */
static int full_sync(int fd) {
    if (fcntl(fd, F_FULLFSYNC) == 0) return 0;
    return fsync(fd);
}

static double ms_since(uint64_t t) { return (double)(mach_absolute_time() - t) / 24000.0; }

static void tag_name(uint32_t tag, char out[5]) {
    for (int i = 0; i < 4; i++) {
        char c = (char)(tag >> (8 * i));
        out[i] = c >= 32 && c < 127 ? c : '?';
    }
    out[4] = 0;
}

static void run_threads(int threads, void *(*fn)(void *), void *arg) {
    if (threads < 1) threads = 1;
    if (threads > MAX_THREADS) threads = MAX_THREADS;
    pthread_t t[MAX_THREADS];
    int started = 0;
    for (int i = 1; i < threads; i++)
        if (pthread_create(&t[started], NULL, fn, arg) == 0) started++;
    fn(arg); /* this thread works too: with no threads to be had it does it all */
    for (int i = 0; i < started; i++) pthread_join(t[i], NULL);
}

void hv_snap_out(HvSnapOut *o, const void *v, size_t n) {
    if (o->bad) return;
    if (o->len + n > o->cap) {
        size_t cap = o->cap ? o->cap : 4096;
        while (cap < o->len + n) cap *= 2;
        uint8_t *p = realloc(o->p, cap);
        if (!p) {
            o->bad = 1;
            return;
        }
        o->p = p, o->cap = cap;
    }
    memcpy(o->p + o->len, v, n);
    o->len += n;
}

void hv_snap_in(HvSnapIn *in, void *v, size_t n) {
    if (in->bad || n > in->len - in->at) {
        if (!in->bad) in->bad = 1;
        memset(v, 0, n);
        return;
    }
    memcpy(v, in->p + in->at, n);
    in->at += n;
}

int hv_snap_in_done(HvSnapIn *in) { return !in->bad && in->at == in->len ? 0 : -1; }

/* ---- Writing ---- */

struct HvSnapWriter {
    int fd;
    unsigned flags;
    int failed, have_ram;
    char *path, *partial;
    int saved; /* a copy of the memory files beside it (hv_snap_put_ram): <path>.ram-<saved_gen> */
    uint8_t saved_gen[HV_RAMFILE_GEN];
    uint64_t ram_end; /* end of the RAM slots */
    uint64_t tail;    /* end of the streamed sections, after the header: the RAM goes after them */
    int streaming;    /* a stream is open */
    const uint64_t *cancel; /* hv_snap_set_cancel: stopped once *cancel != ticket */
    uint64_t ticket;
    uint32_t nsec;
    struct {
        uint32_t tag, version;
        uint8_t *data; /* NULL for a streamed one: already in the file, at off */
        size_t len;
        int streamed;
        uint64_t off;
        uint32_t crc;
    } sec[MAX_SECTIONS];
};

struct HvSnapStream {
    HvSnapWriter *w;
    uint32_t tag, version, crc;
    uint64_t off, written; /* its start in the file, and how much of it is there */
    uint8_t *buf;
    size_t n; /* in buf, not yet written */
};

__attribute__((format(printf, 2, 3))) static void wfail(HvSnapWriter *w, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "hengevm: snapshot: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    w->failed = 1;
}

void hv_snap_set_cancel(HvSnapWriter *w, const uint64_t *count, uint64_t ticket) {
    if (w) w->cancel = count, w->ticket = ticket;
}

int hv_snap_cancelled(HvSnapWriter *w) {
    return w && w->cancel && __atomic_load_n(w->cancel, __ATOMIC_ACQUIRE) != w->ticket;
}

HvSnapWriter *hv_snap_begin(const char *path, unsigned flags) {
    HvSnapWriter *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->flags = flags;
    w->tail = HEADER_BYTES;
    w->path = strdup(path);
    if (!w->path || asprintf(&w->partial, "%s.partial", path) < 0) {
        free(w->path), free(w);
        return NULL;
    }
    unlink(w->partial); /* a save that never finished */
    w->fd = open(w->partial, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (w->fd < 0) {
        int e = errno; /* the caller's to read: kept past the message and the frees */
        fprintf(stderr, "hengevm: snapshot: can't create %s: %s\n", w->partial, strerror(e));
        free(w->partial), free(w->path), free(w);
        errno = e;
        return NULL;
    }
    fcntl(w->fd, F_NOCACHE, 1); /* gigabytes written once: keep them out of the file cache */
    return w;
}

int hv_snap_put(HvSnapWriter *w, uint32_t tag, uint32_t version, HvSnapOut *o) {
    char name[5];
    tag_name(tag, name);
    if (!w->failed && o->bad) wfail(w, "section %s could not be built (out of memory?)", name);
    for (uint32_t i = 0; !w->failed && i < w->nsec; i++)
        if (w->sec[i].tag == tag) wfail(w, "section %s saved twice", name);
    if (!w->failed && w->nsec >= MAX_SECTIONS) wfail(w, "too many sections");
    if (!w->failed && o->len > MAX_SECTION_BYTES) wfail(w, "section %s is too big (%zu bytes)", name, o->len);
    if (w->failed) {
        free(o->p);
        memset(o, 0, sizeof *o);
        return -1;
    }
    w->sec[w->nsec].tag = tag;
    w->sec[w->nsec].version = version;
    w->sec[w->nsec].data = o->p;
    w->sec[w->nsec].len = o->len;
    w->nsec++;
    memset(o, 0, sizeof *o);
    return 0;
}

typedef struct RamJob {
    HvMachine *m;
    int fd;
    int nreg;
    uint64_t file_off[HV_MAX_RAM];
    uint32_t first[HV_MAX_RAM + 1]; /* chunk numbers: region r has first[r] .. first[r + 1] - 1 */
    uint32_t *entries;              /* two words a chunk: INFO, CRC */
    uint32_t next;
    int failed;
    unsigned flags;
    uint64_t stored, zero_chunks;
    uint64_t t_read, t_pack, t_write; /* ticks, all threads: where a slow save's time goes (logged) */
    const uint64_t *cancel;           /* the writer's: stopped once *cancel != ticket */
    uint64_t ticket;
    const char *why; /* the first error (a constant string) */
    uint64_t why_at;
    /* A region kept in its file (RAM version 2): its chunks are the pages the file doesn't hold, one page each,
       pages[r] saying which. NULL: the region in CHUNK-sized slots, as in version 1. */
    uint32_t *pages[HV_MAX_RAM];
    uint64_t unit[HV_MAX_RAM]; /* bytes a chunk of region r stands for: CHUNK, or the page */
    /* An auto-save: a kept region's page c is at copy[r] + slot[r][c] * unit (the save guard's buffer); another
       region's bytes at host[r] (copied at the instant saved). NULL: the region itself, as it is. */
    const uint8_t *copy[HV_MAX_RAM];
    uint32_t *slot[HV_MAX_RAM];
    const uint8_t *host[HV_MAX_RAM];
} RamJob;

/* Where chunk c of region r is in the region, and how long it is. */
static uint64_t chunk_at(const RamJob *j, int r, uint64_t c) {
    return j->pages[r] ? (uint64_t)j->pages[r][c] * j->unit[r] : c * j->unit[r];
}

static void job_fail(RamJob *j, const char *why, uint64_t at) {
    if (!__atomic_exchange_n(&j->failed, 1, __ATOMIC_SEQ_CST)) j->why = why, j->why_at = at;
}

static int find_chunk(RamJob *j, uint32_t idx, int *r) {
    for (int i = 0; i < j->nreg; i++)
        if (idx < j->first[i + 1]) {
            *r = i;
            return 0;
        }
    return -1;
}

static void *save_worker(void *arg) {
    RamJob *j = arg;
    uint8_t *dst = malloc(CHUNK);
    void *scratch = malloc(compression_encode_scratch_buffer_size(COMPRESSION_LZ4_RAW));
    if (!dst || !scratch) {
        job_fail(j, "out of memory", 0);
        free(dst), free(scratch);
        return NULL;
    }
    uint64_t stored = 0, zeros = 0, t_read = 0, t_pack = 0, t_write = 0;
    for (;;) {
        uint32_t idx = __atomic_fetch_add(&j->next, 1, __ATOMIC_RELAXED);
        int r;
        if (idx >= j->first[j->nreg] || __atomic_load_n(&j->failed, __ATOMIC_RELAXED) || find_chunk(j, idx, &r)) break;
        if (j->cancel && __atomic_load_n(j->cancel, __ATOMIC_ACQUIRE) != j->ticket) {
            job_fail(j, "cancelled", 0);
            break;
        }
        HvRam *ram = &j->m->ram[r];
        uint64_t c = idx - j->first[r], at = chunk_at(j, r, c), unit = j->unit[r];
        const uint8_t *src = j->copy[r] ? j->copy[r] + (uint64_t)j->slot[r][c] * unit : j->host[r] ? j->host[r] + at : ram->host + at;
        size_t len = ram->size - at < unit ? (size_t)(ram->size - at) : (size_t)unit;
        uint32_t info, crc = 0;
        /* The first read brings the pages in: from iOS's compressed memory, or its swap, if it put them there */
        uint64_t t = mach_absolute_time(), t1;
        int zero = all_zero(src, len);
        t_read += (t1 = mach_absolute_time()) - t;
        if (zero) {
            info = INFO(KIND_ZERO, 0);
            zeros++;
        } else {
            crc = hv_crc32c(0, src, len);
            /* Compressed only if it saves an eighth: otherwise raw is as good and faster to load. */
            size_t clen = compression_encode_buffer(dst, len - len / 8, src, len, scratch, COMPRESSION_LZ4_RAW);
            const void *out = clen ? (const void *)dst : (const void *)src;
            size_t n = clen ? clen : len;
            info = clen ? INFO(KIND_LZ4, clen) : INFO(KIND_RAW, len);
            t_pack += (t = mach_absolute_time()) - t1;
            int bad = write_all(j->fd, out, n, j->file_off[r] + c * unit) != 0;
            t_write += mach_absolute_time() - t;
            if (bad) {
                job_fail(j, strerror(errno), ram->pa + at);
                break;
            }
            stored += n;
        }
        j->entries[2 * idx] = info;
        j->entries[2 * idx + 1] = crc;
    }
    __atomic_fetch_add(&j->stored, stored, __ATOMIC_RELAXED);
    __atomic_fetch_add(&j->zero_chunks, zeros, __ATOMIC_RELAXED);
    __atomic_fetch_add(&j->t_read, t_read, __ATOMIC_RELAXED);
    __atomic_fetch_add(&j->t_pack, t_pack, __ATOMIC_RELAXED);
    __atomic_fetch_add(&j->t_write, t_write, __ATOMIC_RELAXED);
    free(dst), free(scratch);
    return NULL;
}

/* A region kept in its files: brought up to date and copied beside the snapshot (<path>.ram-<generation>,
   core/ramfile.h), with the pages the copy may not have right listed for the save itself. One region at most has
   a copy. 0; 1: save the region whole instead (logged); -1: stopped (cancelled). */
static int prepare_file(HvSnapWriter *w, HvRam *ram, int threads, HvRamFileSave *s) {
    if (w->saved) return 1;
    int rc = hv_ramfile_commit(ram->file, w->path, threads, w->cancel, w->ticket, s);
    if (rc == -3) return -1;
    if (rc != 0) {
        fprintf(stderr, "hengevm: snapshot: no copy of the memory files this time (above): saving the memory whole\n");
        return 1;
    }
    w->saved = 1;
    memcpy(w->saved_gen, s->gen, sizeof w->saved_gen);
    return 0;
}

/* An auto-save's region kept in its files: its copy finished (on storage, checked against the memory at the
   instant saved). 0; 1: no copy to use (logged); -1: stopped (cancelled). The save is freed unless 0. */
static int finish_file(HvSnapWriter *w, HvRam *ram, int threads, HvRamFileSave *s) {
    if (w->saved) {
        hv_ramfile_remove_copy(w->path, s->gen);
        hv_ramfile_save_free(s);
        return 1;
    }
    int rc = hv_ramfile_finish(ram->file, w->path, threads, w->cancel, w->ticket, s);
    if (rc == -3) return -1;
    if (rc != 0) return 1;
    w->saved = 1;
    memcpy(w->saved_gen, s->gen, sizeof w->saved_gen);
    return 0;
}

static int put_ram(HvSnapWriter *w, HvMachine *m, int threads, HvSnapTaken *taken);

int hv_snap_put_ram(HvSnapWriter *w, HvMachine *m, int threads) { return put_ram(w, m, threads, NULL); }

int hv_snap_put_ram_taken(HvSnapWriter *w, HvMachine *m, int threads, HvSnapTaken *t) {
    int bad = 0;
    for (int r = 0; r < m->nram; r++) {
        if (m->ram[r].readonly) continue;
        if (m->ram[r].file ? !t->file[r] || !t->file[r]->copy : !t->host[r]) bad = 1;
    }
    for (int r = m->nram; r < HV_MAX_RAM; r++) bad |= t->file[r] != NULL;
    if (bad) {
        for (int r = 0; r < HV_MAX_RAM; r++)
            if (t->file[r]) hv_ramfile_remove_copy(w->path, t->file[r]->gen), hv_ramfile_save_free(t->file[r]), t->file[r] = NULL;
        wfail(w, "an auto-save's memory isn't all there");
        return -1;
    }
    return put_ram(w, m, threads, t);
}

/* A put_ram that stops early: everything it holds is freed (each region's pages, slots and checks), and so is
   what is left of an auto-save's taken regions, with their copies. A copy already finished is the writer's to
   remove (w->saved). */
static void put_ram_drop(HvSnapWriter *w, HvMachine *m, RamJob *j, HvRamFileSave *saves, HvSnapTaken *taken) {
    for (int r = 0; r < m->nram; r++) free(j->pages[r]), free(j->slot[r]), j->pages[r] = NULL, j->slot[r] = NULL;
    for (int r = 0; r < HV_MAX_RAM; r++) hv_ramfile_save_free(&saves[r]);
    for (int r = 0; taken && r < HV_MAX_RAM; r++)
        if (taken->file[r]) {
            hv_ramfile_remove_copy(w->path, taken->file[r]->gen);
            hv_ramfile_save_free(taken->file[r]);
            taken->file[r] = NULL;
        }
    free(j->entries);
    free(j), free(saves);
}

static int put_ram(HvSnapWriter *w, HvMachine *m, int threads, HvSnapTaken *taken) {
    if (w->failed) return -1;
    if (w->have_ram) {
        wfail(w, "RAM saved twice");
        return -1;
    }
    w->have_ram = 1;
    uint64_t t0 = mach_absolute_time();
    static RamJob zero_job;
    RamJob *j = malloc(sizeof *j);
    if (!j) {
        wfail(w, "out of memory");
        return -1;
    }
    *j = zero_job;
    j->m = m, j->fd = w->fd, j->nreg = m->nram;
    j->cancel = w->cancel, j->ticket = w->ticket;
    if (w->streaming) {
        free(j);
        wfail(w, "RAM saved while a section is still streaming");
        return -1;
    }
    /* Regions kept in a file first: what the rest of the save needs to hold of them */
    static HvRamFileSave zero_save;
    HvRamFileSave *saves = malloc(HV_MAX_RAM * sizeof *saves);
    if (!saves) {
        free(j);
        wfail(w, "out of memory");
        return -1;
    }
    for (int r = 0; r < HV_MAX_RAM; r++) saves[r] = zero_save;
    int version = 1;
    uint64_t t_file = 0, overlay_bytes = 0, overlay_pages = 0, count[HV_MAX_RAM] = {0};
    for (int r = 0; r < m->nram; r++) {
        j->unit[r] = CHUNK;
        if (taken && !m->ram[r].file) j->host[r] = taken->host[r];
        if (!m->ram[r].file || m->ram[r].readonly) continue;
        uint64_t t = mach_absolute_time();
        int p;
        if (taken) {
            p = finish_file(w, &m->ram[r], threads, taken->file[r]);
            if (p == 0) {
                saves[r] = *taken->file[r]; /* its pages, slots and checks, owned here from now */
                memset(taken->file[r], 0, sizeof *taken->file[r]);
                j->copy[r] = saves[r].copy;
                j->slot[r] = saves[r].slot;
                saves[r].slot = NULL;
            }
            taken->file[r] = NULL;
            if (p > 0) p = 2; /* never whole: the Mac ran on since */
        } else {
            p = prepare_file(w, &m->ram[r], threads, &saves[r]);
        }
        t_file += mach_absolute_time() - t;
        if (p < 0 || p == 2) {
            put_ram_drop(w, m, j, saves, taken);
            if (p < 0) wfail(w, "cancelled");
            else wfail(w, "the memory's saved copy isn't what the memory was (the log says why)");
            return -1;
        }
        if (p > 0 && (w->flags & HV_SNAP_NO_WHOLE)) {
            put_ram_drop(w, m, j, saves, taken);
            wfail(w, "the memory files couldn't be copied, and this save isn't to write the memory whole");
            return -1;
        }
        if (p > 0) {
            /* Whole after all: the app made room for what the files lacked, not for this. Not to the last byte:
               a storage filled mid-save fails anyway, and troubles iOS and every other app meanwhile. */
            uint64_t free_now = hv_snap_free_bytes(w), need = m->ram[r].size + (512ull << 20);
            if (free_now != UINT64_MAX && free_now < need) {
                put_ram_drop(w, m, j, saves, taken);
                wfail(w, "no room to save the memory whole (%.1f GB needed, %.1f GB free)", (double)need / (1ull << 30),
                      (double)free_now / (1ull << 30));
                return -1;
            }
            continue;
        }
        uint64_t n = saves[r].npages;
        j->pages[r] = saves[r].pages;
        saves[r].pages = NULL;
        j->unit[r] = hv_ramfile_page(m->ram[r].file);
        count[r] = n;
        overlay_bytes += n * j->unit[r];
        overlay_pages += n;
        version = 2;
    }
    uint64_t off = (w->tail + STREAM_ALIGN - 1) & ~(uint64_t)(STREAM_ALIGN - 1), total = 0;
    for (int r = 0; r < m->nram; r++) {
        uint64_t n = j->pages[r] ? count[r] : (m->ram[r].size + CHUNK - 1) / CHUNK;
        j->first[r] = (uint32_t)total;
        j->file_off[r] = off;
        total += n;
        off += n * j->unit[r];
        off = (off + STREAM_ALIGN - 1) & ~(uint64_t)(STREAM_ALIGN - 1); /* version 1: already (whole chunks) */
    }
    j->first[m->nram] = (uint32_t)total;
    if (total > 1u << 24) { /* 16 TB of chunks, or 16M pages the file doesn't hold */
        put_ram_drop(w, m, j, saves, taken);
        wfail(w, "RAM too large");
        return -1;
    }
    j->entries = calloc(total ? total : 1, 2 * sizeof *j->entries);
    if (!j->entries) {
        put_ram_drop(w, m, j, saves, taken);
        wfail(w, "out of memory");
        return -1;
    }
    /* What iOS had done with this app's memory, before and after: compressed, and pages brought back in */
    task_vm_info_data_t vm0 = {0}, vm1 = {0};
    task_events_info_data_t ev0 = {0}, ev1 = {0};
    mach_msg_type_number_t n0 = TASK_VM_INFO_COUNT, e0 = TASK_EVENTS_INFO_COUNT;
    int have_vm = task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm0, &n0) == KERN_SUCCESS;
    int have_ev = task_info(mach_task_self(), TASK_EVENTS_INFO, (task_info_t)&ev0, &e0) == KERN_SUCCESS;
    run_threads(threads, save_worker, j);
    mach_msg_type_number_t n1 = TASK_VM_INFO_COUNT, e1 = TASK_EVENTS_INFO_COUNT;
    have_vm = have_vm && task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm1, &n1) == KERN_SUCCESS;
    have_ev = have_ev && task_info(mach_task_self(), TASK_EVENTS_INFO, (task_info_t)&ev1, &e1) == KERN_SUCCESS;
    if (j->failed) {
        wfail(w, "RAM write failed at %#llx: %s", (unsigned long long)j->why_at, j->why);
    } else {
        /* Version 1: as every Henge before the memory files wrote it, byte for byte. Version 2 adds, per region,
           flags (bit 0: kept in its files, with a copy beside the snapshot), the chunk unit, the copy's generation
           and the checks a resume makes (HV_RAMFILE_CHECKS, unused ones zero); then after the entries, each kept
           region's page numbers. */
        HvSnapOut o = {0};
        uint32_t nreg = (uint32_t)m->nram, chunk = CHUNK;
        HV_SNAP_OUT(&o, nreg);
        HV_SNAP_OUT(&o, chunk);
        for (int r = 0; r < m->nram; r++) {
            uint64_t pa = m->ram[r].pa, size = m->ram[r].size, file_off = j->file_off[r];
            uint32_t ro = (uint32_t)m->ram[r].readonly, n = j->first[r + 1] - j->first[r];
            HV_SNAP_OUT(&o, pa);
            HV_SNAP_OUT(&o, size);
            HV_SNAP_OUT(&o, file_off);
            HV_SNAP_OUT(&o, ro);
            HV_SNAP_OUT(&o, n);
            if (version == 2) {
                uint32_t flags = j->pages[r] ? 1u : 0u, unit = (uint32_t)j->unit[r];
                HV_SNAP_OUT(&o, flags);
                HV_SNAP_OUT(&o, unit);
                hv_snap_out(&o, saves[r].gen, sizeof saves[r].gen);
                HV_SNAP_OUT(&o, saves[r].nchecks);
                hv_snap_out(&o, saves[r].check, sizeof saves[r].check);
            }
        }
        hv_snap_out(&o, j->entries, total * 2 * sizeof *j->entries);
        for (int r = 0; r < m->nram; r++)
            if (j->pages[r]) hv_snap_out(&o, j->pages[r], (size_t)(j->first[r + 1] - j->first[r]) * sizeof(uint32_t));
        w->ram_end = off;
        if (hv_snap_put(w, HV_SNAP_RAM, (uint32_t)version, &o) == 0 && m->log) {
            if (version == 2)
                fprintf(stderr, "hengevm: snapshot: RAM kept in its files (copied in %.0f ms); %llu pages the copy may not hold "
                                "(%llu MB) saved here; %llu MB stored in all, in %.0f ms\n",
                        t_file / 24000.0, (unsigned long long)overlay_pages,
                        (unsigned long long)(overlay_bytes >> 20), (unsigned long long)(j->stored >> 20), ms_since(t0));
            else
                fprintf(stderr, "hengevm: snapshot: RAM %llu MB, %llu MB stored (%llu of %llu chunks all zero) in %.0f ms\n",
                        (unsigned long long)(total * CHUNK >> 20), (unsigned long long)(j->stored >> 20),
                        (unsigned long long)j->zero_chunks, (unsigned long long)total, ms_since(t0));
            /* Thread time (summed over the threads, so up to threads x the time above) */
            fprintf(stderr, "hengevm: snapshot: RAM time, %d threads: reading it in %.0f ms, packing %.0f ms, writing %.0f ms\n",
                    threads, j->t_read / 24000.0, j->t_pack / 24000.0, j->t_write / 24000.0);
            if (have_vm)
                fprintf(stderr, "hengevm: snapshot: Henge's memory: %.0f MB compressed by iOS before, %.0f MB after; in use %.0f -> %.0f MB",
                        vm0.compressed / 1048576.0, vm1.compressed / 1048576.0, vm0.phys_footprint / 1048576.0, vm1.phys_footprint / 1048576.0);
            if (have_vm && have_ev)
                fprintf(stderr, "; page faults %d, from storage %d\n", ev1.faults - ev0.faults, ev1.pageins - ev0.pageins);
            else if (have_vm)
                fprintf(stderr, "\n");
        }
    }
    for (int r = 0; r < m->nram; r++) free(j->pages[r]), free(j->slot[r]), hv_ramfile_save_free(&saves[r]);
    free(j->entries);
    free(j), free(saves);
    return w->failed ? -1 : 0;
}

/* ---- Streamed sections ---- */

HvSnapStream *hv_snap_stream_begin(HvSnapWriter *w, uint32_t tag, uint32_t version) {
    char name[5];
    tag_name(tag, name);
    if (w->failed) return NULL;
    if (w->have_ram) wfail(w, "section %s streamed after the RAM", name);
    else if (w->streaming) wfail(w, "section %s streamed while another is", name);
    else if (w->nsec >= MAX_SECTIONS) wfail(w, "too many sections");
    for (uint32_t i = 0; !w->failed && i < w->nsec; i++)
        if (w->sec[i].tag == tag) wfail(w, "section %s saved twice", name);
    if (w->failed) return NULL;
    HvSnapStream *s = calloc(1, sizeof *s);
    if (s) s->buf = malloc(STREAM_BUF);
    if (!s || !s->buf) {
        if (s) free(s);
        wfail(w, "out of memory");
        return NULL;
    }
    s->w = w, s->tag = tag, s->version = version;
    s->off = (w->tail + STREAM_ALIGN - 1) & ~(uint64_t)(STREAM_ALIGN - 1);
    w->streaming = 1;
    return s;
}

static int stream_flush(HvSnapStream *s) {
    if (!s->n || s->w->failed) return s->w->failed ? -1 : 0;
    if (write_all(s->w->fd, s->buf, s->n, s->off + s->written) != 0) {
        wfail(s->w, "write failed: %s", strerror(errno));
        return -1;
    }
    s->written += s->n;
    s->n = 0;
    return 0;
}

uint64_t hv_snap_free_bytes(HvSnapWriter *w) {
    struct statfs f;
    if (!w || w->fd < 0 || fstatfs(w->fd, &f) != 0) return UINT64_MAX;
    return (uint64_t)f.f_bavail * f.f_bsize;
}

int hv_snap_stream_write(HvSnapStream *s, const void *v, size_t n) {
    if (!s || s->w->failed) return -1;
    if (hv_snap_cancelled(s->w)) {
        wfail(s->w, "cancelled");
        return -1;
    }
    const uint8_t *p = v;
    s->crc = hv_crc32c(s->crc, p, n);
    while (n) {
        if (!s->n && n >= STREAM_BUF) { /* big: straight to the file */
            if (write_all(s->w->fd, p, n, s->off + s->written) != 0) {
                wfail(s->w, "write failed: %s", strerror(errno));
                return -1;
            }
            s->written += n;
            return 0;
        }
        size_t k = STREAM_BUF - s->n < n ? STREAM_BUF - s->n : n;
        memcpy(s->buf + s->n, p, k);
        s->n += k, p += k, n -= k;
        if (s->n == STREAM_BUF && stream_flush(s) != 0) return -1;
    }
    return 0;
}

int hv_snap_stream_end(HvSnapStream *s, int keep) {
    if (!s) return -1;
    HvSnapWriter *w = s->w;
    w->streaming = 0;
    int rc = keep && stream_flush(s) == 0 && !w->failed ? 0 : -1;
    if (rc == 0) { /* a section put meanwhile may have taken its tag or the last place */
        char name[5];
        tag_name(s->tag, name);
        for (uint32_t i = 0; !w->failed && i < w->nsec; i++)
            if (w->sec[i].tag == s->tag) wfail(w, "section %s saved twice", name);
        if (!w->failed && w->nsec >= MAX_SECTIONS) wfail(w, "too many sections");
        if (w->failed) rc = -1;
    }
    if (rc == 0) {
        w->sec[w->nsec].tag = s->tag;
        w->sec[w->nsec].version = s->version;
        w->sec[w->nsec].data = NULL;
        w->sec[w->nsec].len = (size_t)s->written;
        w->sec[w->nsec].streamed = 1;
        w->sec[w->nsec].off = s->off;
        w->sec[w->nsec].crc = s->crc;
        w->nsec++;
        w->tail = s->off + s->written;
    }
    free(s->buf);
    free(s);
    return rc;
}

static void free_writer(HvSnapWriter *w) {
    if (w->fd >= 0) close(w->fd);
    for (uint32_t i = 0; i < w->nsec; i++) free(w->sec[i].data);
    free(w->partial), free(w->path), free(w);
}

void hv_snap_abort(HvSnapWriter *w) {
    if (!w) return;
    if (w->fd >= 0) close(w->fd), w->fd = -1;
    unlink(w->partial);
    if (w->saved) hv_ramfile_remove_copy(w->path, w->saved_gen); /* no snapshot names it */
    free_writer(w);
}

/* The directory's entry for the renamed file onto storage too. */
static void sync_dir(const char *path) {
    char *dir = strdup(path);
    if (!dir) return;
    char *slash = strrchr(dir, '/');
    if (slash == dir) slash[1] = 0;
    else if (slash) *slash = 0;
    else strcpy(dir, ".");
    int fd = open(dir, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        full_sync(fd);
        close(fd);
    }
    free(dir);
}

int hv_snap_discard(const char *path) {
    int rc = 0, gone = 0;
    if (unlink(path) == 0) gone = 1;
    else if (errno != ENOENT) {
        fprintf(stderr, "hengevm: snapshot: can't delete %s: %s\n", path, strerror(errno));
        rc = -1;
    }
    size_t n = strlen(path) + sizeof ".partial";
    char *partial = malloc(n);
    if (partial) {
        snprintf(partial, n, "%s.partial", path);
        unlink(partial); /* a save that never finished: nothing anyone loads */
        free(partial);
    }
    hv_code_store_remove(path); /* its translated code (core/codestore.h) */
    /* Only a snapshot that was there must stay gone after a crash: with none (a save called off or failed
       before it was complete), the flush of the storage's cache, slow after gigabytes written, is skipped */
    if (gone) sync_dir(path);
    /* Its copies of the memory files after it: one left by a crash between is named by no snapshot */
    if (hv_ramfile_remove_saved(path, NULL) != 0) fprintf(stderr, "hengevm: snapshot: a copy of the memory beside %s is left\n", path);
    return rc;
}

int hv_snap_commit(HvSnapWriter *w) {
    if (!w) return -1;
    int sync = !(w->flags & HV_SNAP_NO_SYNC);
    if (!w->failed && w->streaming) wfail(w, "committed while a section is still streaming");
    uint64_t off = w->ram_end > w->tail ? w->ram_end : w->tail;
    off = (off + 4095) & ~4095ull;
    static SnapHeader zero_header;
    SnapHeader h = zero_header;
    memcpy(h.magic, MAGIC, sizeof h.magic);
    h.format = HV_SNAP_FORMAT;
    h.header_bytes = sizeof h;
    h.nsections = w->nsec;
    for (uint32_t i = 0; !w->failed && i < w->nsec; i++) {
        h.sec[i].tag = w->sec[i].tag;
        h.sec[i].version = w->sec[i].version;
        if (w->sec[i].streamed) { /* already in the file, before the RAM */
            h.sec[i].off = w->sec[i].off;
            h.sec[i].len = w->sec[i].len;
            h.sec[i].crc = w->sec[i].crc;
            continue;
        }
        h.sec[i].off = off;
        h.sec[i].len = w->sec[i].len;
        h.sec[i].crc = hv_crc32c(0, w->sec[i].data, w->sec[i].len);
        if (write_all(w->fd, w->sec[i].data, w->sec[i].len, off) != 0) wfail(w, "write failed: %s", strerror(errno));
        off += (w->sec[i].len + 7) & ~7ull;
    }
    h.file_bytes = off;
    /* Called off (before each slow step): up to the header, nothing loadable exists, so it is only dropped */
    if (!w->failed && hv_snap_cancelled(w)) wfail(w, "cancelled");
    if (!w->failed && ftruncate(w->fd, (off_t)off) != 0) wfail(w, "can't size the file: %s", strerror(errno));
    /* Everything else on storage before the header that makes it valid. */
    if (!w->failed && sync && full_sync(w->fd) != 0) wfail(w, "sync failed: %s", strerror(errno));
    struct timeval tv;
    gettimeofday(&tv, NULL);
    h.saved_unix_us = (uint64_t)tv.tv_sec * 1000000u + (uint64_t)tv.tv_usec;
    h.crc = hv_crc32c(0, &h, offsetof(SnapHeader, crc));
    if (!w->failed && hv_snap_cancelled(w)) wfail(w, "cancelled");
    if (!w->failed && write_all(w->fd, &h, sizeof h, 0) != 0) wfail(w, "header write failed: %s", strerror(errno));
    if (!w->failed && sync && full_sync(w->fd) != 0) wfail(w, "sync failed: %s", strerror(errno));
    if (!w->failed && close(w->fd) != 0) wfail(w, "close failed: %s", strerror(errno));
    w->fd = -1;
    if (!w->failed && rename(w->partial, w->path) != 0) wfail(w, "can't rename into place: %s", strerror(errno));
    if (w->failed) {
        hv_snap_abort(w);
        return -1;
    }
    if (sync) sync_dir(w->path);
    /* Copies of the memory files no snapshot names now (an older save's, one a crash left) */
    if (hv_ramfile_remove_saved(w->path, w->saved ? w->saved_gen : NULL) != 0)
        fprintf(stderr, "hengevm: snapshot: an old copy of the memory beside %s is left\n", w->path);
    free_writer(w);
    return 0;
}

/* ---- Reading ---- */

struct HvSnapReader {
    int fd;
    char *path; /* where a copy of kept memory files is (<path>.ram-<generation>) */
    SnapHeader h;
    HvSnapBuf buf[MAX_SECTIONS]; /* each section's bytes: read into memory, or mapped from the file if big */
    int taken[MAX_SECTIONS];
};

void hv_snap_buf_free(HvSnapBuf *b) {
    if (!b) return;
    if (b->mapped) {
        if (b->base) munmap(b->base, b->map_len);
    } else {
        free(b->base);
    }
    memset(b, 0, sizeof *b);
}

void hv_snap_close(HvSnapReader *r) {
    if (!r) return;
    if (r->fd >= 0) close(r->fd);
    for (int i = 0; i < MAX_SECTIONS; i++) hv_snap_buf_free(&r->buf[i]);
    free(r->path);
    free(r);
}

/* A section's bytes: a big one mapped from the file (the system reads it as it is used and can let go of it
   again, so it never weighs on the app's memory), a small one read in. */
static int load_section(HvSnapReader *r, HvSnapBuf *b, uint64_t off, uint64_t len) {
    memset(b, 0, sizeof *b);
    if (len > MAP_OVER) {
        long page = sysconf(_SC_PAGESIZE);
        uint64_t start = off & ~(uint64_t)(page - 1);
        size_t map_len = (size_t)(off + len - start);
        void *m = mmap(NULL, map_len, PROT_READ, MAP_PRIVATE | MAP_FILE, r->fd, (off_t)start);
        if (m == MAP_FAILED) return -1;
        b->base = m, b->map_len = map_len, b->mapped = 1;
        b->p = (const uint8_t *)m + (off - start);
        b->len = (size_t)len;
        return 0;
    }
    uint8_t *p = malloc(len ? (size_t)len : 1);
    if (!p || read_all(r->fd, p, (size_t)len, off) != 0) {
        free(p);
        return -1;
    }
    b->base = p, b->p = p, b->len = (size_t)len;
    return 0;
}

HvSnapReader *hv_snap_open(const char *path, char *why, size_t why_len) {
    char tmp[200];
    if (!why) why = tmp, why_len = sizeof tmp;
    why[0] = 0;
    HvSnapReader *r = calloc(1, sizeof *r);
    if (!r) {
        snprintf(why, why_len, "out of memory");
        return NULL;
    }
    r->fd = open(path, O_RDONLY | O_CLOEXEC);
    r->path = strdup(path);
    struct stat st;
    SnapHeader *h = &r->h;
    if (r->fd < 0) {
        snprintf(why, why_len, "can't open it: %s", strerror(errno));
        goto bad;
    }
    fcntl(r->fd, F_NOCACHE, 1);
    if (fstat(r->fd, &st) != 0 || read_all(r->fd, h, sizeof *h, 0) != 0) {
        snprintf(why, why_len, "too short to be a snapshot");
        goto bad;
    }
    if (memcmp(h->magic, MAGIC, sizeof MAGIC) != 0) {
        snprintf(why, why_len, "not a Henge snapshot");
        goto bad;
    }
    if (h->format != HV_SNAP_FORMAT) {
        snprintf(why, why_len, "snapshot format %u; this Henge reads format %u", h->format, HV_SNAP_FORMAT);
        goto bad;
    }
    if (h->header_bytes != sizeof *h || h->crc != hv_crc32c(0, h, offsetof(SnapHeader, crc))) {
        snprintf(why, why_len, "the header is damaged");
        goto bad;
    }
    if (h->nsections > MAX_SECTIONS || (uint64_t)st.st_size < h->file_bytes) {
        snprintf(why, why_len, "the file is incomplete (%lld of %llu bytes)", (long long)st.st_size,
                 (unsigned long long)h->file_bytes);
        goto bad;
    }
    for (uint32_t i = 0; i < h->nsections; i++) {
        SnapSec *s = &h->sec[i];
        char name[5];
        tag_name(s->tag, name);
        for (uint32_t k = 0; k < i; k++)
            if (h->sec[k].tag == s->tag) {
                snprintf(why, why_len, "section %s is there twice", name);
                goto bad;
            }
        if (s->off < HEADER_BYTES || s->off > h->file_bytes || s->len > h->file_bytes - s->off) {
            snprintf(why, why_len, "section %s is out of place", name);
            goto bad;
        }
        if (load_section(r, &r->buf[i], s->off, s->len) != 0) {
            snprintf(why, why_len, "section %s can't be read", name);
            goto bad;
        }
        if (hv_crc32c(0, r->buf[i].p, s->len) != s->crc) {
            snprintf(why, why_len, "section %s is damaged", name);
            goto bad;
        }
    }
    return r;
bad:
    fprintf(stderr, "hengevm: snapshot: %s: %s\n", path, why);
    hv_snap_close(r);
    return NULL;
}

int hv_snap_get(HvSnapReader *r, uint32_t tag, uint32_t version, HvSnapIn *in) {
    char name[5];
    tag_name(tag, name);
    for (uint32_t i = 0; i < r->h.nsections; i++) {
        if (r->h.sec[i].tag != tag) continue;
        if (r->h.sec[i].version != version) {
            fprintf(stderr, "hengevm: snapshot: section %s is version %u; this Henge reads version %u\n", name,
                    r->h.sec[i].version, version);
            return -1;
        }
        if (r->taken[i]) {
            fprintf(stderr, "hengevm: snapshot: section %s was already taken\n", name);
            return -1;
        }
        in->p = r->buf[i].p;
        in->len = r->h.sec[i].len;
        in->at = 0;
        in->bad = 0;
        return 0;
    }
    fprintf(stderr, "hengevm: snapshot: no section %s\n", name);
    return -1;
}

int hv_snap_version(HvSnapReader *r, uint32_t tag) {
    for (uint32_t i = 0; i < r->h.nsections; i++)
        if (r->h.sec[i].tag == tag) return (int)r->h.sec[i].version;
    return -1;
}

int hv_snap_has(HvSnapReader *r, uint32_t tag) {
    for (uint32_t i = 0; i < r->h.nsections; i++)
        if (r->h.sec[i].tag == tag) return 1;
    return 0;
}

int hv_snap_take(HvSnapReader *r, uint32_t tag, uint32_t version, HvSnapBuf *b) {
    HvSnapIn in;
    memset(b, 0, sizeof *b);
    if (hv_snap_get(r, tag, version, &in) != 0) return -1;
    for (uint32_t i = 0; i < r->h.nsections; i++) {
        if (r->h.sec[i].tag != tag) continue;
        *b = r->buf[i];
        memset(&r->buf[i], 0, sizeof r->buf[i]);
        r->taken[i] = 1;
        return 0;
    }
    return -1;
}

typedef struct LoadJob {
    RamJob base;          /* first, entries, failed, why; pages and unit for regions kept in their file */
    HvRam *ram[HV_MAX_RAM]; /* the machine's region for each saved one */
    uint64_t size[HV_MAX_RAM];
    int fd;
    /* Saved whole into a region the machine keeps in a file: the file is emptied first (so zero chunks need
       nothing); the background writer puts what lands on storage. */
    int fill[HV_MAX_RAM];
    int version;
    /* Kept in files: the copy beside the snapshot to take them from, and the checks made after loading */
    uint8_t gen[HV_MAX_RAM][HV_RAMFILE_GEN];
    uint32_t nchecks[HV_MAX_RAM];
    HvRamFileCheck check[HV_MAX_RAM][HV_RAMFILE_CHECKS];
    char why[200];
} LoadJob;

static void *load_worker(void *arg) {
    LoadJob *lj = arg;
    RamJob *j = &lj->base;
    uint8_t *buf = malloc(CHUNK), *out = malloc(CHUNK);
    void *scratch = malloc(compression_decode_scratch_buffer_size(COMPRESSION_LZ4_RAW));
    if (!buf || !out || !scratch) {
        job_fail(j, "out of memory", 0);
        free(buf), free(out), free(scratch);
        return NULL;
    }
    for (;;) {
        uint32_t idx = __atomic_fetch_add(&j->next, 1, __ATOMIC_RELAXED);
        int r;
        if (idx >= j->first[j->nreg] || __atomic_load_n(&j->failed, __ATOMIC_RELAXED) || find_chunk(j, idx, &r)) break;
        HvRam *ram = lj->ram[r];
        uint64_t c = idx - j->first[r], at = chunk_at(j, r, c), unit = j->unit[r], file_at = j->file_off[r] + c * unit;
        /* Memory in files: through the files (hv_ramfile_put), not reading each page in first */
        uint8_t *dst = ram->file && !ram->readonly ? out : ram->host + at;
        size_t len = ram->size - at < unit ? (size_t)(ram->size - at) : (size_t)unit;
        uint32_t info = j->entries[2 * idx], crc = j->entries[2 * idx + 1], kind = INFO_KIND(info), n = INFO_LEN(info);
        if (ram->readonly) { /* not ours to write (code the host mapped read-only): it must already match */
            if (kind == KIND_ZERO ? !all_zero(dst, len) : hv_crc32c(0, dst, len) != crc) {
                job_fail(j, "read-only memory differs from the snapshot", ram->pa + at);
                break;
            }
            continue;
        }
        if (kind == KIND_ZERO) {
            /* A page the file didn't hold is written whatever it is: the file's own copy is older */
            if (j->pages[r] || (!lj->fill[r] && (j->flags & HV_SNAP_DIRTY_RAM))) {
                memset(dst, 0, len);
                if (dst == out && hv_ramfile_put(ram->file, at, out, len) != 0) {
                    job_fail(j, "writing the memory's files failed", ram->pa + at);
                    break;
                }
            }
            continue;
        }
        if (kind == KIND_RAW) {
            if (read_all(lj->fd, dst, len, file_at) != 0) {
                job_fail(j, "read failed", ram->pa + at);
                break;
            }
        } else {
            if (read_all(lj->fd, buf, n, file_at) != 0) {
                job_fail(j, "read failed", ram->pa + at);
                break;
            }
            if (compression_decode_buffer(dst, len, buf, n, scratch, COMPRESSION_LZ4_RAW) != len) {
                job_fail(j, "a compressed chunk is damaged", ram->pa + at);
                break;
            }
        }
        if (hv_crc32c(0, dst, len) != crc) {
            job_fail(j, "a chunk is damaged (checksum)", ram->pa + at);
            break;
        }
        if (dst == out && hv_ramfile_put(ram->file, at, out, len) != 0) {
            job_fail(j, "writing the memory's files failed", ram->pa + at);
            break;
        }
    }
    free(buf), free(out), free(scratch);
    return NULL;
}

static int section_version(HvSnapReader *r, uint32_t tag) {
    for (uint32_t i = 0; i < r->h.nsections; i++)
        if (r->h.sec[i].tag == tag) return (int)r->h.sec[i].version;
    return -1;
}

/* The RAM section read and checked against m, nothing changed: 0 with lj filled in, or -1 with why (for the
   person: a sentence) and detail (for the log). */
static int parse_ram(HvSnapReader *r, HvMachine *m, LoadJob *lj, const char **why) {
    *why = NULL;
    const char *damaged = "The saved state is damaged.", *layout = "The saved state had another number of processors or amount of memory.";
    int version = section_version(r, HV_SNAP_RAM);
    HvSnapIn in;
    if ((version != 1 && version != 2) || hv_snap_get(r, HV_SNAP_RAM, (uint32_t)version, &in) != 0) {
        *why = "The saved state is from another version of Henge and can't be loaded.";
        return -1;
    }
    lj->version = version;
    RamJob *j = &lj->base;
    lj->fd = r->fd;
    j->m = m;
    uint32_t nreg = 0, chunk = 0;
    HV_SNAP_IN(&in, nreg);
    HV_SNAP_IN(&in, chunk);
    const char *detail = NULL;
    if (in.bad || chunk != CHUNK || nreg != (uint32_t)m->nram) detail = "the machine's memory is laid out differently", *why = layout;
    uint64_t total = 0, listed = 0;
    int used[HV_MAX_RAM] = {0};
    for (uint32_t k = 0; !detail && k < nreg; k++) {
        uint64_t pa = 0, size = 0, file_off = 0;
        uint32_t ro = 0, n = 0, flags = 0, unit = CHUNK, nchecks = 0;
        uint8_t gen[HV_RAMFILE_GEN] = {0};
        HvRamFileCheck check[HV_RAMFILE_CHECKS];
        HV_SNAP_IN(&in, pa);
        HV_SNAP_IN(&in, size);
        HV_SNAP_IN(&in, file_off);
        HV_SNAP_IN(&in, ro);
        HV_SNAP_IN(&in, n);
        if (version == 2) {
            HV_SNAP_IN(&in, flags);
            HV_SNAP_IN(&in, unit);
            hv_snap_in(&in, gen, sizeof gen);
            HV_SNAP_IN(&in, nchecks);
            hv_snap_in(&in, check, sizeof check);
        }
        int hit = -1;
        for (int i = 0; i < m->nram; i++)
            if (!used[i] && m->ram[i].pa == pa && m->ram[i].size == size && (uint32_t)m->ram[i].readonly == ro) hit = i;
        if (in.bad || hit < 0) {
            detail = "the machine's memory is laid out differently", *why = layout;
            break;
        }
        HvRam *ram = &m->ram[hit];
        int kept = flags & 1u;
        if (flags & ~1u) {
            detail = "the memory table has flags this Henge doesn't know", *why = damaged;
            break;
        }
        if (kept) {
            if (!ram->file || ro) {
                detail = "the memory was kept in files, and this machine's memory isn't";
                *why = "The Mac's memory files couldn't be made, so the saved state can't be loaded.";
                break;
            }
            if (unit != hv_ramfile_page(ram->file) || n > size / unit || nchecks > HV_RAMFILE_CHECKS) {
                detail = "the memory files' page size differs", *why = layout;
                break;
            }
            if (hv_ramfile_check_saved(r->path, gen, size, lj->why, sizeof lj->why) != 0) {
                detail = "the copy of the memory files beside it isn't there whole (above)";
                *why = lj->why;
                break;
            }
            memcpy(lj->gen[k], gen, sizeof gen);
            lj->nchecks[k] = nchecks;
            memcpy(lj->check[k], check, sizeof check);
            listed += n;
        } else if (unit != CHUNK || n != (size + CHUNK - 1) / CHUNK) {
            detail = "the memory table is damaged", *why = damaged;
            break;
        }
        if (file_off < HEADER_BYTES || file_off > r->h.file_bytes || (uint64_t)n * unit > r->h.file_bytes - file_off) {
            detail = "the memory table is damaged", *why = damaged;
            break;
        }
        used[hit] = 1;
        lj->ram[k] = ram;
        lj->fill[k] = !kept && ram->file && !ro;
        j->file_off[k] = file_off;
        j->unit[k] = unit;
        j->pages[k] = kept ? (uint32_t *)1 : NULL; /* the list is placed below */
        j->first[k] = (uint32_t)total;
        total += n;
    }
    j->nreg = (int)nreg;
    if (!detail) {
        j->first[nreg] = (uint32_t)total;
        if (total > (1u << 24) || in.len - in.at != total * 8 + listed * 4) detail = "the memory table is damaged", *why = damaged;
    }
    if (!detail && ((uintptr_t)(in.p + in.at) & 3)) detail = "the memory table is misaligned", *why = damaged;
    if (!detail) {
        j->entries = (uint32_t *)(uintptr_t)(in.p + in.at); /* read-only use */
        for (uint64_t i = 0; i < total && !detail; i++) {
            int k;
            find_chunk(j, (uint32_t)i, &k);
            uint64_t unit = j->unit[k];
            uint32_t info = j->entries[2 * i], kind = INFO_KIND(info), n = INFO_LEN(info);
            if (!(kind == KIND_ZERO && n == 0) && !(kind == KIND_RAW && n <= unit) && !(kind == KIND_LZ4 && n > 0 && n < unit))
                detail = "the memory table is damaged", *why = damaged;
        }
        /* Each kept region's pages: in the region, ascending (so no page twice) */
        const uint32_t *list = j->entries + total * 2;
        for (uint32_t k = 0; !detail && k < nreg; k++) {
            if (!j->pages[k]) continue;
            uint64_t n = j->first[k + 1] - j->first[k], limit = lj->ram[k]->size / j->unit[k];
            j->pages[k] = (uint32_t *)(uintptr_t)list;
            for (uint64_t i = 0; i < n && !detail; i++)
                if (list[i] >= limit || (i && list[i] <= list[i - 1])) detail = "the memory file's page list is damaged", *why = damaged;
            list += n;
        }
    }
    if (detail) {
        fprintf(stderr, "hengevm: snapshot: %s\n", detail);
        for (uint32_t k = 0; k < HV_MAX_RAM; k++) j->pages[k] = NULL;
        return -1;
    }
    return 0;
}

int hv_snap_check_ram(HvSnapReader *r, HvMachine *m, char *why, size_t why_len) {
    static LoadJob zero_job;
    LoadJob *lj = malloc(sizeof *lj);
    if (!lj) return -1;
    *lj = zero_job;
    const char *w = NULL;
    int rc = parse_ram(r, m, lj, &w);
    if (rc != 0 && why && why_len) snprintf(why, why_len, "%s", w ? w : "The saved state is damaged.");
    free(lj);
    return rc;
}

int hv_snap_get_ram(HvSnapReader *r, HvMachine *m, int threads, unsigned flags) {
    uint64_t t0 = mach_absolute_time();
    static LoadJob zero_job;
    LoadJob *lj = malloc(sizeof *lj);
    if (!lj) return -1;
    *lj = zero_job;
    RamJob *j = &lj->base;
    const char *why;
    if (parse_ram(r, m, lj, &why) != 0) {
        free(lj);
        return -1;
    }
    j->flags = flags;
    for (int k = 0; k < j->nreg; k++) {
        /* Kept: the copy beside the snapshot becomes the memory (cloned: the copy stays as it was), and the save's own
           pages go over it. Saved whole into files: they are emptied first. */
        HvRamFile *f = lj->ram[k]->file;
        if (j->pages[k] && hv_ramfile_adopt(f, r->path, lj->gen[k], lj->why, sizeof lj->why) != 0) {
            fprintf(stderr, "hengevm: snapshot: the copy of the memory couldn't be taken: %s\n", lj->why);
            free(lj);
            return -1;
        }
        if (lj->fill[k] && hv_ramfile_reset(f) != 0) {
            fprintf(stderr, "hengevm: snapshot: the memory files couldn't be emptied for the saved memory\n");
            free(lj);
            return -1;
        }
    }
    run_threads(threads, load_worker, lj);
    int failed = j->failed;
    if (failed)
        fprintf(stderr, "hengevm: snapshot: loading RAM failed at %#llx: %s\n", (unsigned long long)j->why_at, j->why);
    /* The copy with the save's pages over it is the memory that was saved: sampled pieces checked */
    for (int k = 0; !failed && k < j->nreg; k++)
        if (j->pages[k] && hv_ramfile_verify(lj->ram[k]->file, lj->check[k], lj->nchecks[k]) != 0) {
            fprintf(stderr, "hengevm: snapshot: the memory loaded isn't the memory saved\n");
            failed = 1;
        }
    if (!failed && m->log) {
        uint64_t bytes = 0;
        for (int k = 0; k < j->nreg; k++) bytes += (uint64_t)(j->first[k + 1] - j->first[k]) * j->unit[k];
        fprintf(stderr, "hengevm: snapshot: RAM loaded (version %d, %llu MB from the save) in %.0f ms\n", lj->version,
                (unsigned long long)(bytes >> 20), ms_since(t0));
    }
    free(lj);
    return failed ? -1 : 0;
}

/* ---- The CPUs and the clock ---- */

/* Everything architectural an HvCpu holds, in file order. Not saved: what the engine rebuilds (TLB,
   branch caches, thunks), the exclusive monitor (cleared, as any exception may), the ICC registers
   (the GIC's section), PAuth keys (never kept: signing is a no-op). */
#define CPU_FIELDS(X)                                                                                               \
    X(x) X(sp) X(pc) X(nzcv) X(v) X(fpcr) X(fpsr)                                                                   \
    X(tpidr_el0) X(tpidrro_el0) X(tpidr_el1) X(elr_el1) X(spsr_el1) X(sp_el0) X(daif) X(sp_el1) X(pstate_extra)     \
    X(sctlr_el1) X(esr_el1) X(far_el1) X(spsel) X(el)                                                               \
    X(tcr_el1) X(ttbr0_el1) X(ttbr1_el1) X(mair_el1) X(amair_el1) X(vbar_el1) X(par_el1) X(afsr0_el1) X(afsr1_el1) \
    X(contextidr_el1) X(cpacr_el1) X(mdscr_el1) X(csselr_el1) X(cntkctl_el1) X(actlr_el1)                           \
    X(cntv_ctl) X(cntv_cval) X(cntp_ctl) X(cntp_cval) X(cntvoff) X(mpidr) X(off)

/* The experimental Rosetta mode's own state, after each CPU's CPU_FIELDS in a version 2 CPU section, which only a
   Mac in that mode writes (with the mode after the CPU count). Every other Mac writes version 1, the same bytes as
   before the mode existed: older Henges still read it, and refuse version 2 rather than resume it wrong. A version 1
   section is mode 0 with these zero, which is what such a Mac had. FPCR is in CPU_FIELDS (the guest's value in this
   mode); the host's AFP controls are made again from it (hv_rosetta_fp_apply), ACTLR_EL1 is in CPU_FIELDS. */
#define ROSETTA_FIELDS(X) X(apflags) X(aspsr_el1) X(afpcr_el0)

#define TICKS_PER_SECOND 24000000ull

int hv_snap_put_cpus(HvSnapWriter *w, HvMachine *m) {
    if (w->failed) return -1;
    uint64_t counter;
    if (hv_machine_paused_counter(m, &counter) != 0) {
        wfail(w, "the machine is not paused");
        return -1;
    }
    HvSnapOut o = {0};
    uint32_t n = (uint32_t)m->ncpus, mode = (uint32_t)m->rosetta, version = mode ? 2 : 1;
    HV_SNAP_OUT(&o, n);
    if (version == 2) HV_SNAP_OUT(&o, mode);
    for (int i = 0; i < m->ncpus; i++) {
        HvCpu *cpu = m->cpus[i];
        if (!cpu || cpu->halted) {
            free(o.p);
            wfail(w, "CPU %d is %s", i, cpu ? "stopping" : "missing");
            return -1;
        }
#define OUT(f) HV_SNAP_OUT(&o, cpu->f);
        CPU_FIELDS(OUT)
        if (version == 2) { ROSETTA_FIELDS(OUT) }
#undef OUT
    }
    if (hv_snap_put(w, HV_SNAP_CPUS, version, &o) != 0) return -1;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t freq = TICKS_PER_SECOND, unix_us = (uint64_t)tv.tv_sec * 1000000u + (uint64_t)tv.tv_usec;
    HvSnapOut t = {0};
    HV_SNAP_OUT(&t, counter);
    HV_SNAP_OUT(&t, freq);
    HV_SNAP_OUT(&t, unix_us); /* the real time it stopped: macOS's clock is that much behind on restore */
    return hv_snap_put(w, HV_SNAP_TIME, 1, &t);
}

int hv_snap_cpus_rosetta(HvSnapReader *r) {
    HvSnapIn in;
    int version = hv_snap_version(r, HV_SNAP_CPUS);
    if (version == 1) return 0;
    if (version != 2 || hv_snap_get(r, HV_SNAP_CPUS, 2, &in) != 0) return -1;
    uint32_t n, mode;
    HV_SNAP_IN(&in, n);
    HV_SNAP_IN(&in, mode);
    return in.bad || (mode != 1 && mode != 2) ? -1 : (int)mode;
}

int hv_snap_get_cpus(HvSnapReader *r, HvMachine *m) {
    HvSnapIn in, t;
    int version = hv_snap_version(r, HV_SNAP_CPUS) == 2 ? 2 : 1; /* any other: hv_snap_get says which */
    if (hv_snap_get(r, HV_SNAP_CPUS, (uint32_t)version, &in) != 0 || hv_snap_get(r, HV_SNAP_TIME, 1, &t) != 0) return -1;
    uint64_t counter, freq, unix_us;
    HV_SNAP_IN(&t, counter);
    HV_SNAP_IN(&t, freq);
    HV_SNAP_IN(&t, unix_us);
    if (hv_snap_in_done(&t) != 0 || freq != TICKS_PER_SECOND) {
        fprintf(stderr, "hengevm: snapshot: the clock section is damaged\n");
        return -1;
    }
    uint32_t n, mode = 0;
    HV_SNAP_IN(&in, n);
    if (in.bad || n != (uint32_t)m->ncpus) {
        fprintf(stderr, "hengevm: snapshot: it has %u CPUs and this machine %d\n", n, m->ncpus);
        return -1;
    }
    if (version == 2) HV_SNAP_IN(&in, mode);
    if (in.bad || (version == 2 && mode != 1 && mode != 2)) {
        fprintf(stderr, "hengevm: snapshot: the CPU section is damaged\n");
        return -1;
    }
    for (uint32_t i = 0; i < n; i++)
        if (!m->cpus[i]) {
            fprintf(stderr, "hengevm: snapshot: CPU %u is missing\n", i);
            return -1;
        }
    /* The mode it was saved in, whatever this machine was made with: the guest's state only means that there.
       The machine hasn't run and holds no code yet: the code store is loaded after this (host/vm.c), and only if it
       was made in this mode (its settings check). */
    int set = hv_machine_set_rosetta(m, (int)mode);
    if (set != 0) {
        if (set == -2) fprintf(stderr, "hengevm: snapshot: saved in Rosetta mode %u, but code for another mode is loaded\n", mode);
        else fprintf(stderr, "hengevm: snapshot: it was saved in the experimental Rosetta mode, which this host can't run\n");
        return -1;
    }
    if (mode && m->log) fprintf(stderr, "hengevm: snapshot: resuming in the experimental Rosetta mode (%u)\n", mode);
    for (uint32_t i = 0; i < n; i++) {
        HvCpu *cpu = m->cpus[i];
        uint64_t mpidr = cpu->mpidr; /* the board's: PSCI finds CPUs by it */
        hv_cpu_reset_index(cpu, m, i);
#define IN(f) HV_SNAP_IN(&in, cpu->f);
        CPU_FIELDS(IN)
        if (version == 2) { ROSETTA_FIELDS(IN) }
#undef IN
        if (in.bad || cpu->el > 1 || cpu->spsel > 1 || (cpu->off != 0 && cpu->off != 1) || cpu->mpidr != mpidr ||
            (cpu->apflags & ~HV_APFLAGS_MASK) || cpu->aspsr_el1 > 7 || cpu->afpcr_el0 > 3) {
            fprintf(stderr, "hengevm: snapshot: CPU %u's state is damaged\n", i);
            return -1;
        }
        cpu->el0_cnt = (uint32_t)(cpu->cntkctl_el1 & 3);
        cpu->excl_addr = ~0ull;
        cpu->mpidr_el1 = cpu->mpidr;
        hv_tlb_flush(cpu);
        hv_ijc_switch(cpu); /* the branch cache's ids for the address space now in TTBR0 */
    }
    if (hv_snap_in_done(&in) != 0) {
        fprintf(stderr, "hengevm: snapshot: the CPU section is damaged\n");
        return -1;
    }
    hv_machine_restore_paused(m, counter);
    return 0;
}
