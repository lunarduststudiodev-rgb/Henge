#include <errno.h>
#include <fcntl.h>
#include "../core/machine.h"
#include "disk.h"

#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <pthread.h>
#include <sys/clonefile.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>

/* Large transfers go in pieces this size: one pread/pwrite of gigabytes is refused (EINVAL past INT_MAX). */
#define DISK_IO_CHUNK (128u << 20)

int hv_disk_open(HvDisk *d, const char *path, uint64_t offset, int readonly) {
    d->wfd = -1;
    d->guard = NULL;
    d->guard_users = 0;
    d->fd = open(path, readonly ? O_RDONLY : O_RDWR);
    if (d->fd < 0) return -1;
    struct stat st;
    if (fstat(d->fd, &st) != 0 || (uint64_t)st.st_size < offset) {
        close(d->fd);
        d->fd = -1;
        return -1;
    }
    d->offset = offset;
    d->size = (uint64_t)st.st_size - offset;
    d->readonly = readonly;
    /* HV_WRITE_CACHE=0: writes skip the host's cache (F_NOCACHE), so a long run of them cannot pile up
       gigabytes of dirty pages; reads stay cached, and the kernel keeps the two coherent. Off by default:
       on the Mac bench big writes took 2-3x as long that way (2 GB + sync 4-6 s -> 9-17 s) and everyday
       work was no faster. The phone's installer turns it on for the restore, which writes ~15 GB at once. */
    const char *wc = getenv("HV_WRITE_CACHE");
    if (!readonly && wc && !atoi(wc)) {
        d->wfd = open(path, O_RDWR);
        if (d->wfd >= 0 && fcntl(d->wfd, F_NOCACHE, 1) == -1) {
            close(d->wfd);
            d->wfd = -1;
        }
    }
    return 0;
}

void hv_disk_close(HvDisk *d) {
    if (d->fd >= 0) close(d->fd);
    if (d->wfd >= 0) close(d->wfd);
    d->fd = d->wfd = -1;
}

int hv_disk_read(HvDisk *d, uint64_t off, void *buf, uint64_t len) {
    if (off > d->size || len > d->size - off) return -1;
    uint8_t *p = buf;
    while (len) {
        size_t count = len > DISK_IO_CHUNK ? DISK_IO_CHUNK : (size_t)len;
        ssize_t n = pread(d->fd, p, count, (off_t)(d->offset + off));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        off += (uint64_t)n;
        len -= (uint64_t)n;
    }
    return 0;
}

static void guard_claim(HvDisk *d, HvDiskGuard *g, uint64_t off, uint64_t len);

int hv_disk_write(HvDisk *d, uint64_t off, const void *buf, uint64_t len) {
    if (d->readonly || off > d->size || len > d->size - off) return -1;
    /* An auto-save's: the blocks as they were, before they change. No guard (always but during an auto-save's
       clone): nothing to pay, one load. Seeing none, acquire: it was taken away after the log closed (guard_end), so
       no block is logged after this write. Seeing one, counted while it holds it (sequentially consistent with the
       end's store and wait: either this sees no guard after all, or the end waits for it). The arm is in the stop,
       when no write is under way (the stop drains them). */
    if (len && __atomic_load_n(&d->guard, __ATOMIC_ACQUIRE)) {
        __atomic_add_fetch(&d->guard_users, 1, __ATOMIC_SEQ_CST);
        HvDiskGuard *g = __atomic_load_n(&d->guard, __ATOMIC_SEQ_CST);
        if (g) guard_claim(d, g, off, len);
        __atomic_sub_fetch(&d->guard_users, 1, __ATOMIC_RELEASE);
    }
    const uint8_t *p = buf;
    while (len) {
        size_t count = len > DISK_IO_CHUNK ? DISK_IO_CHUNK : (size_t)len;
        ssize_t n = pwrite(d->wfd >= 0 ? d->wfd : d->fd, p, count, (off_t)(d->offset + off));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        off += (uint64_t)n;
        len -= (uint64_t)n;
    }
    return 0;
}

/* HV_FLUSH: 0 fsync; 1 nothing here, what was written is safe in the host's cache as long as the
   host runs; 2 fcntl(F_BARRIERFSYNC). */
int hv_disk_flush(HvDisk *d) {
    if (d->readonly) return 0;
    switch (hv_knob[HV_K_FLUSH]) {
    case 1: return 0;
    case 2: return fcntl(d->fd, F_BARRIERFSYNC) == -1 ? -1 : 0;
    default: return fsync(d->fd);
    }
}

int hv_disk_sync(HvDisk *d) {
    if (d->readonly || d->fd < 0) return 0;
    if (fcntl(d->fd, F_FULLFSYNC) == 0) return 0;
    return fsync(d->fd) == 0 ? 0 : -1; /* a file system without F_FULLFSYNC */
}

/* The directory holding `path` onto storage (its new entry survives a power cut). */
static int sync_parent(const char *path) {
    char dir[1024];
    const char *slash = strrchr(path, '/');
    size_t n = slash ? (size_t)(slash - path) : 1;
    if (n >= sizeof dir) return -1;
    if (slash) memcpy(dir, path, n ? n : 1), dir[n ? n : 1] = 0;
    else strcpy(dir, ".");
    int fd = open(dir, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    int rc = fcntl(fd, F_FULLFSYNC) == 0 || fsync(fd) == 0 ? 0 : -1;
    close(fd);
    return rc;
}

int hv_disk_clone(HvDisk *d, const char *to, uint64_t *id_ino, uint64_t *id_size, int64_t *id_mtime_ns) {
    if (d->fd < 0) return -1;
    /* What the guest wrote is in the file (on the storage, too, so the clone shares blocks that are there) */
    if (hv_disk_sync(d) != 0) return -1;
    if (fclonefileat(d->fd, AT_FDCWD, to, 0) != 0) return -1;
    int fd = open(to, O_RDONLY | O_CLOEXEC);
    struct stat st;
    int ok = fd >= 0 && fstat(fd, &st) == 0 && (fcntl(fd, F_FULLFSYNC) == 0 || fsync(fd) == 0);
    if (fd >= 0) close(fd);
    if (!ok || sync_parent(to) != 0) {
        int e = errno;
        unlink(to);
        errno = e;
        return -1;
    }
    if (id_ino) *id_ino = (uint64_t)st.st_ino;
    if (id_size) *id_size = (uint64_t)st.st_size;
    if (id_mtime_ns) *id_mtime_ns = (int64_t)st.st_mtimespec.tv_sec * 1000000000 + st.st_mtimespec.tv_nsec;
    return 0;
}

int hv_disk_reopen(HvDisk *d, const char *path) {
    HvDisk n;
    if (hv_disk_open(&n, path, d->offset, d->readonly) != 0) return -1;
    if (n.size != d->size) {
        hv_disk_close(&n);
        errno = EINVAL;
        return -1;
    }
    hv_disk_close(d);
    *d = n;
    return 0;
}

static int same_file_time(const struct stat *a, const struct stat *b) {
    return a->st_size == b->st_size && a->st_mtimespec.tv_sec == b->st_mtimespec.tv_sec &&
           a->st_mtimespec.tv_nsec == b->st_mtimespec.tv_nsec;
}

/* `from` renamed to `aside`, never over anything there: if that name is taken, "<name> 2.img", "<name> 3.img" and on
   (the number before the extension). 0 when kept; -1 (from left where it is) when no name could be had. */
static int keep_aside(const char *from, const char *aside) {
    const char *slash = strrchr(aside, '/'), *dot = strrchr(aside, '.');
    if (dot && slash && dot < slash) dot = NULL;
    size_t stem = dot ? (size_t)(dot - aside) : strlen(aside);
    char name[1100];
    for (int n = 1; n < 1000; n++) {
        if (n == 1) {
            if (snprintf(name, sizeof name, "%s", aside) >= (int)sizeof name) return -1;
        } else if (snprintf(name, sizeof name, "%.*s %d%s", (int)stem, aside, n, dot ? dot : "") >= (int)sizeof name) {
            return -1;
        }
        if (renamex_np(from, name, RENAME_EXCL) == 0) {
            fprintf(stderr, "hengevm: disk: the disk that was in place is kept as %s\n", name);
            return 0;
        }
        if (errno != EEXIST) break;
    }
    fprintf(stderr, "hengevm: disk: %s couldn't be kept aside (%s): left where it is\n", from, strerror(errno));
    return -1;
}

int hv_disk_place(HvDisk *d, const char *to, const char *aside) {
    char tmp[1100];
    if (snprintf(tmp, sizeof tmp, "%s.placing", to) >= (int)sizeof tmp) return -1;
    struct stat open_st, old_st, left_st;
    if (fstat(d->fd, &open_st) != 0) return -1;
    /* One left by a start that ended partway: before its swap, a copy of a clone (this one: the same size and time);
       after it, the disk that was at `to` then. That one is kept aside like any other; if it can't be, nothing
       changes now. (No aside: the auxiliary file, whose newer state is never kept.) */
    if (lstat(tmp, &left_st) == 0) {
        if (!aside || same_file_time(&left_st, &open_st)) {
            unlink(tmp);
        } else if (keep_aside(tmp, aside) != 0) {
            errno = EEXIST;
            return -1;
        }
    }
    if (hv_disk_clone(d, tmp, NULL, NULL, NULL) != 0) return -1;
    if (lstat(to, &old_st) == 0) {
        /* At once: `to` is always a whole disk, the new one or the old */
        if (renamex_np(tmp, to, RENAME_SWAP) != 0) {
            int e = errno;
            unlink(tmp);
            errno = e;
            return -1;
        }
        /* tmp now holds what was at `to`: kept aside unless it is this very disk as it was (nothing written since the
           clone this is: the same size and modification time). If it can't be kept aside, it stays as tmp (the next
           place keeps it aside then): never deleted. */
        if (!aside || same_file_time(&old_st, &open_st)) unlink(tmp);
        else (void)keep_aside(tmp, aside);
    } else if (rename(tmp, to) != 0) {
        int e = errno;
        unlink(tmp);
        errno = e;
        return -1;
    }
    if (sync_parent(to) != 0 || (aside && sync_parent(aside) != 0)) return -1;
    return hv_disk_reopen(d, to);
}

/* ---- an auto-save's disk guard ---- */

struct HvDiskGuard {
    pthread_mutex_t mu;     /* a block's logging, and the end */
    uint64_t *logged;       /* a bit per block: its T bytes are in the log (or were never at risk: past the end) */
    uint64_t nblocks;
    int fd;                 /* the log: blocks one after another, unlinked */
    uint64_t tail, cap;     /* bytes in the log, and its most */
    uint64_t *block;        /* per entry: which block (cap / HV_DISK_GUARD_BLOCK entries) */
    uint64_t n;             /* entries */
    int ended;              /* past the clone: nothing more to log */
    int void_;              /* full, or an I/O error: the save is void */
};

HvDiskGuard *hv_disk_guard_new(HvDisk *d, const char *dir, uint64_t max_bytes, uint64_t reserve) {
    if (d->fd < 0 || d->readonly) return NULL;
    HvDiskGuard *g = calloc(1, sizeof *g);
    if (!g) return NULL;
    g->nblocks = (d->size + HV_DISK_GUARD_BLOCK - 1) / HV_DISK_GUARD_BLOCK;
    char path[1100];
    snprintf(path, sizeof path, "%s/.disk-guard-XXXXXX", dir);
    g->fd = mkstemp(path);
    if (g->fd < 0) {
        fprintf(stderr, "hengevm: disk guard: can't make its log in %s: %s\n", dir, strerror(errno));
        free(g);
        return NULL;
    }
    unlink(path);
    struct statfs st;
    uint64_t cap = max_bytes, room = UINT64_MAX;
    if (fstatfs(g->fd, &st) == 0) {
        room = (uint64_t)st.f_bavail * st.f_bsize;
        uint64_t usable = room > reserve ? room - reserve : 0;
        if (usable < cap) cap = usable;
    }
    g->cap = cap - cap % HV_DISK_GUARD_BLOCK;
    if (!g->cap) {
        fprintf(stderr, "hengevm: disk guard: the storage is full: %llu MB free, %llu MB of it to be left free, "
                        "so no room for the log\n", (unsigned long long)(room >> 20), (unsigned long long)(reserve >> 20));
        close(g->fd), free(g);
        return NULL;
    }
    g->logged = calloc((g->nblocks + 63) / 64, sizeof *g->logged);
    g->block = malloc(g->cap / HV_DISK_GUARD_BLOCK * sizeof *g->block);
    if (!g->logged || !g->block) {
        fprintf(stderr, "hengevm: disk guard: out of memory for a log of %llu MB\n", (unsigned long long)(g->cap >> 20));
        close(g->fd);
        free(g->logged), free(g->block), free(g);
        return NULL;
    }
    pthread_mutex_init(&g->mu, NULL);
    return g;
}

void hv_disk_guard_arm(HvDisk *d, HvDiskGuard *g) {
    __atomic_store_n(&d->guard, g, __ATOMIC_RELEASE);
}

static int is_logged(const HvDiskGuard *g, uint64_t b) {
    return (__atomic_load_n(&g->logged[b / 64], __ATOMIC_ACQUIRE) >> (b % 64)) & 1;
}

/* Before [off, off + len) is written: each block it touches that isn't in the log yet goes in, as it is now. */
static void guard_claim(HvDisk *d, HvDiskGuard *g, uint64_t off, uint64_t len) {
    uint64_t first = off / HV_DISK_GUARD_BLOCK, last = (off + len - 1) / HV_DISK_GUARD_BLOCK;
    uint8_t *tmp = NULL;
    for (uint64_t b = first; b <= last; b++) {
        if (is_logged(g, b)) continue;
        pthread_mutex_lock(&g->mu);
        /* Under the lock, again: a writer of the same block waits here until its old bytes are in the log */
        if (!is_logged(g, b) && !g->ended && !g->void_) {
            uint64_t at = b * HV_DISK_GUARD_BLOCK, n = d->size - at < HV_DISK_GUARD_BLOCK ? d->size - at : HV_DISK_GUARD_BLOCK;
            if (!tmp) tmp = malloc(HV_DISK_GUARD_BLOCK);
            if (!tmp || g->tail + HV_DISK_GUARD_BLOCK > g->cap) {
                g->void_ = 1;
                fprintf(stderr, "hengevm: disk guard: the log is full (%llu MB): this save is given up\n", (unsigned long long)(g->cap >> 20));
            } else if (hv_disk_read(d, at, tmp, n) != 0 || pwrite(g->fd, tmp, n, (off_t)g->tail) != (ssize_t)n) {
                g->void_ = 1;
                fprintf(stderr, "hengevm: disk guard: a block couldn't be logged: %s\n", strerror(errno));
            } else {
                g->block[g->n++] = b;
                g->tail += HV_DISK_GUARD_BLOCK;
                __atomic_fetch_or(&g->logged[b / 64], 1ull << (b % 64), __ATOMIC_RELEASE);
            }
        }
        pthread_mutex_unlock(&g->mu);
    }
    free(tmp);
}

/* Ends the guard: when it returns no writer holds it (none logs after it, and it can be freed).
   The log closes before the guard is taken away: a writer that sees no guard writes without logging, so a block
   logged after that could hold its bytes, not T's. Closed first, a block is logged only while every writer still
   logs first (the one logging finishes its block, a 16 KB read and write, while the others wait for it). */
static void guard_end(HvDisk *d, HvDiskGuard *g) {
    pthread_mutex_lock(&g->mu);
    g->ended = 1;
    pthread_mutex_unlock(&g->mu);
    __atomic_store_n(&d->guard, NULL, __ATOMIC_SEQ_CST);
    /* Sequentially consistent, as the writers' count and load are: either a writer sees no guard, or this sees it */
    for (unsigned spins = 0; __atomic_load_n(&d->guard_users, __ATOMIC_SEQ_CST); spins++)
        if (spins > 64) usleep(50);
}

int hv_disk_clone_guarded(HvDisk *d, const char *to, uint64_t *id_ino, uint64_t *id_size, int64_t *id_mtime_ns) {
    HvDiskGuard *g = __atomic_load_n(&d->guard, __ATOMIC_ACQUIRE);
    if (!g) return -1;
    /* Every block that differs from T in the clone was logged before it was written: the clone may hold any of
       their later bytes (even a write's half), and the log puts T's back over them. */
    /* What was written before T into the file system first (the clone shares its blocks; the clone's full sync
       below then puts them on the storage itself). Later writes reaching it too don't matter: logged. */
    int rc = fsync(d->fd) == 0 && fclonefileat(d->fd, AT_FDCWD, to, 0) == 0 ? 0 : -1;
    int e = errno;
    guard_end(d, g);
    if (rc != 0) {
        errno = e;
        return -1;
    }
    if (g->void_) {
        unlink(to);
        return 1;
    }
    int fd = open(to, O_RDWR | O_CLOEXEC);
    uint8_t *tmp = malloc(HV_DISK_GUARD_BLOCK);
    int ok = fd >= 0 && tmp;
    for (uint64_t k = 0; ok && k < g->n; k++) {
        uint64_t at = g->block[k] * HV_DISK_GUARD_BLOCK, n = d->size - at < HV_DISK_GUARD_BLOCK ? d->size - at : HV_DISK_GUARD_BLOCK;
        ok = pread(g->fd, tmp, n, (off_t)(k * HV_DISK_GUARD_BLOCK)) == (ssize_t)n &&
             pwrite(fd, tmp, n, (off_t)(d->offset + at)) == (ssize_t)n;
    }
    free(tmp);
    struct stat st;
    ok = ok && (fcntl(fd, F_FULLFSYNC) == 0 || fsync(fd) == 0) && fstat(fd, &st) == 0;
    e = errno;
    if (fd >= 0) close(fd);
    if (!ok || sync_parent(to) != 0) {
        if (ok) e = errno;
        unlink(to);
        errno = e;
        return -1;
    }
    if (id_ino) *id_ino = (uint64_t)st.st_ino;
    if (id_size) *id_size = (uint64_t)st.st_size;
    if (id_mtime_ns) *id_mtime_ns = (int64_t)st.st_mtimespec.tv_sec * 1000000000 + st.st_mtimespec.tv_nsec;
    return 0;
}

void hv_disk_guard_free(HvDisk *d, HvDiskGuard *g) {
    if (!g) return;
    if (d && __atomic_load_n(&d->guard, __ATOMIC_ACQUIRE) == g) guard_end(d, g);
    close(g->fd);
    pthread_mutex_destroy(&g->mu);
    free(g->logged), free(g->block), free(g);
}

uint64_t hv_disk_guard_logged(const HvDiskGuard *g) { return g ? g->n : 0; }

int hv_disk_guard_test_logged(const HvDiskGuard *g, uint64_t block) { return g && block < g->nblocks ? is_logged(g, block) : -1; }

int hv_disk_guard_test_entry(const HvDiskGuard *g, uint64_t block, void *to) {
    for (uint64_t k = 0; g && k < g->n; k++)
        if (g->block[k] == block) return pread(g->fd, to, HV_DISK_GUARD_BLOCK, (off_t)(k * HV_DISK_GUARD_BLOCK)) > 0 ? (int)k : -1;
    return -1;
}
