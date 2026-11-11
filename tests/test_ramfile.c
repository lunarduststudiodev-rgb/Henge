/* Save memory ahead (core/ramfile.h): the Mac's memory kept in files by a writer, and saved copies of them.

   Checks that the memory is ordinary memory, not the files mapped (a store reaches the files only when the writer
   writes it), that what is marked dirty is listed, that a page the writer takes is clean again unless a device
   still has it (lent) and that the engine is asked to take back writes to exactly the pages made clean, that an
   all-zero page and a page given back (the balloon) become holes, that a resume reads the saved copy in whole, that
   a save leaves a copy on
   storage that is the memory (with the pages it lists) and that nothing after changes it (the Mac running on, a
   resume of it and the resumed Mac running), that pages under a GPU mapping are always listed and marked modified
   once unmapped, that a copy that doesn't match the memory is caught at the save and at the resume, that copies
   are found and deleted by name, and that the background writer keeps to its budget and stops for a hold.

   Usage: test_ramfile [folder for the files] */
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <libproc.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ramfile.h"
#include "scratch_dir.h"

static int failures;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            failures++;                                                                                                \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                       \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fprintf(stderr, "\n");                                                                                     \
        }                                                                                                              \
    } while (0)

#define SEG (128ull << 20)
#define SIZE (SEG + (64ull << 20)) /* two files, the second shorter */

/* The Mac writing its memory: stores, then the pages marked (as a device does: hv_dma_written). */
static void wr(HvRamFile *f, uint64_t off, int v, uint64_t len) {
    memset(hv_ramfile_base(f) + off, v, len);
    hv_ramfile_mark_dirty(f, off, len);
}
static void poke(HvRamFile *f, uint64_t off, uint8_t v) { wr(f, off, v, 1); }

/* What run file i holds at off (read from it, not the memory). */
static int file_byte(const char *run, unsigned i, uint64_t off) {
    char p[1100];
    snprintf(p, sizeof p, "%s/%04u", run, i);
    int fd = open(p, O_RDONLY);
    uint8_t b = 0;
    ssize_t n = fd >= 0 ? pread(fd, &b, 1, (off_t)off) : -1;
    if (fd >= 0) close(fd);
    return n == 1 ? b : -1;
}

/* Whether [off, off + len) of run file i is a hole. */
static int file_hole(const char *run, unsigned i, uint64_t off, uint64_t len) {
    char p[1100];
    snprintf(p, sizeof p, "%s/%04u", run, i);
    int fd = open(p, O_RDONLY);
    if (fd < 0) return 0;
    off_t d = lseek(fd, (off_t)off, SEEK_DATA);
    close(fd);
    return (d < 0 && errno == ENXIO) || (d >= 0 && (uint64_t)d >= off + len);
}

/* The revoke hook (the engine's hv_ram_revoke_writes): the pages it was asked for. */
static uint32_t revoked[1 << 16];
static _Atomic uint32_t nrevoked;
static void on_revoke(void *ctx, HvRamFile *f, const uint32_t *pages, uint32_t n) {
    (void)ctx, (void)f;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t k = atomic_fetch_add(&nrevoked, 1);
        if (k < sizeof revoked / sizeof *revoked) revoked[k] = pages[i];
    }
}

static uint64_t modified_count(HvRamFile *f, uint32_t **out) {
    uint32_t *p;
    uint64_t n;
    if (hv_ramfile_modified(f, &p, &n) != 0) return UINT64_MAX;
    if (out) *out = p;
    else free(p);
    return n;
}

static void copy_path(const char *snap, const uint8_t *gen, char *out, size_t len) {
    char hex[33];
    for (int i = 0; i < 16; i++) snprintf(hex + 2 * i, 3, "%02x", gen[i]);
    snprintf(out, len, "%s.ram-%s", snap, hex);
}

/* The saved copy read from storage (not the cache), into buf (SIZE bytes). */
static int read_copy(const char *dir, uint8_t *buf) {
    for (unsigned i = 0; i * SEG < SIZE; i++) {
        char p[1200];
        snprintf(p, sizeof p, "%s/%04u", dir, i);
        int fd = open(p, O_RDONLY);
        if (fd < 0) return 0;
        fcntl(fd, F_NOCACHE, 1);
        uint64_t len = SIZE - i * SEG < SEG ? SIZE - i * SEG : SEG;
        ssize_t n = pread(fd, buf + i * SEG, len, 0);
        close(fd);
        if (n != (ssize_t)len) return 0;
    }
    return 1;
}

static int exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static int copies_of(const char *folder, const char *base) {
    DIR *d = opendir(folder);
    int n = 0;
    struct dirent *e;
    char prefix[256];
    snprintf(prefix, sizeof prefix, "%s.ram-", base);
    while (d && (e = readdir(d))) n += strncmp(e->d_name, prefix, strlen(prefix)) == 0;
    if (d) closedir(d);
    return n;
}

/* Stores into random pages until told to stop. */
typedef struct Racer {
    HvRamFile *f;
    uint8_t *m;
    uint64_t pg;
    _Atomic int stop;
    uint64_t stores;
} Racer;

static void *race(void *arg) {
    Racer *r = arg;
    uint64_t x = 0x2545f4914f6cdd1dull;
    while (!atomic_load(&r->stop)) {
        x ^= x << 13, x ^= x >> 7, x ^= x << 17;
        ((volatile uint64_t *)r->m)[(x % SIZE) / 8] = x;
        hv_ramfile_mark_dirty(r->f, (x % SIZE) & ~7ull, 8);
        r->stores++;
    }
    return NULL;
}

/* A save guard as core/saveguard.h keeps one (without a Mac): pages guarded at T; a write claims (copies) the page
   first. Region 0 is the memory. */
typedef struct TGuard {
    HvRamFile *f;
    uint8_t *m, *state, *buf;
    uint64_t pg, npages, cap, used, *slot_page;
    pthread_mutex_t mu;
    int full;
} TGuard;

static void tclaim(void *ctx, uint64_t off, uint64_t len) {
    TGuard *g = ctx;
    pthread_mutex_lock(&g->mu);
    for (uint64_t p = off / g->pg; p < (off + len + g->pg - 1) / g->pg && p < g->npages; p++) {
        if (!g->state[p]) continue;
        if (g->used < g->cap) {
            memcpy(g->buf + g->used * g->pg, g->m + p * g->pg, g->pg);
            g->slot_page[g->used++] = p; /* region 0 */
        } else {
            g->full = 1;
        }
        g->state[p] = 0;
    }
    pthread_mutex_unlock(&g->mu);
}

static void tarm(TGuard *g) {
    memset(g->state, 1, g->npages);
    g->used = 0;
    g->full = 0;
}

/* A guarded write (claims first), as every writer to the Mac's memory makes one while armed. */
static void twrite(TGuard *g, uint64_t page, int v, uint64_t pages) {
    tclaim(g, page * g->pg, pages * g->pg);
    wr(g->f, page * g->pg, v, pages * g->pg);
}

/* Guarded stores into random pages until told to stop (the Mac running through a save). */
typedef struct GRacer {
    TGuard *g;
    _Atomic int stop;
    _Atomic uint64_t stores;
} GRacer;

static void *grace(void *arg) {
    GRacer *r = arg;
    uint64_t x = 0x9e3779b97f4a7c15ull;
    while (!atomic_load(&r->stop) && atomic_load(&r->stores) < 4000) { /* some pages, not all: the copy holds the rest */
        x ^= x << 13, x ^= x >> 7, x ^= x << 17;
        uint64_t at = (x % SIZE) & ~7ull;
        tclaim(r->g, at, 8);
        ((volatile uint64_t *)r->g->m)[at / 8] = x;
        hv_ramfile_mark_dirty(r->g->f, at, 8);
        atomic_fetch_add(&r->stores, 1);
        if ((x & 255) == 0) usleep(20);
    }
    return NULL;
}

/* A page given back (the balloon) while a GPU mapping pins it: the claim hook runs inside the pin, before the page
   counts as pinned, and starts the give-back there. */
typedef struct PinRace {
    HvRamFile *f;
    uint64_t off, len;
    pthread_t th;
    _Atomic int started, done;
} PinRace;

static void *pin_race_discard(void *arg) {
    PinRace *r = arg;
    hv_ramfile_discard(r->f, r->off, r->len);
    atomic_store(&r->done, 1);
    return NULL;
}

static void pin_race_claim(void *ctx, uint64_t off, uint64_t len) {
    PinRace *r = ctx;
    if (off != r->off || atomic_exchange(&r->started, 1)) return;
    (void)len;
    pthread_create(&r->th, NULL, pin_race_discard, r);
    for (int i = 0; i < 200 && !atomic_load(&r->done); i++) usleep(1000); /* time for it to run, if it can */
}

int main(int argc, char **argv) {
    const char *top = scratch_dir(argc, argv, "test_ramfile");
    char run[1024], run2[1024], snap[1024], src[1100], dir[1200], dir2[1200];
    snprintf(run, sizeof run, "%s/run", top);
    snprintf(run2, sizeof run2, "%s/run2", top);
    snprintf(snap, sizeof snap, "%s/state.snap", top);
    snprintf(src, sizeof src, "%s/source.bin", top);
    hv_ramfile_remove_saved(snap, NULL);

    /* An old run folder's leftovers go. */
    mkdir(run, 0700);
    char stale[1100];
    snprintf(stale, sizeof stale, "%s/0000", run);
    int sfd0 = open(stale, O_RDWR | O_CREAT | O_TRUNC, 0600);
    CHECK(sfd0 >= 0 && pwrite(sfd0, "old", 3, 0) == 3, "stale file");
    close(sfd0);

    /* A fresh one: zeros, sparse, nothing modified, two files. */
    HvRamFile *f = hv_ramfile_open(run, SIZE);
    CHECK(f, "open");
    if (!f) return 1;
    uint8_t *m = hv_ramfile_base(f);
    uint64_t pg = hv_ramfile_page(f);
    CHECK(m[0] == 0 && m[1] == 0 && m[SIZE - 1] == 0, "zeros (the old file's bytes gone)");
    CHECK(hv_ramfile_unallocated(f) > SIZE / 2, "sparse: %llu unallocated", (unsigned long long)hv_ramfile_unallocated(f));
    CHECK(modified_count(f, NULL) == 0, "nothing modified at first");
    snprintf(stale, sizeof stale, "%s/0001", run);
    CHECK(exists(stale), "two files");
    char region[PROC_PIDPATHINFO_MAXSIZE] = "";
    int rl = proc_regionfilename(getpid(), (uint64_t)(uintptr_t)(m + SIZE / 2), region, sizeof region);
    CHECK(rl <= 0, "the memory isn't a file mapping (it is %.*s)", rl > 0 ? rl : 0, region);

    /* Stores, including across the two files: exactly those pages modified. */
    uint64_t edge = SEG / pg;
    poke(f, 0, 1);
    poke(f, 100 * pg + 5, 2);
    wr(f, 1000 * pg, 0x5a, 100 * pg);
    wr(f, (edge - 1) * pg, 0x6b, 2 * pg);
    CHECK(file_byte(run, 0, 1000 * pg) == 0, "a store isn't in the files before it is written: %d", file_byte(run, 0, 1000 * pg));
    uint32_t *p;
    uint64_t n = modified_count(f, &p);
    CHECK(n == 104, "stores: %llu modified, want 104", (unsigned long long)n);
    if (n == 104) CHECK(p[0] == 0 && p[1] == 100 && p[2] == 1000 && p[101] == 1099 && p[102] == edge - 1 && p[103] == edge, "the right pages");
    free(p);

    /* Written: in the files, nothing dirty; the engine asked to take back writes to each page made clean. */
    hv_ramfile_set_revoke(f, on_revoke, NULL);
    atomic_store(&nrevoked, 0);
    CHECK(hv_ramfile_sync(f, 4, NULL, 0) == 0 && modified_count(f, NULL) == 0, "sync: nothing modified");
    CHECK(file_byte(run, 0, 1000 * pg) == 0x5a && file_byte(run, 1, 0) == 0x6b, "written into the files");
    CHECK(atomic_load(&nrevoked) == 104, "writes taken back for the 104 pages made clean: %u", atomic_load(&nrevoked));

    /* A page a device has a pointer to (lent): written at every pass, but lent still, and its writes not taken
       back, for as long as the device holds the pointer (it may write through it pass after pass, unmarked); once
       the device says it's done (hv_dma_written: plain dirty), the next pass makes it clean. */
    uint8_t *dmap = hv_ramfile_dirty_map(f);
    m[3001 * pg + 7] = 0x77;
    __atomic_store_n(&dmap[3001], HV_DIRTY_LENT, __ATOMIC_SEQ_CST);
    atomic_store(&nrevoked, 0);
    CHECK(hv_ramfile_sync(f, 1, NULL, 0) == 0 && file_byte(run, 0, 3001 * pg + 7) == 0x77, "a lent page written");
    n = modified_count(f, &p);
    CHECK(n == 1 && p[0] == 3001 && dmap[3001] == HV_DIRTY_LENT, "and lent still: %llu", (unsigned long long)n);
    free(p);
    m[3001 * pg + 8] = 0x78; /* the device, through its pointer: no mark */
    CHECK(hv_ramfile_sync(f, 1, NULL, 0) == 0 && file_byte(run, 0, 3001 * pg + 8) == 0x78 && dmap[3001] == HV_DIRTY_LENT,
          "a write through the pointer a pass later: written, lent still");
    m[3001 * pg + 9] = 0x79;
    CHECK(hv_ramfile_sync(f, 1, NULL, 0) == 0 && file_byte(run, 0, 3001 * pg + 9) == 0x79 && dmap[3001] == HV_DIRTY_LENT,
          "and two passes later");
    CHECK(atomic_load(&nrevoked) == 0, "no writes taken back for it");
    __atomic_store_n(&dmap[3001], HV_DIRTY, __ATOMIC_SEQ_CST); /* the device is done (as hv_dma_written) */
    CHECK(hv_ramfile_sync(f, 1, NULL, 0) == 0 && modified_count(f, NULL) == 0 && atomic_load(&nrevoked) == 1 && revoked[0] == 3001,
          "done with: the next pass makes it clean (and takes its writes back)");

    /* All-zero pages become holes; a page given back (the balloon) zeros, and a hole once written. */
    wr(f, 1000 * pg, 0, 10 * pg);
    CHECK(hv_ramfile_sync(f, 1, NULL, 0) == 0 && file_hole(run, 0, 1000 * pg, 10 * pg), "zeros written as a hole");
    CHECK(file_byte(run, 0, 1010 * pg) == 0x5a, "the page after kept");
    CHECK(hv_ramfile_discard(f, 1050 * pg + 100, 20 * pg) == 0, "discard");
    CHECK(m[1050 * pg + 100] == 0x5a && m[1051 * pg] == 0 && m[1069 * pg + 77] == 0 && m[1070 * pg + 1] == 0x5a,
          "the whole pages in it zeros, the partial ones kept");
    n = modified_count(f, &p);
    CHECK(n == 19 && p[0] == 1051 && p[18] == 1069, "and dirty: %llu", (unsigned long long)n);
    free(p);
    CHECK(hv_ramfile_sync(f, 1, NULL, 0) == 0 && file_hole(run, 0, 1051 * pg, 19 * pg), "written as a hole");
    /* How long a give-back holds the lock the GPU's mappings take: timed (the one above, the first, logged its line;
       this one is counted for the next) */
    CHECK(hv_ramfile_discard(f, 1051 * pg, 4 * pg) == 0, "given back again");
    uint64_t hold_max, hold_n;
    hv_ramfile_hold_stats(&hold_max, &hold_n);
    CHECK(hold_n == 1 && hold_max > 0 && hold_max < 1000000000ull, "its hold timed: %llu, %llu ns", (unsigned long long)hold_n,
          (unsigned long long)hold_max);
    CHECK(hv_ramfile_sync(f, 1, NULL, 0) == 0 && modified_count(f, NULL) == 0, "and written");
    hv_ramfile_set_revoke(f, NULL, NULL);

    /* A cancelled save stops, with -3, and leaves no copy. */
    uint64_t cancel = 1;
    wr(f, 4000 * pg, 1, 64 * pg);
    HvRamFileSave s;
    CHECK(hv_ramfile_commit(f, snap, 4, &cancel, 2, &s) == -3, "cancelled");
    CHECK(copies_of(top, "state.snap") == 0, "no copy after a cancel");

    /* A save: a copy on storage that is the memory; nothing listed (all written, no GPU). */
    hv_ramfile_check_all(f, 1);
    CHECK(hv_ramfile_commit(f, snap, 4, &cancel, 1, &s) == 0, "commit");
    CHECK(s.npages == 0 && s.nchecks == HV_RAMFILE_CHECKS, "nothing listed (%llu), %u checks", (unsigned long long)s.npages, s.nchecks);
    copy_path(snap, s.gen, dir, sizeof dir);
    uint8_t *buf = malloc(SIZE), *saved = malloc(SIZE);
    CHECK(buf && saved && read_copy(dir, buf) && memcmp(buf, m, SIZE) == 0, "the copy is the memory");
    memcpy(saved, m, SIZE);
    char why[200] = "";
    CHECK(hv_ramfile_check_saved(snap, s.gen, SIZE, why, sizeof why) == 0, "the copy checks out: %s", why);
    HvRamFileSave first = s;

    /* The Mac running on: the copy doesn't change (its blocks are its own once the files are written). */
    wr(f, 50 * pg, 0x99, 300 * pg);
    poke(f, SIZE - 1, 0x42);
    CHECK(hv_ramfile_sync(f, 2, NULL, 0) == 0, "sync after running on");
    CHECK(read_copy(dir, buf) && memcmp(buf, saved, SIZE) == 0, "the copy as it was");

    /* GPU mappings: listed while mapped though not modified, marked modified once unmapped. */
    void *srcs[3] = {m + 5000 * pg, NULL, m + 5002 * pg};
    static int key;
    hv_ramfile_gpu_map(&key, srcs, 3);
    void *outside[1] = {&key};
    hv_ramfile_gpu_map(&srcs, outside, 1); /* not the memory: ignored */
    CHECK(hv_ramfile_gpu_pages(f) == 2, "two pages under the GPU: %llu", (unsigned long long)hv_ramfile_gpu_pages(f));
    CHECK(hv_ramfile_sync(f, 1, NULL, 0) == 0 && modified_count(f, NULL) == 0, "clean");
    CHECK(hv_ramfile_commit(f, snap, 2, NULL, 0, &s) == 0, "commit with the GPU's pages");
    CHECK(s.npages == 2 && s.pages[0] == 5000 && s.pages[1] == 5002, "the GPU's pages listed: %llu", (unsigned long long)s.npages);
    hv_ramfile_gpu_unmap(&key);
    hv_ramfile_gpu_unmap(&srcs);
    hv_ramfile_gpu_unmap(&key); /* twice: nothing */
    CHECK(hv_ramfile_gpu_pages(f) == 2, "unmapped: still taken, until the next save");
    n = modified_count(f, &p);
    CHECK(n == 2 && p[0] == 5000 && p[1] == 5002, "unmapped pages marked modified: %llu", (unsigned long long)n);
    free(p);
    copy_path(snap, s.gen, dir2, sizeof dir2);
    CHECK(copies_of(top, "state.snap") == 2, "two copies");
    CHECK(hv_ramfile_remove_saved(snap, s.gen) == 0 && !exists(dir) && exists(dir2), "older copies deleted, the kept one left");
    hv_ramfile_save_free(&s);
    hv_ramfile_save_free(&first);

    /* A copy that isn't the memory (a change the kernel didn't mark) is caught: no copy, the save is whole instead.
       (A clone takes what is waiting to be written as well: the copy is spoiled after it is made.) */
    wr(f, 7000 * pg, 0x31, pg);
    CHECK(hv_ramfile_sync(f, 1, NULL, 0) == 0, "written");
    hv_ramfile_test_spoil(f, 7000);
    CHECK(hv_ramfile_commit(f, snap, 2, NULL, 0, &s) == 1, "a copy that isn't the memory is refused");
    CHECK(copies_of(top, "state.snap") == 1, "and none left of it");
    hv_ramfile_test_spoil(f, -1);

    /* A save, with pages listed, then a resume in another memory. */
    wr(f, 6000 * pg, 0x44, 8 * pg);
    hv_ramfile_gpu_map(&key, srcs, 1); /* page 5000 listed */
    m[5000 * pg + 9] = 0x55;           /* the GPU writing it: unmarked */
    CHECK(hv_ramfile_commit(f, snap, 4, NULL, 0, &s) == 0, "commit for a resume");
    CHECK(s.npages == 2 && s.pages[0] == 5000 && s.pages[1] == 5002, "the mapped page, and the one mapped before the "
          "last save (a whole one)): %llu", (unsigned long long)s.npages);
    CHECK(hv_ramfile_gpu_pages(f) == 1, "after the save: the mapped page only");
    hv_ramfile_gpu_unmap(&key);
    memcpy(saved, m, SIZE);
    copy_path(snap, s.gen, dir, sizeof dir);
    HvRamFile *g = hv_ramfile_open(run2, SIZE);
    CHECK(g, "a second memory");
    if (g) {
        uint8_t *gm = hv_ramfile_base(g);
        CHECK(hv_ramfile_adopt(g, snap, s.gen, why, sizeof why) == 0, "adopt: %s", why);
        CHECK(hv_ramfile_base(g) == gm, "same address");
        /* Read in whole as it resumed: the run files changing after doesn't change the memory (it isn't them) */
        char g0[1200];
        snprintf(g0, sizeof g0, "%s/0000", run2);
        int gfd = open(g0, O_RDWR);
        uint8_t junk[64];
        memset(junk, 0xab, sizeof junk);
        CHECK(gfd >= 0 && pwrite(gfd, junk, sizeof junk, 6000 * pg) == (ssize_t)sizeof junk, "the run file changed");
        if (gfd >= 0) pwrite(gfd, saved + 6000 * pg, sizeof junk, 6000 * pg), close(gfd); /* and back */
        CHECK(gm[6000 * pg] == 0x44, "the memory read in at the resume: %#x", gm[6000 * pg]);
        for (uint64_t i = 0; i < s.npages; i++) memcpy(gm + s.pages[i] * pg, saved + s.pages[i] * pg, pg);
        CHECK(memcmp(gm, saved, SIZE) == 0, "the copy and the listed pages are the memory");
        CHECK(hv_ramfile_verify(g, s.check, s.nchecks) == 0, "checks pass");
        /* The resumed Mac running: the copy doesn't change */
        wr(g, 0, 0x12, 64 * pg);
        CHECK(hv_ramfile_sync(g, 2, NULL, 0) == 0, "sync the resumed one");
        int got = read_copy(dir, buf);
        for (uint64_t i = 0; got && i < s.npages; i++) memcpy(buf + s.pages[i] * pg, saved + s.pages[i] * pg, pg);
        CHECK(got && memcmp(buf, saved, SIZE) == 0, "the copy as saved");
        /* A piece that differs fails the checks */
        uint64_t at = (uint64_t)s.check[0].chunk * HV_RAMFILE_CHECK_BYTES;
        gm[at + 3] ^= 0xff;
        CHECK(hv_ramfile_verify(g, s.check, s.nchecks) != 0, "a changed piece is caught");
        /* A refused resume starting afresh: zeros */
        CHECK(hv_ramfile_make_fresh(g) == 0 && hv_ramfile_base(g) == gm, "fresh");
        int zero = 1;
        for (uint64_t i = 0; i < SIZE; i += 4096) zero &= gm[i] == 0 && gm[i + 4095] == 0;
        CHECK(zero, "zeros after a fresh start");
        CHECK(modified_count(g, NULL) == 0, "nothing modified after it");
        /* And the copy is still there, whole, for another try */
        CHECK(hv_ramfile_adopt(g, snap, s.gen, why, sizeof why) == 0, "adopt again: %s", why);
        hv_ramfile_close(g);
        CHECK(!exists(run2), "closed: the run folder gone");
    }

    /* Copies refused: another generation, a damaged id, a missing file. */
    uint8_t other[16];
    memcpy(other, s.gen, 16);
    other[0] ^= 1;
    CHECK(hv_ramfile_check_saved(snap, other, SIZE, why, sizeof why) != 0, "another generation");
    CHECK(hv_ramfile_check_saved(snap, s.gen, SIZE * 2, why, sizeof why) != 0, "another size");
    char idp[1300];
    snprintf(idp, sizeof idp, "%s/id", dir);
    int ifd = open(idp, O_RDWR);
    uint8_t b;
    pread(ifd, &b, 1, 20);
    b ^= 1;
    pwrite(ifd, &b, 1, 20);
    CHECK(hv_ramfile_check_saved(snap, s.gen, SIZE, why, sizeof why) != 0, "damaged id: %s", why);
    b ^= 1;
    pwrite(ifd, &b, 1, 20);
    close(ifd);
    CHECK(hv_ramfile_check_saved(snap, s.gen, SIZE, why, sizeof why) == 0, "repaired");
    snprintf(idp, sizeof idp, "%s/0001", dir);
    unlink(idp);
    CHECK(hv_ramfile_check_saved(snap, s.gen, SIZE, why, sizeof why) != 0, "a file missing: %s", why);
    CHECK(hv_ramfile_remove_copy(snap, s.gen) == 0 && !exists(dir), "one copy deleted");
    hv_ramfile_save_free(&s);
    CHECK(hv_ramfile_remove_saved(snap, NULL) == 0 && copies_of(top, "state.snap") == 0, "all deleted");

    /* An auto-save (v3): the memory as it was at one instant T, saved while the Mac runs on, kept by a save guard. */
    {
        TGuard tg = {.f = f, .m = m, .pg = pg, .npages = SIZE / pg, .cap = SIZE / pg};
        tg.state = calloc(tg.npages, 1);
        tg.buf = malloc(tg.cap * pg);
        tg.slot_page = calloc(tg.cap, sizeof *tg.slot_page);
        pthread_mutex_init(&tg.mu, NULL);
        for (uint64_t i = 0; i < SIZE; i += pg) poke(f, i + 17, (uint8_t)(i / pg * 7 + 3));
        void *tgp[2] = {m + 900 * pg, m + 901 * pg};
        static int tkey, tkey2;
        hv_ramfile_gpu_map(&tkey, tgp, 2);
        uint64_t left = hv_ramfile_catch_up(f, 0, 4, 10000000000ull, 4, NULL, 0);
        CHECK(left == 0, "caught up: nothing left (the GPU's pages apart): %llu", (unsigned long long)left);
        memset(m + 900 * pg, 0x61, 2 * pg);  /* the GPU's pages */
        wr(f, 3000 * pg, 0x62, 3 * pg); /* changed after the catch-up: still modified at T */
        wr(f, 3100 * pg, 0x63, 5 * pg); /* ...and left alone after it */
        hv_ramfile_check_all(f, 1);
        HvRamFileSave t;
        CHECK(hv_ramfile_prepare(f, &t) == 0, "prepared");
        CHECK(t.nverify == SIZE / HV_RAMFILE_CHECK_BYTES, "every piece checked: %u", t.nverify);
        /* T: the Mac stopped */
        tarm(&tg);
        memcpy(saved, m, SIZE);
        CHECK(hv_ramfile_mark(f, 4, &t) == 0 && t.nchecks == HV_RAMFILE_CHECKS, "marked: %u checks", t.nchecks);
        /* Running again: the GPU's pages claimed before it runs; a mapping made after is claimed as it is made */
        uint64_t gp = hv_ramfile_gpu_pages(f); /* these two, and pages under a mapping before (no save since) */
        CHECK(gp >= 2 && hv_ramfile_claim_gpu(f, tclaim, &tg) == gp && !tg.state[900] && !tg.state[901], "the GPU's pages claimed: %llu",
              (unsigned long long)gp);
        void *tgp2[1] = {m + 1500 * pg};
        hv_ramfile_gpu_map(&tkey2, tgp2, 1);
        memset(m + 1500 * pg, 0x74, pg); /* the GPU writing it: no claim of its own */
        memset(m + 900 * pg, 0x71, 2 * pg);
        twrite(&tg, 2990, 0x72, 20); /* over the pages modified at T */
        twrite(&tg, 0, 0x73, 4);
        CHECK(hv_ramfile_sync(f, 2, NULL, 0) == 0, "written while it runs"); /* the writer: post-T bytes into the files */
        GRacer gr = {.g = &tg};
        pthread_t grt;
        pthread_create(&grt, NULL, grace, &gr);
        while (atomic_load(&gr.stores) < 1000) usleep(100);
        uint64_t cancel2 = 5;
        int rc = hv_ramfile_take(f, snap, 4, &cancel2, 5, &t);
        CHECK(rc == 0, "taken: %d", rc);
        copy_path(snap, t.gen, dir, sizeof dir);
        CHECK(exists(dir), "the copy is there");
        twrite(&tg, 5000, 0x75, 8); /* after the take, before the disarm */
        hv_ramfile_claim_stop(f);
        atomic_store(&gr.stop, 1);
        pthread_join(grt, NULL);
        memset(tg.state, 0, tg.npages); /* disarmed */
        CHECK(!tg.full, "the guard's buffer held every page");
        rc = hv_ramfile_claimed(f, snap, &t, tg.slot_page, tg.used, 0, tg.buf);
        CHECK(rc == 0 && t.npages == tg.used && t.copy == tg.buf, "the claimed pages listed: %d, %llu of %llu", rc,
              (unsigned long long)t.npages, (unsigned long long)tg.used);
        int asc = 1;
        for (uint64_t k = 1; k < t.npages; k++) asc &= t.pages[k] > t.pages[k - 1];
        CHECK(asc, "ascending");
        int held = 1;
        for (uint64_t k = 0; k < t.npages; k++) held &= memcmp(tg.buf + (uint64_t)t.slot[k] * pg, saved + (uint64_t)t.pages[k] * pg, pg) == 0;
        CHECK(held, "each claimed page's bytes are T's");
        rc = hv_ramfile_finish(f, snap, 2, &cancel2, 5, &t);
        CHECK(rc == 0, "finished: %d", rc);
        CHECK(hv_ramfile_check_saved(snap, t.gen, SIZE, why, sizeof why) == 0, "the copy checks out: %s", why);
        int got = read_copy(dir, buf);
        for (uint64_t k = 0; got && k < t.npages; k++) memcpy(buf + (uint64_t)t.pages[k] * pg, tg.buf + (uint64_t)t.slot[k] * pg, pg);
        CHECK(got && memcmp(buf, saved, SIZE) == 0, "the copy and the claimed pages are the memory at T");
        CHECK(memcmp(m, saved, SIZE) != 0, "(and the Mac ran on)");
        /* A resume of it */
        HvRamFile *g2 = hv_ramfile_open(run2, SIZE);
        CHECK(g2 && hv_ramfile_adopt(g2, snap, t.gen, why, sizeof why) == 0, "adopt the auto-save: %s", why);
        if (g2) {
            uint8_t *gm = hv_ramfile_base(g2);
            for (uint64_t k = 0; k < t.npages; k++) memcpy(gm + (uint64_t)t.pages[k] * pg, tg.buf + (uint64_t)t.slot[k] * pg, pg);
            CHECK(memcmp(gm, saved, SIZE) == 0, "resumed: the memory at T");
            CHECK(hv_ramfile_verify(g2, t.check, t.nchecks) == 0, "a resume's checks pass");
            hv_ramfile_close(g2);
        }
        hv_ramfile_save_free(&t);
        CHECK(t.verify == NULL && t.copy == NULL && t.slot == NULL, "freed");

        /* One save's steps, the guard armed (to the take, or short of it). */
#define T_UPTO_CLAIM()                                                                                                  \
    do {                                                                                                               \
        CHECK(hv_ramfile_prepare(f, &t) == 0, "prepared");                                                             \
        tarm(&tg);                                                                                                     \
        CHECK(hv_ramfile_mark(f, 2, &t) == 0, "marked");                                                               \
        hv_ramfile_claim_gpu(f, tclaim, &tg);                                                                          \
    } while (0)
#define T_DISARM() (hv_ramfile_claim_stop(f), memset(tg.state, 0, tg.npages))

        /* A write the guard missed, that reached the files: the copy isn't T's, caught at the finish */
        T_UPTO_CLAIM();
        wr(f, 4321 * pg, 0x55, pg);
        CHECK(hv_ramfile_sync(f, 2, NULL, 0) == 0, "the missed write written");
        CHECK(hv_ramfile_take(f, snap, 2, NULL, 0, &t) == 0, "taken");
        copy_path(snap, t.gen, dir2, sizeof dir2);
        T_DISARM();
        CHECK(hv_ramfile_claimed(f, snap, &t, tg.slot_page, tg.used, 0, tg.buf) == 0, "claimed");
        CHECK(hv_ramfile_finish(f, snap, 2, NULL, 0, &t) == 1 && !exists(dir2), "a copy with a missed write is refused and removed");
        CHECK(t.pages == NULL && t.verify == NULL, "and the save freed");

        /* A copy that isn't what the files held (storage got it wrong): refused */
        T_UPTO_CLAIM();
        hv_ramfile_test_spoil(f, 4400);
        CHECK(hv_ramfile_take(f, snap, 3, NULL, 0, &t) == 0, "taken (to be spoiled)");
        hv_ramfile_test_spoil(f, -1);
        copy_path(snap, t.gen, dir2, sizeof dir2);
        T_DISARM();
        CHECK(hv_ramfile_claimed(f, snap, &t, tg.slot_page, tg.used, 0, tg.buf) == 0, "claimed");
        CHECK(hv_ramfile_finish(f, snap, 2, NULL, 0, &t) == 1 && !exists(dir2), "a spoiled copy is refused and removed");

        /* A finish cancelled, a take cancelled: -3, nothing left */
        T_UPTO_CLAIM();
        CHECK(hv_ramfile_take(f, snap, 2, NULL, 0, &t) == 0, "taken (to be cancelled)");
        copy_path(snap, t.gen, dir2, sizeof dir2);
        T_DISARM();
        CHECK(hv_ramfile_claimed(f, snap, &t, tg.slot_page, tg.used, 0, tg.buf) == 0, "claimed");
        cancel2 = 6;
        CHECK(hv_ramfile_finish(f, snap, 2, &cancel2, 5, &t) == -3 && !exists(dir2), "cancelled finish: no copy");
        T_UPTO_CLAIM();
        CHECK(hv_ramfile_take(f, snap, 2, &cancel2, 5, &t) == -3, "cancelled take");
        T_DISARM();
        copy_path(snap, t.gen, dir2, sizeof dir2);
        CHECK(!exists(dir2) && t.verify == NULL, "no copy, the save freed");

        /* Taken with no claim (hv_ramfile_claim_gpu skipped): refused */
        CHECK(hv_ramfile_prepare(f, &t) == 0 && hv_ramfile_mark(f, 2, &t) == 0, "prepared");
        CHECK(hv_ramfile_take(f, snap, 2, NULL, 0, &t) == -1, "no claim: refused");

        /* A slot twice, or past the memory (a guard gone wrong): refused, the copy removed */
        T_UPTO_CLAIM();
        twrite(&tg, 6000, 0x41, 1);
        CHECK(hv_ramfile_take(f, snap, 2, NULL, 0, &t) == 0, "taken");
        copy_path(snap, t.gen, dir2, sizeof dir2);
        T_DISARM();
        tg.slot_page[tg.used] = tg.slot_page[0];
        CHECK(hv_ramfile_claimed(f, snap, &t, tg.slot_page, tg.used + 1, 0, tg.buf) == 1 && !exists(dir2), "a page twice: refused");
        T_UPTO_CLAIM();
        CHECK(hv_ramfile_take(f, snap, 2, NULL, 0, &t) == 0, "taken");
        copy_path(snap, t.gen, dir2, sizeof dir2);
        T_DISARM();
        tg.slot_page[0] = SIZE / pg;
        CHECK(hv_ramfile_claimed(f, snap, &t, tg.slot_page, tg.used ? tg.used : 1, 0, tg.buf) == 1 && !exists(dir2),
              "a page past the memory: refused");
        /* Another region's slots are not this memory's */
        T_UPTO_CLAIM();
        twrite(&tg, 6001, 0x42, 1);
        CHECK(hv_ramfile_take(f, snap, 2, NULL, 0, &t) == 0, "taken");
        T_DISARM();
        tg.slot_page[tg.used] = 1ull << 48 | 7;
        CHECK(hv_ramfile_claimed(f, snap, &t, tg.slot_page, tg.used + 1, 0, tg.buf) == 0, "claimed");
        int seven = 0;
        for (uint64_t k = 0; k < t.npages; k++) seven |= t.pages[k] == 7;
        CHECK(!seven, "region 1's page 7 is not listed");
        CHECK(hv_ramfile_finish(f, snap, 2, NULL, 0, &t) == 0, "finished");
        hv_ramfile_save_free(&t);
#undef T_UPTO_CLAIM
#undef T_DISARM

        /* A cancelled catch-up */
        wr(f, 100 * pg, 0x67, 64 * pg);
        uint64_t c3 = 9;
        CHECK(hv_ramfile_catch_up(f, 0, 4, 10000000000ull, 2, &c3, 8) == UINT64_MAX, "cancelled catch-up");

        hv_ramfile_gpu_unmap(&tkey);
        hv_ramfile_gpu_unmap(&tkey2);
        hv_ramfile_check_all(f, 0);
        CHECK(hv_ramfile_remove_saved(snap, NULL) == 0, "copies deleted");
        free(tg.state), free(tg.buf), free(tg.slot_page);
    }

    /* Reset: zeros at the same address. */
    wr(f, 0, 0xee, 16 * pg);
    CHECK(hv_ramfile_reset(f) == 0, "reset");
    CHECK(hv_ramfile_base(f) == m, "same address");
    int zero = 1;
    for (uint64_t i = 0; i < SIZE; i += 4096) zero &= m[i] == 0 && m[i + 4095] == 0;
    CHECK(zero, "zeros after a reset");
    CHECK(modified_count(f, NULL) == 0, "nothing modified after a reset");

    /* The writer: nothing below the mark; above it, down to half of it, within the budget; a hold stops it. */
    hv_ramfile_set_policy(f, 4ull << 20, 1.0, 8ull << 20, 50);
    wr(f, 0, 1, 2ull << 20);
    CHECK(hv_ramfile_writer_pass(f) == 0, "below the mark: nothing written");
    wr(f, 0, 2, 32ull << 20);
    uint64_t w = hv_ramfile_writer_pass(f);
    CHECK(w > 0 && w <= (8ull << 20), "within the budget: %llu", (unsigned long long)w);
    CHECK(modified_count(f, NULL) * pg == (32ull << 20) - w, "the rest still modified");
    hv_ramfile_set_policy(f, 1ull << 20, 1e12, 1ull << 40, 50);
    w = hv_ramfile_writer_pass(f);
    CHECK(modified_count(f, NULL) * pg == (512ull << 10) && modified_count(f, NULL) * pg + w >= (24ull << 20),
          "a big budget writes down to half the mark (%llu written)", (unsigned long long)w);
    HvRamFileStats st;
    hv_ramfile_stats(f, &st);
    CHECK(st.high == (1ull << 20) && st.low == (512ull << 10), "the marks");
    hv_ramfile_set_policy(f, 0, 1e12, 1ull << 40, 50);
    hv_ramfile_stats(f, &st);
    CHECK(st.high >= (256ull << 20) && st.high <= (4ull << 30), "a mark from the rate: %llu MB", (unsigned long long)(st.high >> 20));

    hv_ramfile_set_policy(f, 1, 1e12, 1ull << 40, 30); /* any page: all of them */
    hv_ramfile_writer_start(f);
    wr(f, 10 * pg, 9, 50 * pg);
    for (int i = 0; i < 100 && modified_count(f, NULL); i++) usleep(20000);
    CHECK(modified_count(f, NULL) == 0, "the writer writes in the background");
    hv_ramfile_writer_hold(f, 1);
    wr(f, 10 * pg, 8, 50 * pg);
    usleep(200000);
    CHECK(modified_count(f, NULL) == 50, "held: nothing written");
    /* What a save would take now: those pages written into the files, and saved beside them again at most */
    uint64_t gpu_left = hv_ramfile_gpu_pages(f);
    CHECK(gpu_left == 3, "pages 900, 901 and 1500 still taken (no save since their unmap): %llu", (unsigned long long)gpu_left);
    CHECK(hv_ramfile_save_need(f) == (50 + gpu_left) * pg, "save need %llu", (unsigned long long)hv_ramfile_save_need(f));
    static int need_key;
    void *need_gpu[1] = {m + 2000 * pg};
    hv_ramfile_gpu_map(&need_key, need_gpu, 1);
    CHECK(hv_ramfile_save_need(f) == (51 + gpu_left) * pg, "and the page under the GPU");
    hv_ramfile_gpu_unmap(&need_key);
    hv_ramfile_writer_hold(f, 0);
    for (int i = 0; i < 100 && modified_count(f, NULL); i++) usleep(20000);
    CHECK(modified_count(f, NULL) == 0, "goes on after the hold");
    hv_ramfile_stats(f, &st);
    CHECK(st.scans > 0 && st.written > 0, "stats");

    /* The Mac storing all the while the writer writes: nothing it stored is missed. Short rounds (a store lost in one
       is not covered by the next), each saved with every piece of the copy checked. */
    hv_ramfile_set_policy(f, 1, 1e12, 1ull << 40, 2);
    hv_ramfile_check_all(f, 1);
    Racer racer = {.f = f, .m = m, .pg = pg};
    uint64_t before = st.written;
    int bad = 0;
    for (int round = 0; round < 12; round++) {
        atomic_store(&racer.stop, 0);
        pthread_t th;
        hv_ramfile_writer_hold(f, 0);
        pthread_create(&th, NULL, race, &racer);
        usleep(60000);
        atomic_store(&racer.stop, 1);
        pthread_join(th, NULL);
        hv_ramfile_writer_hold(f, 1);
        bad += hv_ramfile_commit(f, snap, 4, NULL, 0, &s) != 0;
        if (round < 11) hv_ramfile_save_free(&s);
    }
    hv_ramfile_check_all(f, 0);
    hv_ramfile_stats(f, &st);
    CHECK(racer.stores > 100000 && st.written > before, "stores %llu while it wrote", (unsigned long long)racer.stores);
    CHECK(bad == 0, "every round saved, every piece the memory: %d not", bad);
    memcpy(saved, m, SIZE);
    g = hv_ramfile_open(run2, SIZE);
    CHECK(g && hv_ramfile_adopt(g, snap, s.gen, why, sizeof why) == 0, "adopt after the race: %s", why);
    if (g) {
        uint8_t *gm = hv_ramfile_base(g);
        for (uint64_t i = 0; i < s.npages; i++) memcpy(gm + s.pages[i] * pg, saved + s.pages[i] * pg, pg);
        CHECK(memcmp(gm, saved, SIZE) == 0, "resumed after the race: the memory");
        hv_ramfile_close(g);
    }
    hv_ramfile_save_free(&s);

    /* Listed again (705, a surface whose run failed): the oldest mapping of the key holds the new list, in one step;
       the page it let go stops being taken, the kept one and the newer mapping's are still taken */
    static int re;
    void *ab[2] = {m + 110 * pg, m + 111 * pg}, *c[1] = {m + 210 * pg}, *a_only[2] = {m + 110 * pg, NULL};
    hv_ramfile_gpu_map(&re, ab, 2);
    hv_ramfile_gpu_map(&re, c, 1);
    hv_ramfile_gpu_relist(&re, NULL, 2); /* nothing to list from: the old list stays */
    hv_ramfile_gpu_relist(&re, a_only, 2);
    CHECK(hv_ramfile_commit(f, snap, 2, NULL, 0, &s) == 0, "saved after a relist");
    hv_ramfile_save_free(&s);
    CHECK(hv_ramfile_gpu_pages(f) == 2, "after it: the kept page and the newer mapping's (%llu)", (unsigned long long)hv_ramfile_gpu_pages(f));
    hv_ramfile_gpu_unmap(&re); /* the oldest: the relisted one, which kept its place */
    CHECK(hv_ramfile_commit(f, snap, 2, NULL, 0, &s) == 0, "saved after its unmap");
    hv_ramfile_save_free(&s);
    CHECK(hv_ramfile_commit(f, snap, 2, NULL, 0, &s) == 0, "and again");
    CHECK(s.npages == 1 && s.pages[0] == 210, "the unmap ended the relisted mapping, the newer still taken (%llu pages, first %llu)",
          (unsigned long long)s.npages, (unsigned long long)(s.npages ? s.pages[0] : 0));
    hv_ramfile_save_free(&s);
    hv_ramfile_gpu_unmap(&re);
    hv_ramfile_gpu_relist(&re, a_only, 2); /* no mapping of the key: nothing */
    CHECK(hv_ramfile_commit(f, snap, 2, NULL, 0, &s) == 0, "saved with nothing mapped");
    hv_ramfile_save_free(&s);
    CHECK(hv_ramfile_gpu_pages(f) == 0, "a relist with no mapping pins nothing (%llu)", (unsigned long long)hv_ramfile_gpu_pages(f));

    /* One key mapped twice (Metal reusing an address): an unmap ends the older mapping, the newer stays. */
    static int twice;
    void *older[1] = {m + 100 * pg}, *newer[1] = {m + 200 * pg};
    hv_ramfile_gpu_map(&twice, older, 1);
    hv_ramfile_gpu_map(&twice, newer, 1);
    hv_ramfile_gpu_unmap(&twice);
    CHECK(hv_ramfile_commit(f, snap, 2, NULL, 0, &s) == 0, "saved with a key mapped twice");
    CHECK(s.npages == 2 && s.pages[0] == 100 && s.pages[1] == 200, "both pages taken: %llu", (unsigned long long)s.npages);
    hv_ramfile_save_free(&s);
    CHECK(hv_ramfile_gpu_pages(f) == 1, "after it: the newer mapping's page only");
    hv_ramfile_gpu_unmap(&twice);

    /* A page given back while a GPU mapping is being made (S3-1): the GPU keeps the page the guest has. The give-back
       waits for the pin, then zeros the page in place; without waiting it would see no pin and swap in new memory,
       the GPU (here: a second mapping of the same memory) keeping the old page. */
    {
        uint64_t at = 300 * pg;
        m[at + 3] = 0x5a;
        vm_address_t alias = 0;
        vm_prot_t cur, max;
        kern_return_t kr = vm_remap(mach_task_self(), &alias, pg, 0, VM_FLAGS_ANYWHERE, mach_task_self(), (vm_address_t)(m + at), FALSE,
                                    &cur, &max, VM_INHERIT_NONE);
        CHECK(kr == KERN_SUCCESS && ((uint8_t *)alias)[3] == 0x5a, "a second mapping of the page: %d", kr);
        PinRace pr = {.f = f, .off = at, .len = pg};
        hv_ramfile_claim_gpu(f, pin_race_claim, &pr);
        static int rkey;
        void *rsrc[1] = {m + at};
        hv_ramfile_gpu_map(&rkey, rsrc, 1);
        hv_ramfile_claim_stop(f);
        CHECK(atomic_load(&pr.started), "the give-back started inside the pin");
        if (atomic_load(&pr.started)) pthread_join(pr.th, NULL);
        CHECK(m[at + 3] == 0, "given back: zeros");
        CHECK(kr == KERN_SUCCESS && ((uint8_t *)alias)[3] == 0, "the GPU's page is the guest's (zeroed, not swapped out): %#x",
              kr == KERN_SUCCESS ? ((uint8_t *)alias)[3] : 0);
        m[at + 4] = 0x66;
        CHECK(kr == KERN_SUCCESS && ((uint8_t *)alias)[4] == 0x66, "and what the guest writes after reaches it");
        hv_ramfile_gpu_unmap(&rkey);
        if (kr == KERN_SUCCESS) vm_deallocate(mach_task_self(), alias, pg);
    }

    /* A mapping whose pages couldn't be listed: from then on every save is whole (last: it stays so). */
    hv_ramfile_gpu_map(&twice, NULL, 4);
    CHECK(hv_ramfile_commit(f, snap, 2, NULL, 0, &s) == 1, "GPU pages untracked: the save whole");
    CHECK(hv_ramfile_remove_saved(snap, NULL) == 0 && copies_of(top, "state.snap") == 0, "no copies left");
    hv_ramfile_writer_hold(f, 1); /* f stays open: its writer runs on till the end */

    free(buf), free(saved);
    if (failures) {
        fprintf(stderr, "test_ramfile: %d failures\n", failures);
        return 1;
    }
    scratch_done();
    printf("test_ramfile: all passed\n");
    return 0;
}
