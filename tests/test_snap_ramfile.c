/* A snapshot's RAM when the memory is kept in files (core/ramfile.h, RAM section version 2), through the save
   format itself: a save holds only what its copy of the files may not; a resume into another memory is the memory
   saved, as often as it is tried; the Mac running on after the save changes nothing in it; a save without its copy,
   or into memory that isn't in files, is refused before anything changes; a save that doesn't finish leaves no
   copy, and a later one or a discard deletes the old copies. And the saves every Henge before wrote (version 1)
   still load, into memory in files as well.

   Usage: test_snap_ramfile [folder] */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "machine.h"
#include "ramfile.h"
#include "scratch_dir.h"
#include "snapshot.h"

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

#define RAM_PA 0x70000000ull
#define RAM_SIZE (256ull << 20)
#define FW_PA 0x100000ull
#define FW_SIZE (1ull << 20)

typedef struct Mac {
    HvMachine m;
    HvRamFile *f; /* NULL: anonymous memory */
    uint8_t *ram, *fw;
} Mac;

static int make_mac(Mac *mac, const char *run) {
    memset(mac, 0, sizeof *mac);
    if (hv_machine_init(&mac->m, 1u << 20) != 0) return -1;
    mac->m.log = 1;
    mac->fw = mmap(NULL, FW_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (run) {
        mac->f = hv_ramfile_open(run, RAM_SIZE);
        if (!mac->f) return -1;
        mac->ram = hv_ramfile_base(mac->f);
    } else {
        mac->ram = mmap(NULL, RAM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    }
    hv_machine_add_ram(&mac->m, FW_PA, FW_SIZE, mac->fw, 0);
    if (hv_machine_add_ram(&mac->m, RAM_PA, RAM_SIZE, mac->ram, 0) != 0) return -1;
    if (mac->f && hv_machine_attach_file(&mac->m, &mac->m.ram[mac->m.nram - 1], mac->f) != 0) return -1;
    return 0;
}

static uint64_t seed = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) {
    seed ^= seed << 13, seed ^= seed >> 7, seed ^= seed << 17;
    return seed;
}

/* The pages written marked dirty, as a device marks what it writes (hv_dma_written). */
static void wrote(Mac *mac, uint64_t at, uint64_t len) {
    if (mac->f) hv_ramfile_mark_dirty(mac->f, at, len);
}

/* Some of everything: random pages, runs of a byte, compressible text, the firmware. */
static void scribble(Mac *mac, int rounds) {
    for (int i = 0; i < rounds; i++) {
        uint64_t at = rnd() % (RAM_SIZE - 65536);
        switch (rnd() % 3) {
        case 0:
            for (int k = 0; k < 2048; k++) ((uint64_t *)(mac->ram + (at & ~7ull)))[k] = rnd();
            wrote(mac, at & ~7ull, 2048 * 8);
            break;
        case 1: {
            uint64_t n = 1 + rnd() % 65536;
            memset(mac->ram + at, (int)(rnd() & 0xff), n);
            wrote(mac, at, n);
            break;
        }
        default:
            snprintf((char *)mac->ram + at, 4096, "line %d of the kernel's log, said at %llu", i, (unsigned long long)at);
            wrote(mac, at, 4096);
        }
    }
    for (uint64_t k = 0; k < FW_SIZE; k += 4096) mac->fw[k] = (uint8_t)rnd();
}

static int save(Mac *mac, const char *path) {
    HvSnapWriter *w = hv_snap_begin(path, HV_SNAP_NO_SYNC);
    if (!w) return -1;
    if (hv_snap_put_ram(w, &mac->m, 4) != 0) {
        hv_snap_abort(w);
        return -1;
    }
    return hv_snap_commit(w);
}

/* 0: refused before anything changed (why says why); 1: loaded; -1: failed while loading. */
static int load(Mac *mac, const char *path, char *why, size_t len) {
    HvSnapReader *r = hv_snap_open(path, why, len);
    if (!r) return 0;
    int rc = hv_snap_check_ram(r, &mac->m, why, len) != 0 ? 0 : hv_snap_get_ram(r, &mac->m, 4, HV_SNAP_DIRTY_RAM) == 0 ? 1 : -1;
    hv_snap_close(r);
    return rc;
}

static int ram_version(const char *path) {
    HvSnapReader *r = hv_snap_open(path, NULL, 0);
    if (!r) return -1;
    HvSnapIn in;
    int v = hv_snap_get(r, HV_SNAP_RAM, 2, &in) == 0 ? 2 : hv_snap_get(r, HV_SNAP_RAM, 1, &in) == 0 ? 1 : 0;
    hv_snap_close(r);
    return v;
}

static int copies(const char *folder) {
    DIR *d = opendir(folder);
    int n = 0;
    struct dirent *e;
    while (d && (e = readdir(d))) n += strstr(e->d_name, ".ram-") != NULL;
    if (d) closedir(d);
    return n;
}

static off_t size_of(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 ? st.st_size : -1;
}

int main(int argc, char **argv) {
    const char *dir = scratch_dir(argc, argv, "test_snap_ramfile");
    char run1[1024], run2[1024], snap[1024], old[1024], why[256];
    snprintf(run1, sizeof run1, "%s/run1", dir);
    snprintf(run2, sizeof run2, "%s/run2", dir);
    snprintf(snap, sizeof snap, "%s/suspended.snap", dir);
    snprintf(old, sizeof old, "%s/old.snap", dir);
    hv_snap_discard(snap);
    hv_snap_discard(old);

    static Mac a, b, plain, c; /* HvMachine is too large for the stack */
    CHECK(make_mac(&a, run1) == 0 && make_mac(&b, run2) == 0 && make_mac(&plain, NULL) == 0, "machines");
    uint64_t pg = hv_ramfile_page(a.f);
    uint8_t *want = malloc(RAM_SIZE), *want_fw = malloc(FW_SIZE);

    /* A Mac that ran a while: much of it written ahead already, some still changed, some under the GPU */
    scribble(&a, 400);
    CHECK(hv_ramfile_sync(a.f, 4, NULL, 0) == 0, "written ahead");
    scribble(&a, 40);
    void *gpu[4] = {a.ram + 10 * pg, a.ram + 11 * pg, a.ram + 5000 * pg, NULL};
    static int key;
    hv_ramfile_gpu_map(&key, gpu, 4);
    a.ram[5000 * pg + 1] ^= 0xff;

    CHECK(save(&a, snap) == 0, "saved");
    memcpy(want, a.ram, RAM_SIZE);
    memcpy(want_fw, a.fw, FW_SIZE);
    CHECK(ram_version(snap) == 2, "RAM version 2: %d", ram_version(snap));
    CHECK(copies(dir) == 1, "one copy beside it: %d", copies(dir));
    CHECK(size_of(snap) < (off_t)(64ull << 20), "the save holds only what changed: %lld MB", (long long)(size_of(snap) >> 20));
    hv_ramfile_gpu_unmap(&key);

    /* The Mac runs on (its save left there): nothing in the save changes */
    scribble(&a, 200);
    CHECK(hv_ramfile_sync(a.f, 4, NULL, 0) == 0, "ran on");

    /* Resumed in another memory: the memory saved. A second try (as after a failed resume) the same. */
    for (int tryno = 0; tryno < 2; tryno++) {
        memset(b.fw, 0, FW_SIZE);
        int rc = load(&b, snap, why, sizeof why);
        CHECK(rc == 1, "resume %d: %d %s", tryno, rc, why);
        CHECK(memcmp(b.ram, want, RAM_SIZE) == 0, "resume %d: the memory saved", tryno);
        CHECK(memcmp(b.fw, want_fw, FW_SIZE) == 0, "resume %d: the firmware saved", tryno);
        memset(b.ram, 0x5c, 1 << 20); /* the resumed Mac running */
        wrote(&b, 0, 1 << 20);
        CHECK(hv_ramfile_sync(b.f, 2, NULL, 0) == 0, "resumed Mac ran");
    }

    /* Refused, before anything changes: into memory that isn't in files; without its copy */
    memset(plain.ram, 0x11, 4096);
    CHECK(load(&plain, snap, why, sizeof why) == 0 && plain.ram[0] == 0x11, "memory not in files refused: %s", why);
    char moved[1100];
    snprintf(moved, sizeof moved, "%s/elsewhere.snap", dir);
    CHECK(rename(snap, moved) == 0, "moved");
    CHECK(load(&b, moved, why, sizeof why) == 0, "without its copy refused: %s", why);
    CHECK(rename(moved, snap) == 0, "back");

    /* Another save over it: the old copy goes once the new save is complete; one that doesn't finish leaves none */
    CHECK(save(&a, snap) == 0 && copies(dir) == 1, "the newer save's copy only: %d", copies(dir));
    memcpy(want, a.ram, RAM_SIZE);
    HvSnapWriter *w = hv_snap_begin(snap, HV_SNAP_NO_SYNC);
    CHECK(w && hv_snap_put_ram(w, &a.m, 4) == 0 && copies(dir) == 2, "a save under way: two copies");
    hv_snap_abort(w);
    CHECK(copies(dir) == 1, "called off: its copy gone");
    CHECK(load(&b, snap, why, sizeof why) == 1 && memcmp(b.ram, want, RAM_SIZE) == 0, "the complete save still loads: %s", why);
    CHECK(hv_snap_discard(snap) == 0 && copies(dir) == 0 && size_of(snap) < 0, "discarded with its copy");

    /* Auto-saves: saved while the Mac runs on, again and again, each into its own folder, the older folder deleted
       only after the newer save is complete. A newer save leaves an older one in another folder alone, and each
       loads as the memory was at its own save, however much ran after it (pages under the GPU coming and going). */
    {
        char fold[2][1100], path[2][1100];
        uint8_t *at[2] = {malloc(RAM_SIZE), malloc(RAM_SIZE)};
        for (int k = 0; k < 2; k++) {
            snprintf(fold[k], sizeof fold[k], "%s/auto%d", dir, k);
            mkdir(fold[k], 0700);
            snprintf(path[k], sizeof path[k], "%s/Mac.hvsnap", fold[k]);
            hv_snap_discard(path[k]);
        }
        static int keys[8];
        for (int round = 0; round < 8; round++) {
            int k = round & 1;
            scribble(&a, 60);
            if (round % 3 == 0) CHECK(hv_ramfile_sync(a.f, 4, NULL, 0) == 0, "auto %d: written ahead", round);
            void *g[3] = {a.ram + (uint64_t)(100 + round) * pg, a.ram + (uint64_t)(7000 + 3 * round) * pg, NULL};
            hv_ramfile_gpu_map(&keys[round], g, 3);
            a.ram[(uint64_t)(100 + round) * pg + 7] ^= (uint8_t)(round + 1);
            if (round) hv_ramfile_gpu_unmap(&keys[round - 1]); /* seen until the next save that works */
            CHECK(save(&a, path[k]) == 0, "auto %d: saved", round);
            memcpy(at[k], a.ram, RAM_SIZE);
            CHECK(copies(fold[k]) == 1, "auto %d: one copy in its folder: %d", round, copies(fold[k]));
            if (round) CHECK(copies(fold[!k]) == 1, "auto %d: the older one's copy left alone", round);
            if (round) { /* the older one still loads as it was (as a crash before the newer one finished would) */
                int rc = load(&b, path[!k], why, sizeof why);
                CHECK(rc == 1 && memcmp(b.ram, at[!k], RAM_SIZE) == 0, "auto %d: the older one as it was: %d %s", round, rc, why);
            }
            CHECK(hv_snap_discard(path[!k]) == 0 && copies(fold[!k]) == 0, "auto %d: the older one deleted", round);
        }
        hv_ramfile_gpu_unmap(&keys[7]);
        scribble(&a, 100); /* the Mac runs on after the last one */
        CHECK(hv_ramfile_sync(a.f, 4, NULL, 0) == 0, "ran on after the last auto-save");
        int rc = load(&b, path[1], why, sizeof why);
        CHECK(rc == 1 && memcmp(b.ram, at[1], RAM_SIZE) == 0, "the newest auto-save as it was: %d %s", rc, why);
        CHECK(hv_snap_discard(path[1]) == 0 && copies(fold[1]) == 0, "auto-saves cleared");
        rmdir(fold[0]), rmdir(fold[1]);
        free(at[0]), free(at[1]);
    }

    /* An auto-save never writes the memory whole (HV_SNAP_NO_WHOLE): when the copy can't be used (here one that
       doesn't match the memory), it fails and leaves nothing; a Suspend falls back to writing it whole. */
    {
        hv_ramfile_check_all(a.f, 1);
        hv_ramfile_test_spoil(a.f, 1234);
        scribble(&a, 20);
        HvSnapWriter *nw = hv_snap_begin(snap, HV_SNAP_NO_SYNC | HV_SNAP_NO_WHOLE);
        CHECK(nw && hv_snap_put_ram(nw, &a.m, 4) != 0, "an auto-save refuses to write the memory whole");
        if (nw) hv_snap_abort(nw);
        CHECK(copies(dir) == 0 && size_of(snap) < 0, "and leaves nothing");
        CHECK(save(&a, snap) == 0 && ram_version(snap) == 1 && copies(dir) == 0, "a Suspend writes it whole instead (version %d)", ram_version(snap));
        memcpy(want, a.ram, RAM_SIZE);
        CHECK(load(&b, snap, why, sizeof why) == 1 && memcmp(b.ram, want, RAM_SIZE) == 0, "which loads: %s", why);
        CHECK(hv_snap_discard(snap) == 0, "cleared");
        hv_ramfile_test_spoil(a.f, -1);
        hv_ramfile_check_all(a.f, 0);
    }

    /* A save every Henge before this wrote (memory not in files): version 1, and it loads into memory in files */
    scribble(&plain, 300);
    CHECK(save(&plain, old) == 0 && ram_version(old) == 1 && copies(dir) == 0, "version 1, no copy");
    memcpy(want, plain.ram, RAM_SIZE);
    memset(b.ram, 0x77, RAM_SIZE / 2);
    wrote(&b, 0, RAM_SIZE / 2);
    CHECK(load(&b, old, why, sizeof why) == 1, "version 1 into memory in files: %s", why);
    CHECK(memcmp(b.ram, want, RAM_SIZE) == 0, "the memory saved (zeros where it had none)");
    CHECK(make_mac(&c, NULL) == 0 && load(&c, old, why, sizeof why) == 1 && memcmp(c.ram, want, RAM_SIZE) == 0,
          "version 1 into memory not in files: %s", why);
    /* And memory in files saved again after: version 2 */
    CHECK(save(&b, snap) == 0 && ram_version(snap) == 2, "saved again, version 2");
    CHECK(hv_snap_discard(snap) == 0 && hv_snap_discard(old) == 0 && copies(dir) == 0, "clean");

    free(want), free(want_fw);
    hv_ramfile_close(a.f), hv_ramfile_close(b.f);
    scratch_done();
    if (failures) {
        fprintf(stderr, "test_snap_ramfile: %d failures\n", failures);
        return 1;
    }
    printf("test_snap_ramfile: all passed\n");
    return 0;
}
