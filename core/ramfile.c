#include "ramfile.h"

#include <arm_acle.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_error.h>
#include <mach/vm_map.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/clonefile.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/qos.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Not in iOS's headers, in its libSystem: the process's own I/O, for the log (iOS reports an app that writes
   more than its daily allowance). */
int proc_pid_rusage(int pid, int flavor, rusage_info_t *buffer);

#define SEG (128ull << 20)        /* one file of the memory */
#define SLICE (8ull << 20)        /* the writer's batch: copied out and written at once; a hold waits for at most one */
#define SAVE_SLICE (64ull << 20)  /* a save thread's share at a time: the cancel is checked between */
#define MEM_CHUNK (128ull << 20)  /* one memory entry of memory outside the footprint at most */
#define MIN_FREE (1ull << 30)     /* the writer stops below this much free storage */
#define LEAVE_S 3.0               /* what leaving may take to write: the writer keeps the modified pages below it */
#define HIGH_MIN (256ull << 20)
#define HIGH_MAX (4ull << 30)
#define LOG_EVERY_NS (600ull * 1000000000ull)
#define SAVED_VERSION 2u

/* <snap>.ram-<gen>/id: what the saved copy is. Written and flushed before the snapshot naming it exists. */
typedef struct SavedId {
    char magic[8]; /* "HENGERAM" */
    uint32_t version, nseg;
    uint8_t gen[HV_RAMFILE_GEN];
    uint64_t size, seg;
    uint32_t crc, pad; /* CRC32C of everything above */
} SavedId;
_Static_assert(sizeof(SavedId) == 56, "saved copy id layout");

/* A writer's own room: pages are copied out of the memory before they're written (never written from the memory
   itself: storage reading guest pages would hold them while the Mac wants them). */
typedef struct Work {
    uint8_t *buf; /* SLICE bytes */
    uint32_t *pages, *cleared;
    uint8_t *zero;
} Work;
static void work_free(Work *w);

struct HvRamFile {
    char *dir;
    uint8_t *base;
    uint64_t size, page, npages;
    unsigned nseg;
    int *fd;         /* the run files, written with pwrite (never mapped) */
    int nofoot;      /* the memory is outside the footprint (as plain guest RAM is), not plain anonymous memory */
    uint8_t *dirty;  /* per page, HV_DIRTY_* (core/machine.h): what the files may not hold */
    HvRamFileRevoke revoke;
    void *revoke_ctx;
    Work *wwork; /* the background writer's (hv_ramfile_writer_pass) */
    int adopted; /* holds a saved copy (a resume put it there), not zeros or a Mac that ran here */
    int check_all;
    int64_t spoil; /* hv_ramfile_test_spoil */
    _Atomic uint32_t *pins; /* GPU mappings over each page */
    uint64_t open_io;       /* Henge's bytes written to storage when this opened */
    _Atomic uint8_t *seen;  /* under a GPU mapping since the last save: in the next save whatever the pins say */
    /* An auto-save between hv_ramfile_claim_gpu and hv_ramfile_claim_stop: a page a new GPU mapping covers is claimed
       as it is mapped (under open_lock) */
    HvRamFileClaim claim;
    void *claim_ctx;

    pthread_mutex_t lock;
    pthread_cond_t cond;
    int writer_started, hold, busy;
    _Atomic int holding; /* read by a pass between batches */

    /* Policy: above high modified, write down to low, within a bucket of rate bytes/s holding at most burst.
       high comes from how fast a save writes (rate_est): what leaving can write in LEAVE_S. */
    uint64_t high_fixed, burst;
    double rate, tokens, rate_est;
    uint64_t refilled_ns;
    unsigned period_ms;
    int low_space; /* logged once per spell */

    /* Accounting */
    _Atomic uint64_t written, flushes, scans, last_dirty, last_high;
    uint64_t logged_ns, logged_written, logged_io;
    _Atomic uint64_t busy_ns; /* the writer's time scanning and writing (its cost to the running Mac) */
    uint64_t logged_busy;
};

static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW); }

__attribute__((target("crc"))) static uint32_t crc32c(const void *data, size_t len) {
    const uint8_t *p = data;
    uint32_t c = 0;
    for (; len >= 8; p += 8, len -= 8) {
        uint64_t w;
        memcpy(&w, p, 8);
        c = __crc32cd(c, w);
    }
    while (len--) c = __crc32cb(c, *p++);
    return c;
}

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "hengevm: ram file: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static void say(char *why, size_t len, const char *s) {
    if (why && len) snprintf(why, len, "%s", s);
}

static int full_flush(int fd) {
    if (fcntl(fd, F_FULLFSYNC) == 0) return 0;
    return fsync(fd); /* a filesystem without it (tests on some volumes) */
}

static void set_class(int fd) {
#ifdef F_SETPROTECTIONCLASS
    /* Class C: readable once the iPad has been unlocked after it started, so a Mac can save and resume locked. */
    if (fcntl(fd, F_SETPROTECTIONCLASS, 3) != 0 && errno != ENOTSUP && errno != EINVAL) logf_("protection class: %s", strerror(errno));
#else
    (void)fd;
#endif
}

static int sync_dir(const char *dir) {
    int fd = open(dir, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    int rc = fsync(fd);
    close(fd);
    return rc;
}

static void parent_of(const char *path, char *out, size_t len) {
    snprintf(out, len, "%s", path);
    char *s = strrchr(out, '/');
    if (s == out) s[1] = 0;
    else if (s) *s = 0;
    else snprintf(out, len, ".");
}

/* A folder of plain files, and the folder. 0 (or it wasn't there), or -1. */
static int remove_tree(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return errno == ENOENT ? 0 : -1;
    int rc = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (unlinkat(dirfd(d), e->d_name, 0) != 0 && errno != ENOENT) rc = -1;
    }
    closedir(d);
    if (rmdir(dir) != 0 && errno != ENOENT) rc = -1;
    return rc;
}

static uint64_t seg_len(const HvRamFile *f, unsigned i) {
    uint64_t off = (uint64_t)i * SEG;
    return f->size - off < SEG ? f->size - off : SEG;
}

static void saved_path(const char *snap, const uint8_t gen[HV_RAMFILE_GEN], char *out, size_t len) {
    char hex[2 * HV_RAMFILE_GEN + 1];
    for (int i = 0; i < HV_RAMFILE_GEN; i++) snprintf(hex + 2 * i, 3, "%02x", gen[i]);
    snprintf(out, len, "%s.ram-%s", snap, hex);
}

/* ---- The open memories (for the GPU's mappings) ---- */

#define MAX_OPEN 8
/* open_lock is a leaf lock: nothing done under it may wait (for the vCPUs, the writer, the GPU) or take another
   lock, apart from stdio's (itself a leaf) when a failure is logged, as fresh() does in a give-back. It is taken where
   the open memories are listed (hv_ramfile_open/close), by the balloon's give-back on a vCPU (hv_ramfile_discard), by
   the GPU's mappings on the GPU worker and in Metal's deallocators on the presenter (hv_ramfile_gpu_map/unmap/relist),
   by a save (gpu_saved, hv_ramfile_take), and where the claim hook is set and cleared (hv_ramfile_claim_gpu/stop):
   any wait under it could stall one of those on another. The case to watch is that claim hook (f->claim), which runs
   under it: from pin_range as a page is mapped, and from hv_ramfile_claim_gpu over the pages already mapped. Nothing
   sets one in a running Mac yet (pvg_lite.h). The auto-save that will must claim without waiting under this lock, or
   at most for a page whose holder never takes it: the balloon's vCPU finishes its own claim (hv_guard_claim_range)
   before its give-back takes this lock, so it holds no page then. A claim that took another lock, or waited for the
   vCPUs, would break this. */
static pthread_mutex_t open_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t io_written(void);
static HvRamFile *open_files[MAX_OPEN];

/* The memory holding p, and p's page in it. Under open_lock. */
static HvRamFile *find_page(const void *p, uint64_t *page) {
    for (int i = 0; i < MAX_OPEN; i++) {
        HvRamFile *f = open_files[i];
        if (f && (const uint8_t *)p >= f->base && (const uint8_t *)p < f->base + f->size) {
            *page = (uint64_t)((const uint8_t *)p - f->base) / f->page;
            return f;
        }
    }
    return NULL;
}

/* ---- The memory ---- */

/* Not in the headers: what plain guest RAM is made with (devices/vmapple.c) */
extern kern_return_t mach_memory_entry_ownership(mach_port_t entry, task_t owner, int ledger_tag, int ledger_flags);

/* Zeros at [off, off + len) of the memory (whole pages), the pages there before given back to the system. Memory
   outside the footprint where the system allows it, as plain guest RAM is; else plain anonymous memory. Never a
   file: a mapped file's pages are the first the system takes back under pressure, each read again from storage the
   next time it is touched, and that (with the writer's flushes holding pages busy) was what stalled the Mac. 0, or
   -1 (logged; the range is then as it was, or zeros, or unmapped). */
static int fresh(HvRamFile *f, uint64_t off, uint64_t len) {
    enum { MAP_MEM_LEDGER_TAGGED_ = 0x002000, MAP_MEM_NAMED_CREATE_ = 0x020000, LEDGER_TAG_DEFAULT = 1, LEDGER_NO_FOOTPRINT = 1 };
    uint64_t at = off, end = off + len;
    while (f->nofoot && at < end) {
        uint64_t n = end - at < MEM_CHUNK ? end - at : MEM_CHUNK;
        memory_object_size_t got = n;
        mach_port_t entry = MACH_PORT_NULL;
        kern_return_t kr = mach_make_memory_entry_64(mach_task_self(), &got, 0,
                                                     MAP_MEM_NAMED_CREATE_ | MAP_MEM_LEDGER_TAGGED_ | VM_PROT_READ | VM_PROT_WRITE, &entry,
                                                     MACH_PORT_NULL);
        if (kr == KERN_SUCCESS) kr = mach_memory_entry_ownership(entry, TASK_NULL, LEDGER_TAG_DEFAULT, LEDGER_NO_FOOTPRINT);
        if (kr == KERN_SUCCESS) {
            vm_address_t to = (vm_address_t)(f->base + at);
            kr = vm_map(mach_task_self(), &to, n, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, entry, 0, FALSE, VM_PROT_READ | VM_PROT_WRITE,
                        VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
            if (kr == KERN_SUCCESS && to != (vm_address_t)(f->base + at)) kr = KERN_NO_SPACE;
        }
        if (entry != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), entry);
        if (kr != KERN_SUCCESS) {
            logf_("memory outside the footprint refused (%s): plain memory from here", mach_error_string(kr));
            f->nofoot = 0;
            break;
        }
        at += n;
    }
    if (at < end && mmap(f->base + at, end - at, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) != f->base + at) {
        logf_("no memory at %llu MB: %s", (unsigned long long)(at >> 20), strerror(errno));
        return -1;
    }
    return 0;
}

/* After the page changed (a full fence before): dirty, unless it is already (LENT stays LENT). */
static void mark(HvRamFile *f, uint64_t pg) {
    uint8_t v = HV_DIRTY_CLEAN;
    if (__atomic_load_n(&f->dirty[pg], __ATOMIC_RELAXED) == HV_DIRTY_CLEAN)
        __atomic_compare_exchange_n(&f->dirty[pg], &v, HV_DIRTY, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED);
}

uint8_t *hv_ramfile_dirty_map(HvRamFile *f) { return f->dirty; }

void hv_ramfile_set_revoke(HvRamFile *f, HvRamFileRevoke fn, void *ctx) {
    pthread_mutex_lock(&f->lock);
    f->revoke = fn, f->revoke_ctx = ctx;
    pthread_mutex_unlock(&f->lock);
}

void hv_ramfile_mark_dirty(HvRamFile *f, uint64_t off, uint64_t len) {
    if (!len || off >= f->size) return;
    if (len > f->size - off) len = f->size - off;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    for (uint64_t pg = off / f->page; pg <= (off + len - 1) / f->page; pg++) mark(f, pg);
}

/* ---- Making the files ---- */

/* The run folder's files made again: empty (saved NULL) or clones of a saved copy's. Only ever written with pwrite;
   the memory is never theirs. A failure part way leaves them mixed: the caller doesn't use the memory. */
static int make_set(HvRamFile *f, const char *saved) {
    for (unsigned i = 0; i < f->nseg; i++) {
        char now[1100], tmp[1110], src[1100];
        snprintf(now, sizeof now, "%s/%04u", f->dir, i);
        snprintf(tmp, sizeof tmp, "%s.new", now);
        uint64_t len = seg_len(f, i);
        (void)unlink(tmp);
        if (saved) {
            snprintf(src, sizeof src, "%s/%04u", saved, i);
            if (clonefile(src, tmp, 0) != 0) {
                logf_("can't clone %s: %s", src, strerror(errno));
                return -1;
            }
        }
        int fd = open(tmp, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd < 0) {
            logf_("can't make %s: %s", tmp, strerror(errno));
            return -1;
        }
        set_class(fd);
        /* Not through the cache: the writer's pages would only crowd the Mac's out of it */
        (void)fcntl(fd, F_NOCACHE, 1);
        struct stat st;
        if ((!saved && ftruncate(fd, (off_t)len) != 0) || fstat(fd, &st) != 0 || (uint64_t)st.st_size != len) {
            logf_("%s isn't %llu bytes: %s", tmp, (unsigned long long)len, strerror(errno));
            close(fd);
            return -1;
        }
        if (f->fd[i] >= 0) close(f->fd[i]);
        f->fd[i] = fd;
        /* Not under its name, a later save's copy would hold the file that was there before: not used */
        if (rename(tmp, now) != 0) {
            logf_("can't rename %s: %s", tmp, strerror(errno));
            return -1;
        }
    }
    return 0;
}

HvRamFile *hv_ramfile_open(const char *dir, uint64_t size) {
    HvRamFile *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->base = MAP_FAILED;
    f->page = (uint64_t)getpagesize();
    f->size = size;
    f->npages = size / f->page;
    if (!size || size % f->page || f->npages > UINT32_MAX) {
        logf_("%llu bytes isn't a whole number of %llu-byte pages", (unsigned long long)size, (unsigned long long)f->page);
        free(f);
        return NULL;
    }
    f->nseg = (unsigned)((size + SEG - 1) / SEG);
    f->dir = strdup(dir);
    f->fd = malloc(f->nseg * sizeof *f->fd);
    f->dirty = calloc(f->npages + 8, 1); /* + 8: counted 8 at a time */
    f->pins = calloc(f->npages, sizeof *f->pins);
    f->seen = calloc(f->npages, sizeof *f->seen);
    pthread_mutex_init(&f->lock, NULL);
    pthread_cond_init(&f->cond, NULL);
    f->rate = 8.0 * (1ull << 30) / 3600.0;
    f->open_io = io_written();
    f->burst = 2ull << 30;
    f->tokens = (double)f->burst;
    f->rate_est = 300.0 * (1 << 20); /* until a save measures it: low, so the writer starts early */
    f->period_ms = 3000;
    f->spoil = -1;
    if (!f->dir || !f->fd || !f->dirty || !f->pins || !f->seen) goto fail;
    for (unsigned i = 0; i < f->nseg; i++) f->fd[i] = -1;

    /* Whatever an earlier run left: no save refers to a run folder. */
    if (remove_tree(dir) != 0) logf_("couldn't remove all of an old %s: %s", dir, strerror(errno));
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        logf_("can't make %s: %s", dir, strerror(errno));
        goto fail;
    }
    f->base = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (f->base == MAP_FAILED) {
        logf_("no room for %llu MB of memory: %s", (unsigned long long)(size >> 20), strerror(errno));
        goto fail;
    }
    f->nofoot = !getenv("HV_RAM_NO_LEDGER");
    if (fresh(f, 0, size) != 0 || make_set(f, NULL) != 0) goto fail;

    pthread_mutex_lock(&open_lock);
    int slot = -1;
    for (int i = 0; i < MAX_OPEN && slot < 0; i++)
        if (!open_files[i]) slot = i;
    if (slot >= 0) open_files[slot] = f;
    pthread_mutex_unlock(&open_lock);
    if (slot < 0) {
        logf_("too many open");
        goto fail;
    }
    logf_("%s, %llu MB in %u files; the memory itself is %s", dir, (unsigned long long)(size >> 20), f->nseg,
          f->nofoot ? "outside the footprint" : "plain memory");
    return f;
fail:
    if (f->base != MAP_FAILED) munmap(f->base, size);
    if (f->fd)
        for (unsigned i = 0; i < f->nseg; i++)
            if (f->fd[i] >= 0) close(f->fd[i]);
    if (f->dir) remove_tree(f->dir);
    pthread_mutex_destroy(&f->lock);
    pthread_cond_destroy(&f->cond);
    free(f->dirty);
    free(f->pins);
    free(f->seen);
    free(f->fd);
    free(f->dir);
    free(f);
    return NULL;
}

void hv_ramfile_close(HvRamFile *f) {
    if (!f) return;
    pthread_mutex_lock(&open_lock);
    for (int i = 0; i < MAX_OPEN; i++)
        if (open_files[i] == f) open_files[i] = NULL;
    pthread_mutex_unlock(&open_lock);
    /* A started writer keeps running on it: tests close only files without one. */
    munmap(f->base, f->size);
    for (unsigned i = 0; i < f->nseg; i++)
        if (f->fd[i] >= 0) close(f->fd[i]);
    remove_tree(f->dir);
    if (f->wwork) work_free(f->wwork), free(f->wwork);
    free(f->dirty);
    free(f->pins);
    free(f->seen);
    free(f->fd);
    free(f->dir);
    free(f);
}

uint8_t *hv_ramfile_base(const HvRamFile *f) { return f->base; }
uint64_t hv_ramfile_size(const HvRamFile *f) { return f->size; }
uint64_t hv_ramfile_page(const HvRamFile *f) { return f->page; }

static int reset_held(HvRamFile *f) {
    if (fresh(f, 0, f->size) != 0) return -1;
    memset(f->dirty, 0, f->npages);
    if (make_set(f, NULL) != 0) return -1;
    f->adopted = 0;
    logf_("%s emptied", f->dir);
    return 0;
}

/* Held: no pass is writing (to files about to be closed) or reading marks about to be cleared. */
int hv_ramfile_reset(HvRamFile *f) {
    hv_ramfile_writer_hold(f, 1);
    int r = reset_held(f);
    hv_ramfile_writer_hold(f, 0);
    return r;
}

int hv_ramfile_make_fresh(HvRamFile *f) { return f->adopted ? hv_ramfile_reset(f) : 0; }

/* How long the give-back holds open_lock (the GPU worker, and Metal's deallocators on the presenter, can wait that
   long): the longest and the count, logged at most once a minute, at a give-back a minute after the last line (the
   iPad's check reads it: over about 1 ms, the give-back is to be split into pieces that drop the lock between). */
static _Atomic uint64_t hold_max_ns, holds, hold_line_ns;
static void note_hold(uint64_t t0) {
    uint64_t now = now_ns(), dt = now - t0, was = atomic_load_explicit(&hold_max_ns, memory_order_relaxed);
    while (dt > was && !atomic_compare_exchange_weak_explicit(&hold_max_ns, &was, dt, memory_order_relaxed, memory_order_relaxed)) {
    }
    atomic_fetch_add_explicit(&holds, 1, memory_order_relaxed);
    uint64_t last = atomic_load_explicit(&hold_line_ns, memory_order_relaxed);
    /* now > last: a give-back that read its clock before another's line moved it on writes none */
    if ((!last || (now > last && now - last >= 60000000000ull)) && atomic_compare_exchange_strong(&hold_line_ns, &last, now)) {
        uint64_t n = atomic_exchange(&holds, 0), max = atomic_exchange(&hold_max_ns, 0);
        /* In integers: a give-back runs on a vCPU (virtio_balloon.c), where floating point would be the guest's.
           Microseconds rounded, as %.3f of the milliseconds was. */
        uint64_t us = max / 1000 + (max % 1000 >= 500);
        logf_("give-backs since the last such line: %llu, the longest held the GPU mappings' lock %llu.%03llu ms",
              (unsigned long long)n, (unsigned long long)(us / 1000), (unsigned long long)(us % 1000));
    }
}

void hv_ramfile_hold_stats(uint64_t *max_ns, uint64_t *n) {
    *max_ns = atomic_load_explicit(&hold_max_ns, memory_order_relaxed);
    *n = atomic_load_explicit(&holds, memory_order_relaxed);
}

int hv_ramfile_discard(HvRamFile *f, uint64_t off, uint64_t len) {
    uint64_t first = (off + f->page - 1) / f->page, end = (off + len) / f->page;
    if (off > f->size || len > f->size - off) return -1;
    int rc = 0;
    /* Under the lock a GPU mapping pins under, before it remaps (hv_ramfile_gpu_map): a page pinned after its check
       here would be one the GPU then maps while it is swapped for new memory, the GPU keeping the old (the guest's
       later writes never reaching it). With the lock, a pin is either seen (the page zeroed in place) or comes after
       (the remap takes the new memory). Nothing under it waits for the vCPUs. */
    pthread_mutex_lock(&open_lock);
    uint64_t t0 = now_ns();
    for (uint64_t i = first; i < end;) {
        /* Under a GPU mapping (never, for pages the guest gave back): its pages stay where the GPU has them */
        if (atomic_load_explicit(&f->pins[i], memory_order_acquire)) {
            memset(f->base + i * f->page, 0, f->page);
            i++;
            continue;
        }
        uint64_t e = i + 1;
        while (e < end && !atomic_load_explicit(&f->pins[e], memory_order_acquire)) e++;
        if (fresh(f, i * f->page, (e - i) * f->page) != 0) rc = -1;
        i = e;
    }
    pthread_mutex_unlock(&open_lock);
    note_hold(t0); /* its line outside the lock */
    /* After: whatever the files held there, the page is zeros now (the writer makes it a hole) */
    if (end > first) hv_ramfile_mark_dirty(f, first * f->page, (end - first) * f->page);
    return rc;
}

uint64_t hv_ramfile_unallocated(const HvRamFile *f) {
    uint64_t have = 0;
    for (unsigned i = 0; i < f->nseg; i++) {
        struct stat st;
        if (fstat(f->fd[i], &st) != 0) return f->size;
        have += (uint64_t)st.st_blocks * 512;
    }
    return have >= f->size ? 0 : f->size - have;
}

/* ---- GPU mappings ---- */

typedef struct GpuMap {
    const void *key;
    size_t n;
    struct GpuMap *next;
    void *src[];
} GpuMap;
#define GPU_BUCKETS 1024
static GpuMap *gpu_maps[GPU_BUCKETS];
/* A GPU mapping whose pages couldn't be listed: what the GPU wrote there may be in no save's account, so every save
   of this run is whole. */
static _Atomic int gpu_untracked;

static unsigned gpu_bucket(const void *key) { return (unsigned)(((uintptr_t)key >> 14) * 2654435761u) % GPU_BUCKETS; }

/* Each page of [p, p + 16 KB) in an open memory. Under open_lock. */
static void pin_range(void *p, int add) {
    for (uint64_t at = 0; at < 0x4000;) {
        uint64_t page;
        HvRamFile *f = find_page((uint8_t *)p + at, &page);
        if (!f) return;
        if (add) {
            if (f->claim) f->claim(f->claim_ctx, page * f->page, f->page); /* before the GPU can write it */
            atomic_store_explicit(&f->seen[page], 1, memory_order_relaxed);
            atomic_fetch_add_explicit(&f->pins[page], 1, memory_order_release);
        } else {
            /* Marked dirty before it stops counting: a save that sees no mapping then sees it dirty. */
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            mark(f, page);
            uint32_t was = atomic_load_explicit(&f->pins[page], memory_order_relaxed);
            if (was) atomic_fetch_sub_explicit(&f->pins[page], 1, memory_order_release);
        }
        at = (page + 1) * f->page - (uint64_t)((uint8_t *)p - f->base);
    }
}

void hv_ramfile_gpu_map(const void *mapping, void *const *src, size_t n) {
    GpuMap *g = src ? malloc(sizeof *g + n * sizeof(void *)) : NULL;
    if (!g) {
        if (n && !atomic_exchange(&gpu_untracked, 1))
            logf_("a GPU mapping's pages couldn't be listed: every save of this run is whole");
        return;
    }
    pthread_mutex_lock(&open_lock);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        uint64_t page;
        if (!src[i] || !find_page(src[i], &page)) continue;
        g->src[k++] = src[i];
        pin_range(src[i], 1);
    }
    g->key = mapping;
    g->n = k;
    g->next = NULL;
    /* Last in its bucket: an unmap takes the oldest of a key. A key (an address) may be reused by a new mapping
       before the old one's unmap comes; that unmap is then the old one's. */
    GpuMap **at = &gpu_maps[gpu_bucket(mapping)];
    while (*at) at = &(*at)->next;
    *at = g;
    pthread_mutex_unlock(&open_lock);
}

void hv_ramfile_gpu_unmap(const void *mapping) {
    pthread_mutex_lock(&open_lock);
    GpuMap **at = &gpu_maps[gpu_bucket(mapping)];
    while (*at && (*at)->key != mapping) at = &(*at)->next;
    GpuMap *g = *at;
    if (g) {
        *at = g->next;
        for (size_t i = 0; i < g->n; i++) pin_range(g->src[i], 0);
    }
    pthread_mutex_unlock(&open_lock);
    free(g);
}

/* The oldest mapping of <mapping> holds src now (S3-1, 705: a surface whose run failed lists its pages again without
   it). One step under the lock: the new pages are pinned before the old unpinned, so a page kept goes 1, 2, 1 and is
   never unpinned while mapped. No memory for the new list: the old one stays, its pages pinned for nothing, a save
   taking more than it needs, never less. */
void hv_ramfile_gpu_relist(const void *mapping, void *const *src, size_t n) {
    if (!src) return; /* nothing to list from: the old list stays, as with no memory */
    GpuMap *g = malloc(sizeof *g + n * sizeof(void *));
    if (!g) return;
    pthread_mutex_lock(&open_lock);
    GpuMap **at = &gpu_maps[gpu_bucket(mapping)];
    while (*at && (*at)->key != mapping) at = &(*at)->next;
    GpuMap *old = *at;
    if (old) {
        size_t k = 0;
        for (size_t i = 0; i < n; i++) {
            uint64_t page;
            if (!src[i] || !find_page(src[i], &page)) continue;
            g->src[k++] = src[i];
            pin_range(src[i], 1);
        }
        for (size_t i = 0; i < old->n; i++) pin_range(old->src[i], 0);
        g->key = mapping;
        g->n = k;
        g->next = old->next; /* in the old one's place: its age among the key's mappings kept */
        *at = g;
    }
    pthread_mutex_unlock(&open_lock);
    free(old ? old : g);
}

/* A save made it: pages no GPU mapping holds now stop being taken whatever, each marked dirty (so the files get
   what the save held of it, should a mapping's end have been missed). Under the Mac stopped. */
static void gpu_saved(HvRamFile *f) {
    pthread_mutex_lock(&open_lock);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    for (uint64_t i = 0; i < f->npages; i++)
        if (atomic_load_explicit(&f->seen[i], memory_order_relaxed) && !atomic_load_explicit(&f->pins[i], memory_order_relaxed)) {
            mark(f, i);
            atomic_store_explicit(&f->seen[i], 0, memory_order_relaxed);
        }
    pthread_mutex_unlock(&open_lock);
}

int hv_ramfile_copyable(void) { return !atomic_load(&gpu_untracked); }

uint64_t hv_ramfile_gpu_pages(HvRamFile *f) {
    uint64_t n = 0;
    for (uint64_t i = 0; i < f->npages; i++)
        n += atomic_load_explicit(&f->pins[i], memory_order_relaxed) != 0 || atomic_load_explicit(&f->seen[i], memory_order_relaxed);
    return n;
}

/* ---- Finding and writing what changed ---- */

/* Pages the files may not hold */
static uint64_t count_dirty(const HvRamFile *f) {
    uint64_t n = 0;
    for (uint64_t i = 0; i < f->npages; i += 8) {
        uint64_t w;
        memcpy(&w, f->dirty + i, 8); /* a racy read: a count, for the policy and the log */
        if (!w) continue;
        for (uint64_t k = i; k < i + 8 && k < f->npages; k++) n += f->dirty[k] != HV_DIRTY_CLEAN;
    }
    return n;
}

/* A writer's own room: pages are copied out of the memory before they're written (never written from the memory
   itself: storage reading guest pages would hold them while the Mac wants them). */

static int work_init(HvRamFile *f, Work *w) {
    uint64_t per = SLICE / f->page;
    w->buf = mmap(NULL, SLICE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    w->pages = malloc(per * sizeof *w->pages);
    w->cleared = malloc(per * sizeof *w->cleared);
    w->zero = malloc(per);
    if (w->buf != MAP_FAILED && w->pages && w->cleared && w->zero) return 0;
    if (w->buf != MAP_FAILED) munmap(w->buf, SLICE);
    free(w->pages), free(w->cleared), free(w->zero);
    memset(w, 0, sizeof *w);
    return -1;
}

static void work_free(Work *w) {
    if (w->buf) munmap(w->buf, SLICE);
    free(w->pages), free(w->cleared), free(w->zero);
}

static int all_zero(const uint8_t *p, uint64_t n) {
    const uint64_t *q = (const uint64_t *)(const void *)p;
    uint64_t acc = 0;
    for (uint64_t i = 0; i < n / 8; i += 8) acc |= q[i] | q[i + 1] | q[i + 2] | q[i + 3] | q[i + 4] | q[i + 5] | q[i + 6] | q[i + 7];
    return !acc;
}

static int pwrite_all(int fd, const uint8_t *p, uint64_t len, uint64_t off) {
    while (len) {
        ssize_t w = pwrite(fd, p, len, (off_t)off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        p += w, off += (uint64_t)w, len -= (uint64_t)w;
    }
    return 0;
}

/* Writes the dirty pages among [first, end) (one file's), at most `limit` bytes of them (and stopping at a hold,
   when stop_on_hold). Each batch: the pages' marks taken (a LENT page keeps its mark: its device may still write it),
   then every writable TLB entry to a page marked clean dropped (the revoke hook), then the pages copied out and
   written (all-zero runs as holes). A store after the drop goes through a TLB fill, which marks the page again; a
   device's write marks it after. So a page whose mark is clear holds in the files what it holds in the memory.
   Returns the bytes of pages taken; *err set on a write that failed (its pages marked dirty again). */
static uint64_t write_some(HvRamFile *f, uint64_t first, uint64_t end, uint64_t limit, int stop_on_hold, Work *w, int *err) {
    uint64_t per = SLICE / f->page, done = 0, at = first, seg_pages = SEG / f->page;
    while (at < end && !*err) {
        if (stop_on_hold && atomic_load_explicit(&f->holding, memory_order_relaxed)) break;
        /* One batch: within one file */
        unsigned seg = (unsigned)(at / seg_pages);
        uint64_t seg_first = (uint64_t)seg * seg_pages, stop = seg_first + seg_pages < end ? seg_first + seg_pages : end;
        int fd = f->fd[seg];
        uint32_t n = 0, nc = 0;
        for (; at < stop && n < per && done + (uint64_t)(n + 1) * f->page <= limit; at++) {
            uint8_t v = __atomic_load_n(&f->dirty[at], __ATOMIC_RELAXED);
            while (v != HV_DIRTY_CLEAN) {
                /* LENT: copied, but left as it is (its device may write it any time, until hv_dma_written) */
                if (v == HV_DIRTY_LENT) {
                    w->pages[n++] = (uint32_t)at;
                    break;
                }
                if (__atomic_compare_exchange_n(&f->dirty[at], &v, HV_DIRTY_CLEAN, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {
                    w->pages[n++] = (uint32_t)at;
                    w->cleared[nc++] = (uint32_t)at;
                    break;
                }
            }
        }
        if (!n) {
            if (at < stop) break; /* the limit */
            continue;             /* nothing dirty in this file: the next */
        }
        /* The marks clear before the hints are read (hv_ram_revoke_writes): a TLB fill sets its hint before it reads
           the mark, so one of the two sees the other */
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (nc && f->revoke) f->revoke(f->revoke_ctx, f, w->cleared, nc);
        for (uint32_t k = 0; k < n; k++) {
            memcpy(w->buf + (uint64_t)k * f->page, f->base + (uint64_t)w->pages[k] * f->page, f->page);
            w->zero[k] = (uint8_t)all_zero(w->buf + (uint64_t)k * f->page, f->page);
        }
        for (uint32_t k = 0; k < n;) {
            uint32_t j = k + 1;
            while (j < n && w->pages[j] == w->pages[j - 1] + 1 && w->zero[j] == w->zero[k]) j++;
            uint64_t off = (w->pages[k] - seg_first) * f->page, len = (uint64_t)(j - k) * f->page;
            int bad;
            if (w->zero[k]) {
                struct fpunchhole ph = {.fp_flags = 0, .reserved = 0, .fp_offset = (off_t)off, .fp_length = (off_t)len};
                bad = fcntl(fd, F_PUNCHHOLE, &ph) != 0 && pwrite_all(fd, w->buf + (uint64_t)k * f->page, len, off) != 0;
            } else {
                bad = pwrite_all(fd, w->buf + (uint64_t)k * f->page, len, off) != 0;
            }
            if (bad) {
                logf_("writing the memory at %llu MB: %s", (unsigned long long)((w->pages[k] * f->page) >> 20), strerror(errno));
                __atomic_thread_fence(__ATOMIC_SEQ_CST);
                for (uint32_t q = k; q < n; q++) mark(f, w->pages[q]);
                *err = 1;
                break;
            }
            k = j;
        }
        done += (uint64_t)n * f->page;
    }
    return done;
}

static uint64_t free_bytes(const HvRamFile *f) {
    struct statfs s;
    if (fstatfs(f->fd[0], &s) != 0) return UINT64_MAX;
    return (uint64_t)s.f_bavail * s.f_bsize;
}

static uint64_t io_written(void) {
    struct rusage_info_v4 ri;
    if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, (rusage_info_t *)&ri) != 0) return 0;
    return ri.ri_diskio_byteswritten;
}

static uint64_t high_mark(const HvRamFile *f) {
    if (f->high_fixed) return f->high_fixed;
    double h = f->rate_est * LEAVE_S;
    if (h < (double)HIGH_MIN) h = (double)HIGH_MIN;
    if (h > (double)HIGH_MAX) h = (double)HIGH_MAX;
    return (uint64_t)h / f->page * f->page;
}

uint64_t hv_ramfile_writer_pass(HvRamFile *f) {
    uint64_t t = now_ns();
    if (f->refilled_ns) {
        f->tokens += f->rate * (double)(t - f->refilled_ns) / 1e9;
        if (f->tokens > (double)f->burst) f->tokens = (double)f->burst;
    }
    f->refilled_ns = t;
    atomic_fetch_add(&f->scans, 1);
    uint64_t dirty = count_dirty(f) * f->page;
    atomic_store(&f->last_dirty, dirty);
    uint64_t high = high_mark(f), low = high / 2;
    atomic_store(&f->last_high, high);
    uint64_t wrote = 0;
    if (dirty > high) {
        uint64_t room = free_bytes(f);
        if (room < MIN_FREE) {
            if (!f->low_space) logf_("only %llu MB free: not writing ahead", (unsigned long long)(room >> 20));
            f->low_space = 1;
        } else {
            f->low_space = 0;
            uint64_t want = dirty - low, limit = f->tokens > 0 ? (uint64_t)f->tokens : 0;
            int err = 0;
            if (!f->wwork) {
                Work *w = calloc(1, sizeof *w);
                if (w && work_init(f, w) == 0) f->wwork = w;
                else free(w);
            }
            if (f->wwork) {
                wrote = write_some(f, 0, f->npages, want < limit ? want : limit, 1, f->wwork, &err);
                /* The copy room's pages given back between passes (they're rewritten whole each batch) */
                (void)madvise(f->wwork->buf, SLICE, MADV_FREE_REUSABLE);
            }
            f->tokens -= (double)wrote;
            if (wrote) atomic_fetch_add(&f->flushes, 1);
            atomic_fetch_add(&f->written, wrote);
        }
    }
    atomic_fetch_add(&f->busy_ns, now_ns() - t);
    if (!f->logged_ns) f->logged_ns = t, f->logged_io = io_written();
    if (t - f->logged_ns >= LOG_EVERY_NS) {
        uint64_t w = atomic_load(&f->written), io = io_written();
        uint64_t busy = atomic_load(&f->busy_ns);
        logf_("written ahead %llu MB in %.0f min (%llu MB in all), %llu MB changed now (writes above %llu MB), budget %llu MB, "
              "GPU %llu MB; the writer busy %.1f s (%.2f%% of the time); Henge wrote %llu MB to storage in all",
              (unsigned long long)((w - f->logged_written) >> 20), (double)(t - f->logged_ns) / 6e10, (unsigned long long)(w >> 20),
              (unsigned long long)(dirty >> 20), (unsigned long long)(high >> 20), (unsigned long long)(f->tokens / (1 << 20)),
              (unsigned long long)(hv_ramfile_gpu_pages(f) * f->page >> 20), (double)(busy - f->logged_busy) / 1e9,
              100.0 * (double)(busy - f->logged_busy) / (double)(t - f->logged_ns), (unsigned long long)((io - f->logged_io) >> 20));
        f->logged_busy = busy;
        f->logged_ns = t;
        f->logged_written = w;
    }
    return wrote;
}

static void *writer_thread(void *arg) {
    HvRamFile *f = arg;
    pthread_setname_np("hengevm-ram-writer");
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0); /* throttled I/O: the Mac's own disk goes first */
    pthread_mutex_lock(&f->lock);
    for (;;) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t ns = (uint64_t)ts.tv_nsec + (uint64_t)f->period_ms * 1000000ull;
        ts.tv_sec += (time_t)(ns / 1000000000ull);
        ts.tv_nsec = (long)(ns % 1000000000ull);
        pthread_cond_timedwait(&f->cond, &f->lock, &ts);
        while (f->hold) pthread_cond_wait(&f->cond, &f->lock);
        f->busy = 1;
        pthread_mutex_unlock(&f->lock);
        hv_ramfile_writer_pass(f);
        pthread_mutex_lock(&f->lock);
        f->busy = 0;
        pthread_cond_broadcast(&f->cond);
    }
    return NULL;
}

void hv_ramfile_writer_start(HvRamFile *f) {
    pthread_mutex_lock(&f->lock);
    int start = !f->writer_started;
    f->writer_started = 1;
    pthread_mutex_unlock(&f->lock);
    if (!start) return;
    pthread_t t;
    if (pthread_create(&t, NULL, writer_thread, f) == 0) pthread_detach(t);
    else logf_("no writer: %s", strerror(errno));
}

void hv_ramfile_writer_hold(HvRamFile *f, int hold) {
    pthread_mutex_lock(&f->lock);
    f->hold += hold ? 1 : -1;
    if (f->hold < 0) f->hold = 0;
    atomic_store(&f->holding, f->hold > 0);
    if (f->hold)
        while (f->busy) pthread_cond_wait(&f->cond, &f->lock);
    pthread_cond_broadcast(&f->cond);
    pthread_mutex_unlock(&f->lock);
}

void hv_ramfile_set_policy(HvRamFile *f, uint64_t high_bytes, double bytes_per_s, uint64_t burst_bytes, unsigned period_ms) {
    pthread_mutex_lock(&f->lock);
    f->high_fixed = high_bytes;
    f->rate = bytes_per_s;
    f->burst = burst_bytes;
    f->tokens = (double)burst_bytes;
    f->refilled_ns = 0;
    f->period_ms = period_ms ? period_ms : 3000;
    pthread_mutex_unlock(&f->lock);
}

void hv_ramfile_check_all(HvRamFile *f, int all) { f->check_all = all; }
void hv_ramfile_test_spoil(HvRamFile *f, int64_t page) { f->spoil = page; }

/* The copy's page made wrong (hv_ramfile_test_spoil). */
static void spoil_copy(HvRamFile *f, const char *dir) {
    if (f->spoil < 0 || (uint64_t)f->spoil >= f->npages) return;
    uint64_t off = (uint64_t)f->spoil * f->page;
    char p[1110];
    snprintf(p, sizeof p, "%s/%04u", dir, (unsigned)(off / SEG));
    int fd = open(p, O_RDWR | O_CLOEXEC);
    uint8_t *junk = malloc(f->page);
    if (fd >= 0 && junk) {
        for (uint64_t i = 0; i < f->page; i++) junk[i] = f->base[off + i] ^ 0xa5;
        (void)!pwrite(fd, junk, f->page, (off_t)(off % SEG));
        fsync(fd);
    }
    if (fd >= 0) close(fd);
    free(junk);
}

void hv_ramfile_stats(HvRamFile *f, HvRamFileStats *s) {
    s->written = atomic_load(&f->written);
    s->flushes = atomic_load(&f->flushes);
    s->scans = atomic_load(&f->scans);
    s->last_dirty = atomic_load(&f->last_dirty);
    s->high = high_mark(f);
    s->low = s->high / 2;
    s->rate = (uint64_t)f->rate_est;
}

/* ---- Writing it all ---- */

typedef struct SyncJob {
    HvRamFile *f;
    int qos; /* 0: user-initiated (a save, the Mac stopped) */
    const uint64_t *cancel;
    uint64_t ticket, nslices;
    _Atomic uint64_t next, written;
    _Atomic int failed, cancelled;
} SyncJob;

static void *sync_worker(void *arg) {
    SyncJob *j = arg;
    HvRamFile *f = j->f;
    pthread_set_qos_class_self_np(j->qos ? (qos_class_t)j->qos : QOS_CLASS_USER_INITIATED, 0);
    uint64_t per = SAVE_SLICE / f->page;
    Work w;
    if (work_init(f, &w) != 0) {
        atomic_store(&j->failed, 1);
        return NULL;
    }
    for (;;) {
        if (atomic_load(&j->failed) || atomic_load(&j->cancelled)) break;
        if (j->cancel && __atomic_load_n(j->cancel, __ATOMIC_ACQUIRE) != j->ticket) {
            atomic_store(&j->cancelled, 1);
            break;
        }
        uint64_t s = atomic_fetch_add(&j->next, 1);
        if (s >= j->nslices) break;
        uint64_t first = s * per, n = f->npages - first < per ? f->npages - first : per;
        int err = 0;
        atomic_fetch_add(&j->written, write_some(f, first, first + n, UINT64_MAX, 0, &w, &err));
        if (err) atomic_store(&j->failed, 1);
    }
    work_free(&w);
    return NULL;
}

/* The workers of one job: page ranges apart. The writer held (two writes of one page racing could leave the older
   one in the file, its mark clear). */
static void run_job(SyncJob *j, int threads, int *started) {
    if (threads < 1) threads = 1;
    if (threads > 8) threads = 8;
    pthread_t th[8];
    int n = 0;
    qos_class_t was = qos_class_self();
    for (int i = 1; i < threads; i++)
        if (pthread_create(&th[n], NULL, sync_worker, j) == 0) n++;
    sync_worker(j);
    for (int i = 0; i < n; i++) pthread_join(th[i], NULL);
    pthread_set_qos_class_self_np(was, 0);
    if (started) *started = n;
}

static int sync_held(HvRamFile *f, int threads, const uint64_t *cancel, uint64_t ticket);

int hv_ramfile_sync(HvRamFile *f, int threads, const uint64_t *cancel, uint64_t ticket) {
    hv_ramfile_writer_hold(f, 1);
    int rc = sync_held(f, threads, cancel, ticket);
    hv_ramfile_writer_hold(f, 0);
    return rc;
}

static int sync_held(HvRamFile *f, int threads, const uint64_t *cancel, uint64_t ticket) {
    uint64_t t = now_ns();
    uint64_t per = SAVE_SLICE / f->page;
    SyncJob j = {.f = f, .cancel = cancel, .ticket = ticket, .nslices = (f->npages + per - 1) / per};
    int started = 0;
    run_job(&j, threads, &started);
    if (atomic_load(&j.cancelled)) return -3;
    if (atomic_load(&j.failed)) return -1;
    uint64_t t1 = now_ns(), wrote = atomic_load(&j.written);
    /* What a save writes, at full speed, sets how much the writer lets build up. */
    if (wrote >= (64ull << 20) && t1 > t) f->rate_est = 0.5 * f->rate_est + 0.5 * (double)wrote * 1e9 / (double)(t1 - t);
    for (unsigned i = 0; i < f->nseg; i++)
        if (fsync(f->fd[i]) != 0) {
            logf_("can't flush %s/%04u: %s", f->dir, i, strerror(errno));
            return -1;
        }
    logf_("written: %llu MB on %d threads in %.0f ms, flushed in %.0f ms", (unsigned long long)(wrote >> 20), started + 1,
          (double)(t1 - t) / 1e6, (double)(now_ns() - t1) / 1e6);
    return 0;
}

int hv_ramfile_modified(HvRamFile *f, uint32_t **pages, uint64_t *n) {
    *pages = NULL;
    *n = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint64_t count = count_dirty(f);
    uint32_t *out = malloc((count ? count : 1) * sizeof *out);
    if (!out) return -1;
    uint64_t k = 0;
    for (uint64_t i = 0; i < f->npages && k < count; i++)
        if (__atomic_load_n(&f->dirty[i], __ATOMIC_RELAXED) != HV_DIRTY_CLEAN) out[k++] = (uint32_t)i;
    *pages = out;
    *n = k;
    return 0;
}

uint64_t hv_ramfile_save_need(HvRamFile *f) {
    /* The dirty pages are written into the files (new blocks: a copy may hold the old ones); with the Mac
       stopped, what is still dirty after that is only what the GPU holds, saved beside them. */
    return count_dirty(f) * f->page + hv_ramfile_gpu_pages(f) * f->page;
}

int hv_ramfile_put(HvRamFile *f, uint64_t off, const void *src, size_t len) {
    if (off > f->size || len > f->size - off) return -1;
    memcpy(f->base + off, src, len);
    hv_ramfile_mark_dirty(f, off, len);
    return 0;
}

/* ---- Saved copies ---- */

void hv_ramfile_save_free(HvRamFileSave *s) {
    if (!s) return;
    free(s->pages);
    free(s->verify);
    free(s->slot);
    s->slot = NULL;
    s->pages = NULL;
    s->npages = 0;
    s->verify = NULL;
    s->nverify = 0;
    s->copy = NULL;
}

/* What the save holds itself: modified pages and pages under a GPU mapping. The mappings are read first: a page
   unmapped after is in the set already, one unmapped before was marked modified before it stopped counting. */
static int overlay_pages(HvRamFile *f, uint32_t **pages, uint64_t *n, uint64_t *gpu_only) {
    *pages = NULL;
    *n = *gpu_only = 0;
    uint8_t *pinned = malloc(f->npages);
    if (!pinned) return -1;
    for (uint64_t i = 0; i < f->npages; i++)
        pinned[i] = atomic_load_explicit(&f->pins[i], memory_order_acquire) != 0 || atomic_load_explicit(&f->seen[i], memory_order_relaxed);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint64_t count = 0;
    for (uint64_t i = 0; i < f->npages; i++) count += pinned[i] || f->dirty[i] != HV_DIRTY_CLEAN;
    uint32_t *out = malloc((count ? count : 1) * sizeof *out);
    if (!out) {
        free(pinned);
        return -1;
    }
    uint64_t k = 0;
    for (uint64_t i = 0; i < f->npages && k < count; i++) {
        int dirty = __atomic_load_n(&f->dirty[i], __ATOMIC_RELAXED) != HV_DIRTY_CLEAN;
        if (pinned[i] || dirty) {
            out[k++] = (uint32_t)i;
            *gpu_only += !dirty;
        }
    }
    free(pinned);
    *pages = out;
    *n = k;
    return 0;
}

static uint32_t id_crc(const SavedId *id) { return crc32c(id, offsetof(SavedId, crc)); }

/* The saved copy's id, checked against what a snapshot says it is. 0, or -1 with why. */
static int read_saved(const char *dir, const uint8_t gen[HV_RAMFILE_GEN], uint64_t size, SavedId *out, char *why, size_t len) {
    char p[1100];
    snprintf(p, sizeof p, "%s/id", dir);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        logf_("%s: %s", p, strerror(errno));
        say(why, len, "Part of the saved memory is missing.");
        return -1;
    }
    SavedId id;
    ssize_t n = pread(fd, &id, sizeof id, 0);
    close(fd);
    if (n != (ssize_t)sizeof id || memcmp(id.magic, "HENGERAM", 8) != 0 || id.version != SAVED_VERSION || id.crc != id_crc(&id) ||
        memcmp(id.gen, gen, HV_RAMFILE_GEN) != 0 || id.size != size || id.seg != SEG || id.nseg != (size + SEG - 1) / SEG) {
        logf_("%s doesn't describe this save (%zd bytes, version %u, %llu MB)", p, n, id.version, (unsigned long long)(id.size >> 20));
        say(why, len, "The saved memory doesn't match this save.");
        return -1;
    }
    *out = id;
    return 0;
}

int hv_ramfile_check_saved(const char *snap, const uint8_t gen[HV_RAMFILE_GEN], uint64_t size, char *why, size_t why_len) {
    char dir[1100];
    saved_path(snap, gen, dir, sizeof dir);
    SavedId id;
    if (read_saved(dir, gen, size, &id, why, why_len) != 0) return -1;
    for (unsigned i = 0; i < id.nseg; i++) {
        char p[1110];
        snprintf(p, sizeof p, "%s/%04u", dir, i);
        uint64_t off = (uint64_t)i * SEG, want = size - off < SEG ? size - off : SEG;
        struct stat st;
        if (stat(p, &st) != 0 || (uint64_t)st.st_size != want) {
            logf_("%s: missing or not %llu bytes", p, (unsigned long long)want);
            say(why, why_len, "Part of the saved memory is missing.");
            return -1;
        }
    }
    return 0;
}

typedef struct LoadJob {
    HvRamFile *f;
    _Atomic unsigned next;
    _Atomic int failed;
    _Atomic uint64_t bytes;
} LoadJob;

/* Reads [off, off + len) of run file k into the memory. */
static int load_range(HvRamFile *f, unsigned k, uint64_t off, uint64_t len) {
    uint8_t *dst = f->base + (uint64_t)k * SEG + off;
    while (len) {
        size_t want = len < (64u << 20) ? (size_t)len : (64u << 20);
        ssize_t r = pread(f->fd[k], dst, want, (off_t)off);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) {
            logf_("reading %s/%04u at %llu MB: %s", f->dir, k, (unsigned long long)(off >> 20), r < 0 ? strerror(errno) : "ended early");
            return -1;
        }
        dst += r, off += (uint64_t)r, len -= (uint64_t)r;
    }
    return 0;
}

/* Each run file's data read into the memory (its holes are zeros there already). */
static void *load_worker(void *arg) {
    LoadJob *j = arg;
    HvRamFile *f = j->f;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
    for (unsigned k; (k = atomic_fetch_add(&j->next, 1)) < f->nseg;) {
        if (atomic_load(&j->failed)) break;
        uint64_t len = seg_len(f, k), at = 0, got = 0;
        while (at < len) {
            off_t d = lseek(f->fd[k], (off_t)at, SEEK_DATA);
            if (d < 0) {
                if (errno == ENXIO) break; /* holes to the end */
                d = (off_t)at;             /* can't be told: all of it */
            }
            off_t h = lseek(f->fd[k], d, SEEK_HOLE);
            uint64_t e = h < 0 || (uint64_t)h > len ? len : (uint64_t)h;
            if ((uint64_t)d >= e) break;
            if (load_range(f, k, (uint64_t)d, e - (uint64_t)d) != 0) {
                atomic_store(&j->failed, 1);
                break;
            }
            got += e - (uint64_t)d;
            at = e;
        }
        atomic_fetch_add(&j->bytes, got);
    }
    return NULL;
}

static int adopt_held(HvRamFile *f, const char *snap, const uint8_t gen[HV_RAMFILE_GEN], char *why, size_t why_len) {
    uint64_t t = now_ns();
    if (hv_ramfile_check_saved(snap, gen, f->size, why, why_len) != 0) return -1;
    char dir[1100];
    saved_path(snap, gen, dir, sizeof dir);
    f->adopted = 1; /* from here on, it isn't zeros */
    if (fresh(f, 0, f->size) != 0) {
        say(why, why_len, "There wasn't memory for the saved memory.");
        return -1;
    }
    memset(f->dirty, 0, f->npages);
    if (make_set(f, dir) != 0) {
        say(why, why_len, "The saved memory couldn't be opened.");
        return -1;
    }
    uint64_t t1 = now_ns();
    /* Read in whole now, not as the Mac touches it: a page read from storage as the Mac waits for it is what made a
       Mac with Quick Suspend stall */
    LoadJob j = {.f = f};
    pthread_t th[8];
    int started = 0;
    qos_class_t was = qos_class_self();
    for (unsigned i = 1; i < 8 && i < f->nseg; i++)
        if (pthread_create(&th[started], NULL, load_worker, &j) == 0) started++;
    load_worker(&j);
    for (int i = 0; i < started; i++) pthread_join(th[i], NULL);
    pthread_set_qos_class_self_np(was, 0);
    if (atomic_load(&j.failed)) {
        say(why, why_len, "The saved memory couldn't be read.");
        return -1;
    }
    logf_("resuming from %s: cloned in %.0f ms, %llu MB read in %.0f ms on %d threads", dir, (double)(t1 - t) / 1e6,
          (unsigned long long)(atomic_load(&j.bytes) >> 20), (double)(now_ns() - t1) / 1e6, started + 1);
    return 0;
}

/* Held, as a reset: the files and marks are replaced under no pass. */
int hv_ramfile_adopt(HvRamFile *f, const char *snap, const uint8_t gen[HV_RAMFILE_GEN], char *why, size_t why_len) {
    hv_ramfile_writer_hold(f, 1);
    int r = adopt_held(f, snap, gen, why, why_len);
    hv_ramfile_writer_hold(f, 0);
    return r;
}

int hv_ramfile_verify(HvRamFile *f, const HvRamFileCheck *check, uint32_t n) {
    uint64_t t = now_ns();
    for (uint32_t i = 0; i < n; i++) {
        uint64_t off = (uint64_t)check[i].chunk * HV_RAMFILE_CHECK_BYTES;
        if (off >= f->size) {
            logf_("check %u: piece %u is past the memory", i, check[i].chunk);
            return -1;
        }
        uint64_t len = f->size - off < HV_RAMFILE_CHECK_BYTES ? f->size - off : HV_RAMFILE_CHECK_BYTES;
        uint32_t c = crc32c(f->base + off, len);
        if (c != check[i].crc) {
            logf_("the memory at %llu MB isn't what was saved (CRC %08x, saved %08x)", (unsigned long long)(off >> 20), c, check[i].crc);
            return -1;
        }
    }
    logf_("%u pieces of the memory checked against the save in %.0f ms", n, (double)(now_ns() - t) / 1e6);
    return 0;
}

int hv_ramfile_remove_saved(const char *snap, const uint8_t *keep) {
    char parent[1100], keep_path[1100] = "";
    parent_of(snap, parent, sizeof parent);
    const char *base = strrchr(snap, '/') ? strrchr(snap, '/') + 1 : snap;
    char prefix[1100];
    snprintf(prefix, sizeof prefix, "%s.ram-", base);
    if (keep) saved_path(base, keep, keep_path, sizeof keep_path);
    DIR *d = opendir(parent);
    if (!d) return errno == ENOENT ? 0 : -1;
    int rc = 0, removed = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, prefix, strlen(prefix)) != 0 || (keep && !strcmp(e->d_name, keep_path))) continue;
        char p[2200];
        snprintf(p, sizeof p, "%s/%s", parent, e->d_name);
        if (remove_tree(p) != 0) {
            logf_("can't delete %s: %s", p, strerror(errno));
            rc = -1;
        }
        removed++;
    }
    closedir(d);
    if (removed && sync_dir(parent) != 0) rc = -1;
    return rc;
}

int hv_ramfile_remove_copy(const char *snap, const uint8_t gen[HV_RAMFILE_GEN]) {
    char dir[1100], parent[1100];
    saved_path(snap, gen, dir, sizeof dir);
    parent_of(dir, parent, sizeof parent);
    int rc = remove_tree(dir);
    if (rc != 0) logf_("can't delete %s: %s", dir, strerror(errno));
    (void)sync_dir(parent);
    return rc;
}

static int seal_copy(HvRamFile *f, const char *dir, const uint8_t gen[HV_RAMFILE_GEN]);

/* The saved copy: the run folder cloned (blocks shared, nothing copied), its id written, all of it on storage. */
static int make_copy(HvRamFile *f, const char *dir, const uint8_t gen[HV_RAMFILE_GEN]) {
    if (clonefile(f->dir, dir, 0) != 0) {
        logf_("can't clone %s to %s: %s", f->dir, dir, strerror(errno));
        return -1;
    }
    char p[1110];
    for (unsigned i = 0; i < f->nseg; i++) {
        snprintf(p, sizeof p, "%s/%04u", dir, i);
        int fd = open(p, O_RDONLY | O_CLOEXEC);
        int ok = fd >= 0 && fsync(fd) == 0;
        if (fd >= 0) close(fd);
        if (!ok) {
            logf_("can't flush %s: %s", p, strerror(errno));
            return -1;
        }
    }
    /* A file still being swapped in (make_set): the copy would hold the one it was replacing. Never while the Mac
       is stopped for a save, short of a failure there; refused all the same. */
    for (unsigned i = 0; i < f->nseg; i++) {
        snprintf(p, sizeof p, "%s/%04u.new", dir, i);
        if (access(p, F_OK) == 0) {
            logf_("%s is there: the copy isn't used", p);
            return -1;
        }
    }
    return seal_copy(f, dir, gen);
}

/* The copy's id written, and the copy (its files flushed already) on storage. */
static int seal_copy(HvRamFile *f, const char *dir, const uint8_t gen[HV_RAMFILE_GEN]) {
    char p[1110];
    SavedId id;
    memset(&id, 0, sizeof id);
    memcpy(id.magic, "HENGERAM", 8);
    id.version = SAVED_VERSION;
    id.nseg = f->nseg;
    memcpy(id.gen, gen, HV_RAMFILE_GEN);
    id.size = f->size;
    id.seg = SEG;
    id.crc = id_crc(&id);
    snprintf(p, sizeof p, "%s/id", dir);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        logf_("can't write %s: %s", p, strerror(errno));
        return -1;
    }
    set_class(fd);
    int ok = pwrite(fd, &id, sizeof id, 0) == (ssize_t)sizeof id && fsync(fd) == 0;
    char parent[1100];
    parent_of(dir, parent, sizeof parent);
    ok = ok && sync_dir(dir) == 0 && sync_dir(parent) == 0;
    /* Last, the drive's own cache: everything above (and the memory the run files were given) on storage. */
    ok = ok && full_flush(fd) == 0;
    int e = errno;
    close(fd);
    if (!ok) {
        logf_("can't put %s on storage: %s", dir, strerror(e));
        return -1;
    }
    return 0;
}

/* Which pieces a save checks: all (check_all), or a random sample. */
static uint32_t pick_checks(HvRamFile *f, uint32_t *out, uint32_t max) {
    uint32_t chunks = (uint32_t)((f->size + HV_RAMFILE_CHECK_BYTES - 1) / HV_RAMFILE_CHECK_BYTES);
    if (chunks <= max) {
        for (uint32_t i = 0; i < chunks; i++) out[i] = i;
        return chunks;
    }
    uint32_t n = 0;
    while (n < max) {
        uint32_t c = arc4random_uniform(chunks), dup = 0;
        for (uint32_t i = 0; i < n && !dup; i++) dup = out[i] == c;
        if (!dup) out[n++] = c;
    }
    return n;
}

typedef struct CheckJob {
    HvRamFile *f;
    const int *fds;
    const uint8_t *copy;        /* an auto-save: its pages' contents (page k at copy + slot[k] * page), else in the memory */
    const uint32_t *slot;
    const HvRamFileCheck *want; /* a taken save: each piece's CRC then (chunks unused), else the memory now */
    const uint32_t *pages, *chunks;
    uint64_t npages;
    uint32_t n;
    _Atomic uint32_t next;
    _Atomic int failed, unreadable;
    _Atomic uint64_t bad;
} CheckJob;

static void *check_worker(void *arg) {
    CheckJob *j = arg;
    HvRamFile *f = j->f;
    uint8_t *buf = malloc(HV_RAMFILE_CHECK_BYTES);
    if (!buf) {
        atomic_store(&j->failed, 1), atomic_store(&j->unreadable, 1);
        return NULL;
    }
    for (;;) {
        uint32_t c = atomic_fetch_add(&j->next, 1);
        if (c >= j->n || atomic_load(&j->unreadable)) break;
        uint64_t off = (uint64_t)(j->want ? j->want[c].chunk : j->chunks[c]) * HV_RAMFILE_CHECK_BYTES;
        uint64_t len = f->size - off < HV_RAMFILE_CHECK_BYTES ? f->size - off : HV_RAMFILE_CHECK_BYTES;
        unsigned s = (unsigned)(off / SEG);
        if (j->fds[s] < 0 || pread(j->fds[s], buf, len, (off_t)(off - (uint64_t)s * SEG)) != (ssize_t)len) {
            logf_("can't read the saved copy back at %llu MB: %s", (unsigned long long)(off >> 20), strerror(errno));
            atomic_store(&j->failed, 1), atomic_store(&j->unreadable, 1);
            break;
        }
        /* The save's own pages in this piece (pages is ascending) */
        uint64_t p0 = off / f->page, p1 = (off + len) / f->page, lo = 0, hi = j->npages;
        while (lo < hi) {
            uint64_t mid = (lo + hi) / 2;
            if (j->pages[mid] < p0) lo = mid + 1;
            else hi = mid;
        }
        for (uint64_t k = lo; k < j->npages && j->pages[k] < p1; k++)
            memcpy(buf + (uint64_t)j->pages[k] * f->page - off,
                   j->copy ? j->copy + (uint64_t)j->slot[k] * f->page : f->base + (uint64_t)j->pages[k] * f->page, f->page);
        if (j->want) {
            uint32_t got = crc32c(buf, len);
            if (got != j->want[c].crc) {
                logf_("the saved copy, read back, isn't what the memory was at %llu MB (CRC %08x, then %08x)",
                      (unsigned long long)(off >> 20), got, j->want[c].crc);
                atomic_fetch_add(&j->bad, 1);
                atomic_store(&j->failed, 1);
            }
        } else if (memcmp(buf, f->base + off, len) != 0) {
            for (uint64_t p = p0; p < p1; p++)
                if (memcmp(buf + (p - p0) * f->page, f->base + p * f->page, f->page) != 0 && atomic_fetch_add(&j->bad, 1) < 8)
                    logf_("the saved copy differs from the memory at page %llu (%llu MB in)", (unsigned long long)p,
                          (unsigned long long)(p * f->page >> 20));
            atomic_store(&j->failed, 1);
        }
    }
    free(buf);
    return NULL;
}

/* The pieces of the copy in dir, read back from storage, with the save's own pages over them, against the memory, on
   threads threads (the Mac stopped). 0: all equal; -1: a difference, or the copy can't be read. */
static int check_copy(HvRamFile *f, const char *dir, const uint32_t *pages, uint64_t npages, const uint32_t *chunks, uint32_t n,
                      int threads, const uint8_t *copy, const uint32_t *slot, const HvRamFileCheck *want) {
    int *fds = malloc(f->nseg * sizeof *fds);
    if (!fds) return -1;
    for (unsigned i = 0; i < f->nseg; i++) {
        char p[1110];
        snprintf(p, sizeof p, "%s/%04u", dir, i);
        fds[i] = open(p, O_RDONLY | O_CLOEXEC);
        if (fds[i] >= 0) fcntl(fds[i], F_NOCACHE, 1); /* from storage, not the cache */
    }
    CheckJob j = {.f = f, .fds = fds, .copy = copy, .slot = slot, .want = want, .pages = pages, .chunks = chunks, .npages = npages, .n = n};
    if (threads < 1) threads = 1;
    if (threads > 8) threads = 8;
    pthread_t th[8];
    int started = 0;
    for (int i = 1; i < threads; i++)
        if (pthread_create(&th[started], NULL, check_worker, &j) == 0) started++;
    check_worker(&j);
    for (int i = 0; i < started; i++) pthread_join(th[i], NULL);
    uint64_t bad = atomic_load(&j.bad);
    if (bad) logf_("%llu %s differ in %u pieces checked", (unsigned long long)bad, want ? "pieces" : "pages", n);
    for (unsigned i = 0; i < f->nseg; i++)
        if (fds[i] >= 0) close(fds[i]);
    free(fds);
    return atomic_load(&j.failed) ? -1 : 0;
}

int hv_ramfile_commit(HvRamFile *f, const char *snap, int threads, const uint64_t *cancel, uint64_t ticket, HvRamFileSave *out) {
    memset(out, 0, sizeof *out);
    uint64_t t0 = now_ns();
    if (atomic_load(&gpu_untracked)) {
        logf_("a GPU mapping wasn't tracked this run: the memory is saved whole");
        return 1;
    }
    int rc = hv_ramfile_sync(f, threads, cancel, ticket);
    if (rc != 0) return rc == -3 ? -3 : 1;
    uint64_t t1 = now_ns(), gpu_only = 0;
    if (overlay_pages(f, &out->pages, &out->npages, &gpu_only) != 0) {
        logf_("can't tell which pages the files don't hold");
        return 1;
    }
    arc4random_buf(out->gen, HV_RAMFILE_GEN);
    char dir[1100];
    saved_path(snap, out->gen, dir, sizeof dir);
    if (make_copy(f, dir, out->gen) != 0) {
        remove_tree(dir);
        hv_ramfile_save_free(out);
        return 1;
    }
    spoil_copy(f, dir);
    uint64_t t2 = now_ns();
    /* Sampled (or every) piece, the memory as it is against what a resume would make of the copy and the save */
    uint32_t total = (uint32_t)((f->size + HV_RAMFILE_CHECK_BYTES - 1) / HV_RAMFILE_CHECK_BYTES);
    uint32_t want = f->check_all || getenv("HV_RAM_VERIFY") ? total : HV_RAMFILE_CHECKS;
    uint32_t *chunks = malloc((want ? want : 1) * sizeof *chunks);
    uint32_t n = chunks ? pick_checks(f, chunks, want) : 0;
    if (!chunks || check_copy(f, dir, out->pages, out->npages, chunks, n, threads, NULL, NULL, NULL) != 0) {
        logf_("the saved copy doesn't match the memory: not used, the memory is saved whole");
        free(chunks);
        remove_tree(dir);
        hv_ramfile_save_free(out);
        return 1;
    }
    /* What a resume checks: other pieces, picked afresh (more of the memory checked between the two) */
    uint32_t keep = pick_checks(f, chunks, HV_RAMFILE_CHECKS);
    for (uint32_t i = 0; i < keep; i++) {
        uint64_t off = (uint64_t)chunks[i] * HV_RAMFILE_CHECK_BYTES;
        uint64_t len = f->size - off < HV_RAMFILE_CHECK_BYTES ? f->size - off : HV_RAMFILE_CHECK_BYTES;
        out->check[i].chunk = chunks[i];
        out->check[i].crc = crc32c(f->base + off, len);
    }
    out->nchecks = keep;
    free(chunks);
    gpu_saved(f);
    logf_("written ahead since it started: %llu MB in %llu passes, the writer busy %.1f s; Henge wrote %llu MB to storage in all",
          (unsigned long long)(atomic_load(&f->written) >> 20), (unsigned long long)atomic_load(&f->flushes),
          (double)atomic_load(&f->busy_ns) / 1e9,
          (unsigned long long)((io_written() - f->open_io) >> 20));
    logf_("saved copy %s: written in %.0f ms, copied and on storage in %.0f ms, %u pieces checked in %.0f ms; the save holds "
          "%llu MB itself (%llu MB under the GPU only)",
          strrchr(dir, '/') ? strrchr(dir, '/') + 1 : dir, (double)(t1 - t0) / 1e6, (double)(t2 - t1) / 1e6, n,
          (double)(now_ns() - t2) / 1e6, (unsigned long long)(out->npages * f->page >> 20),
          (unsigned long long)(gpu_only * f->page >> 20));
    return 0;
}

/* ---- A save in a stop nobody notices (an auto-save) ---- */

/* Modified pages, counted (the GPU's apart: those always go into a save, and a write doesn't make them clean). */
static int count_modified(HvRamFile *f, uint64_t *modified, uint64_t *taken) {
    uint64_t m = 0, t = 0;
    for (uint64_t i = 0; i < f->npages; i++) {
        int gpu = atomic_load_explicit(&f->pins[i], memory_order_relaxed) != 0 || atomic_load_explicit(&f->seen[i], memory_order_relaxed);
        int mod = __atomic_load_n(&f->dirty[i], __ATOMIC_RELAXED) != HV_DIRTY_CLEAN;
        m += mod && !gpu;
        t += mod || gpu;
    }
    *modified = m;
    if (taken) *taken = t;
    return 0;
}

uint64_t hv_ramfile_catch_up(HvRamFile *f, uint64_t under, int max_passes, uint64_t max_ns, int threads, const uint64_t *cancel,
                             uint64_t ticket) {
    uint64_t t0 = now_ns(), per = SAVE_SLICE / f->page, prev = UINT64_MAX, now = UINT64_MAX, first = 0, wrote = 0;
    hv_ramfile_writer_hold(f, 1); /* one writer of a page at a time */
    int passes = 0;
    for (;;) {
        if (cancel && __atomic_load_n(cancel, __ATOMIC_ACQUIRE) != ticket) {
            now = UINT64_MAX;
            break;
        }
        uint64_t pages;
        if (count_modified(f, &pages, NULL) != 0) {
            now = UINT64_MAX;
            break;
        }
        now = pages * f->page;
        if (!passes) first = now;
        /* Little left, or the Mac changes memory as fast as it is written: the stop takes what is left */
        if (now <= under || now >= prev || passes >= max_passes || now_ns() - t0 >= max_ns) break;
        prev = now;
        /* Throttled, as the writer is: the Mac's own disk goes first */
        SyncJob j = {.f = f, .qos = QOS_CLASS_UTILITY, .cancel = cancel, .ticket = ticket, .nslices = (f->npages + per - 1) / per};
        run_job(&j, threads, NULL);
        wrote += atomic_load(&j.written);
        passes++;
        if (atomic_load(&j.cancelled) || atomic_load(&j.failed)) {
            now = UINT64_MAX;
            break;
        }
    }
    hv_ramfile_writer_hold(f, 0);
    if (now != UINT64_MAX)
        logf_("caught up: %llu MB changed, %llu MB written in %d passes, %llu MB left, in %.0f ms", (unsigned long long)(first >> 20),
              (unsigned long long)(wrote >> 20), passes, (unsigned long long)(now >> 20), (double)(now_ns() - t0) / 1e6);
    return now;
}

int hv_ramfile_prepare(HvRamFile *f, HvRamFileSave *out) {
    memset(out, 0, sizeof *out);
    if (atomic_load(&gpu_untracked)) {
        logf_("a GPU mapping wasn't tracked this run: the memory is saved whole");
        return 1;
    }
    /* A file still being swapped in (make_set): the copy would hold the one it was replacing */
    char p[1110];
    for (unsigned i = 0; i < f->nseg; i++) {
        snprintf(p, sizeof p, "%s/%04u.new", f->dir, i);
        if (access(p, F_OK) == 0) {
            logf_("%s is there: no copy", p);
            return 1;
        }
    }
    if (f->page != 1u << 14) { /* the guard's page (saveguard.h) */
        logf_("pages of %llu bytes: the save guard's are 16 KB", (unsigned long long)f->page);
        return 1;
    }
    uint32_t total = (uint32_t)((f->size + HV_RAMFILE_CHECK_BYTES - 1) / HV_RAMFILE_CHECK_BYTES);
    uint32_t want = f->check_all || getenv("HV_RAM_VERIFY") ? total : HV_RAMFILE_CHECKS;
    uint32_t *chunks = malloc(want * sizeof *chunks);
    out->verify = malloc(want * sizeof *out->verify);
    if (!chunks || !out->verify) {
        free(chunks);
        hv_ramfile_save_free(out);
        return -1;
    }
    out->nverify = pick_checks(f, chunks, want);
    for (uint32_t i = 0; i < out->nverify; i++) out->verify[i].chunk = chunks[i], out->verify[i].crc = 0;
    free(chunks);
    arc4random_buf(out->gen, HV_RAMFILE_GEN);
    return 0;
}

typedef struct MarkJob {
    HvRamFile *f;
    HvRamFileCheck *pieces;
    uint32_t n;
    _Atomic uint32_t next;
} MarkJob;

static void *mark_worker(void *arg) {
    MarkJob *j = arg;
    HvRamFile *f = j->f;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0); /* the Mac is stopped for this */
    for (uint32_t k; (k = atomic_fetch_add(&j->next, 1)) < j->n;) {
        uint64_t off = (uint64_t)j->pieces[k].chunk * HV_RAMFILE_CHECK_BYTES;
        uint64_t len = f->size - off < HV_RAMFILE_CHECK_BYTES ? f->size - off : HV_RAMFILE_CHECK_BYTES;
        j->pieces[k].crc = crc32c(f->base + off, len);
    }
    return NULL;
}

int hv_ramfile_mark(HvRamFile *f, int threads, HvRamFileSave *s) {
    if (!s->verify) return -1;
    MarkJob j = {.f = f, .pieces = s->verify, .n = s->nverify};
    if (threads < 1) threads = 1;
    if (threads > 8) threads = 8;
    pthread_t th[8];
    int started = 0;
    qos_class_t was = qos_class_self();
    for (int i = 1; i < threads && (uint32_t)i < j.n; i++)
        if (pthread_create(&th[started], NULL, mark_worker, &j) == 0) started++;
    mark_worker(&j);
    for (int i = 0; i < started; i++) pthread_join(th[i], NULL);
    pthread_set_qos_class_self_np(was, 0);
    s->nchecks = s->nverify < HV_RAMFILE_CHECKS ? s->nverify : HV_RAMFILE_CHECKS;
    memcpy(s->check, s->verify, s->nchecks * sizeof *s->check);
    return 0;
}

uint64_t hv_ramfile_claim_gpu(HvRamFile *f, HvRamFileClaim claim, void *ctx) {
    uint64_t n = 0;
    pthread_mutex_lock(&open_lock);
    /* The hook first, under the lock a mapping is made under: a page is under a mapping read here, or claimed as it
       is mapped */
    f->claim = claim, f->claim_ctx = ctx;
    for (uint64_t i = 0; i < f->npages;) {
        if (!atomic_load_explicit(&f->pins[i], memory_order_acquire) && !atomic_load_explicit(&f->seen[i], memory_order_relaxed)) {
            i++;
            continue;
        }
        uint64_t e = i + 1;
        while (e < f->npages && (atomic_load_explicit(&f->pins[e], memory_order_acquire) || atomic_load_explicit(&f->seen[e], memory_order_relaxed)))
            e++;
        claim(ctx, i * f->page, (e - i) * f->page);
        n += e - i;
        i = e;
    }
    pthread_mutex_unlock(&open_lock);
    return n;
}

void hv_ramfile_claim_stop(HvRamFile *f) {
    pthread_mutex_lock(&open_lock);
    f->claim = NULL, f->claim_ctx = NULL;
    pthread_mutex_unlock(&open_lock);
}

typedef struct TakeJob {
    HvRamFile *f;
    const char *dir;
    const uint64_t *cancel;
    uint64_t ticket;
    _Atomic unsigned next;
    _Atomic int failed, cancelled;
    _Atomic uint64_t t_flush, t_clone; /* thread time, summed */
} TakeJob;

/* Each run file: on storage (the writes under way finished), then cloned. */
static void *take_worker(void *arg) {
    TakeJob *j = arg;
    HvRamFile *f = j->f;
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0); /* the Mac runs: its own work first */
    for (unsigned k; (k = atomic_fetch_add(&j->next, 1)) < f->nseg;) {
        if (atomic_load(&j->failed) || atomic_load(&j->cancelled)) break;
        if (j->cancel && __atomic_load_n(j->cancel, __ATOMIC_ACQUIRE) != j->ticket) {
            atomic_store(&j->cancelled, 1);
            break;
        }
        uint64_t t = now_ns();
        if (fsync(f->fd[k]) != 0) {
            logf_("can't flush %s/%04u: %s", f->dir, k, strerror(errno));
            atomic_store(&j->failed, 1);
            break;
        }
        uint64_t t1 = now_ns();
        char src[1100], dst[1110];
        snprintf(src, sizeof src, "%s/%04u", f->dir, k);
        snprintf(dst, sizeof dst, "%s/%04u", j->dir, k);
        if (clonefile(src, dst, 0) != 0) {
            logf_("can't clone %s to %s: %s", src, dst, strerror(errno));
            atomic_store(&j->failed, 1);
            break;
        }
        atomic_fetch_add(&j->t_flush, t1 - t);
        atomic_fetch_add(&j->t_clone, now_ns() - t1);
    }
    return NULL;
}

static int take_held(HvRamFile *f, const char *snap, int threads, const uint64_t *cancel, uint64_t ticket, HvRamFileSave *s,
                     HvRamFileClaim claim, void *ctx, uint64_t t0);

int hv_ramfile_take(HvRamFile *f, const char *snap, int threads, const uint64_t *cancel, uint64_t ticket, HvRamFileSave *s) {
    uint64_t t0 = now_ns();
    pthread_mutex_lock(&open_lock);
    HvRamFileClaim claim = f->claim;
    void *ctx = f->claim_ctx;
    pthread_mutex_unlock(&open_lock);
    if (!claim) {
        logf_("taken with no claim: hv_ramfile_claim_gpu first");
        hv_ramfile_save_free(s);
        return -1;
    }
    /* The writer held to the end: no write of a page under way as its mark is read, or as the files are cloned */
    hv_ramfile_writer_hold(f, 1);
    int rc = take_held(f, snap, threads, cancel, ticket, s, claim, ctx, t0);
    hv_ramfile_writer_hold(f, 0);
    return rc;
}

static int take_held(HvRamFile *f, const char *snap, int threads, const uint64_t *cancel, uint64_t ticket, HvRamFileSave *s,
                     HvRamFileClaim claim, void *ctx, uint64_t t0) {
    /* What the files don't hold: copied before anything changes it. A page dirty at T and written since is T's in
       the files (unchanged since, or it would have been claimed); one written since is claimed already. After this,
       every page not claimed holds in the files what it held at T. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint64_t modified = 0;
    for (uint64_t i = 0; i < f->npages;) {
        if (__atomic_load_n(&f->dirty[i], __ATOMIC_RELAXED) == HV_DIRTY_CLEAN) {
            i++;
            continue;
        }
        uint64_t e = i + 1;
        while (e < f->npages && __atomic_load_n(&f->dirty[e], __ATOMIC_RELAXED) != HV_DIRTY_CLEAN) e++;
        claim(ctx, i * f->page, (e - i) * f->page);
        modified += e - i;
        i = e;
    }
    /* Pages no GPU mapping holds now stop being taken whatever, each marked modified (it changes nothing: they were
       claimed by hv_ramfile_claim_gpu) */
    gpu_saved(f);
    uint64_t t1 = now_ns();

    char dir[1100];
    saved_path(snap, s->gen, dir, sizeof dir);
    if (mkdir(dir, 0700) != 0) {
        logf_("can't make %s: %s", dir, strerror(errno));
        hv_ramfile_save_free(s);
        return 1;
    }
    if (threads < 1) threads = 1;
    if (threads > 8) threads = 8;
    TakeJob j = {.f = f, .dir = dir, .cancel = cancel, .ticket = ticket};
    pthread_t th[8];
    int started = 0;
    qos_class_t was = qos_class_self();
    for (int i = 1; i < threads; i++)
        if (pthread_create(&th[started], NULL, take_worker, &j) == 0) started++;
    take_worker(&j);
    for (int i = 0; i < started; i++) pthread_join(th[i], NULL);
    pthread_set_qos_class_self_np(was, 0);
    int rc = atomic_load(&j.cancelled) ? -3 : atomic_load(&j.failed) ? 1 : 0;
    if (rc) {
        remove_tree(dir);
        hv_ramfile_save_free(s);
        return rc;
    }
    spoil_copy(f, dir);
    logf_("taken with the Mac running in %.0f ms: %llu MB modified claimed in %.1f ms; %u files flushed and cloned on %d threads "
          "(thread time: flushing %.0f ms, cloning %.0f ms)",
          (double)(now_ns() - t0) / 1e6, (unsigned long long)(modified * f->page >> 20), (double)(t1 - t0) / 1e6, f->nseg,
          started + 1, (double)atomic_load(&j.t_flush) / 1e6, (double)atomic_load(&j.t_clone) / 1e6);
    return 0;
}

int hv_ramfile_claimed(HvRamFile *f, const char *snap, HvRamFileSave *s, const uint64_t *slot_page, uint64_t nslots, unsigned region,
                       const uint8_t *buf) {
    uint32_t *at = malloc(f->npages * sizeof *at);
    int rc = at ? 0 : -1;
    uint64_t n = 0;
    if (at) {
        memset(at, 0xff, f->npages * sizeof *at);
        for (uint64_t k = 0; k < nslots && !rc; k++) {
            if (slot_page[k] >> 48 != region) continue;
            uint64_t pg = slot_page[k] & 0xffffffffffffull;
            if (pg >= f->npages || at[pg] != UINT32_MAX || k >= UINT32_MAX) {
                logf_("the save guard's slot %llu is for page %llu, outside the memory or taken twice", (unsigned long long)k,
                      (unsigned long long)pg);
                rc = 1;
                break;
            }
            at[pg] = (uint32_t)k;
            n++;
        }
    }
    free(s->pages), free(s->slot);
    s->pages = rc ? NULL : malloc((n ? n : 1) * sizeof *s->pages);
    s->slot = rc ? NULL : malloc((n ? n : 1) * sizeof *s->slot);
    if (!rc && (!s->pages || !s->slot)) rc = -1;
    if (!rc) {
        uint64_t k = 0;
        for (uint64_t i = 0; i < f->npages; i++)
            if (at[i] != UINT32_MAX) s->pages[k] = (uint32_t)i, s->slot[k] = at[i], k++;
        s->npages = n;
        s->copy = buf;
    }
    free(at);
    if (rc) {
        (void)hv_ramfile_remove_copy(snap, s->gen);
        hv_ramfile_save_free(s);
    }
    return rc;
}

int hv_ramfile_finish(HvRamFile *f, const char *snap, int threads, const uint64_t *cancel, uint64_t ticket, HvRamFileSave *s) {
    uint64_t t0 = now_ns();
    char dir[1100], p[1110];
    saved_path(snap, s->gen, dir, sizeof dir);
    int rc = 0;
    for (unsigned i = 0; i < f->nseg && !rc; i++) {
        if (cancel && __atomic_load_n(cancel, __ATOMIC_ACQUIRE) != ticket) {
            rc = -3;
            break;
        }
        snprintf(p, sizeof p, "%s/%04u", dir, i);
        int fd = open(p, O_RDONLY | O_CLOEXEC);
        int ok = fd >= 0 && fsync(fd) == 0;
        if (fd >= 0) close(fd);
        if (!ok) {
            logf_("can't flush %s: %s", p, strerror(errno));
            rc = 1;
        }
    }
    if (!rc && seal_copy(f, dir, s->gen) != 0) rc = 1;
    uint64_t t1 = now_ns();
    if (!rc && cancel && __atomic_load_n(cancel, __ATOMIC_ACQUIRE) != ticket) rc = -3;
    if (!rc && check_copy(f, dir, s->pages, s->npages, NULL, s->nverify, threads, s->copy, s->slot, s->verify) != 0) {
        logf_("the saved copy doesn't match what the memory was: not used");
        rc = 1;
    }
    if (rc) {
        char parent[1100];
        remove_tree(dir);
        parent_of(dir, parent, sizeof parent);
        (void)sync_dir(parent);
        hv_ramfile_save_free(s);
        return rc;
    }
    logf_("saved copy %s: on storage in %.0f ms, %u pieces read back and checked in %.0f ms; the save holds %llu MB itself",
          strrchr(dir, '/') ? strrchr(dir, '/') + 1 : dir, (double)(t1 - t0) / 1e6, s->nverify, (double)(now_ns() - t1) / 1e6,
          (unsigned long long)(s->npages * f->page >> 20));
    return 0;
}
