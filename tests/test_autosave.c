/* Auto-save's disk copies (host/disk.c hv_disk_clone): a clone is the disk as it was at that instant, whatever the
   Mac writes after (through the cache or past it); its identity is what host/vm.c's DiskId reads from the file
   opened again after the clone is renamed into the disk's place (the restore's step); the auxiliary disk's offset
   doesn't matter (the whole file is cloned); a clone never replaces a file already there; a large, fragmented image
   clones in a moment. The restore puts a copy of the clone in the disk's place, keeps the disk that was there aside
   (unless it is the same), leaves the clone as it was, and runs on the copy; a start cut short there is recovered.

   Usage: test_autosave [folder] */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../host/disk.h"
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

static void fill(uint8_t *b, size_t n, uint8_t v) { memset(b, v, n); }

static int same_bytes(HvDisk *d, uint64_t off, uint8_t v, size_t n) {
    uint8_t *b = malloc(n);
    int ok = b && hv_disk_read(d, off, b, n) == 0;
    for (size_t i = 0; ok && i < n; i++) ok = b[i] == v;
    free(b);
    return ok;
}

static void id_of(const char *path, uint64_t *ino, uint64_t *size, int64_t *mtime) {
    struct stat st;
    memset(&st, 0, sizeof st);
    stat(path, &st);
    *ino = st.st_ino, *size = (uint64_t)st.st_size;
    *mtime = (int64_t)st.st_mtimespec.tv_sec * 1000000000 + st.st_mtimespec.tv_nsec;
}

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    const char *dir = scratch_dir(argc, argv, "test_autosave");
    char disk_p[1100], aux_p[1100], dclone[1100], aclone[1100];
    snprintf(disk_p, sizeof disk_p, "%s/disk.img", dir);
    snprintf(aux_p, sizeof aux_p, "%s/auxiliary.bin", dir);
    snprintf(dclone, sizeof dclone, "%s/clone-disk.img", dir);
    snprintf(aclone, sizeof aclone, "%s/clone-aux.img", dir);
    unlink(disk_p), unlink(aux_p), unlink(dclone), unlink(aclone);

    /* The Mac's two disks: 256 MB sparse, and the auxiliary one seen from 16 KB in */
    int fd = open(disk_p, O_RDWR | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0 && ftruncate(fd, 256 << 20) == 0, "disk made");
    close(fd);
    fd = open(aux_p, O_RDWR | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0 && ftruncate(fd, 4 << 20) == 0 && pwrite(fd, "META", 4, 0) == 4, "aux made");
    close(fd);

    for (int cached = 1; cached >= 0; cached--) { /* through the host's cache, then past it (HV_WRITE_CACHE=0) */
        setenv("HV_WRITE_CACHE", cached ? "1" : "0", 1);
        unlink(dclone), unlink(aclone);
        HvDisk disk, aux;
        CHECK(hv_disk_open(&disk, disk_p, 0, 0) == 0 && hv_disk_open(&aux, aux_p, 16384, 0) == 0, "opened (%d)", cached);
        CHECK(cached || disk.wfd >= 0, "writes past the cache");
        size_t n = 1 << 20;
        uint8_t *b = malloc(n);
        fill(b, n, 0xA1);
        CHECK(hv_disk_write(&disk, 7 << 20, b, n) == 0, "written before");
        fill(b, 4096, 0xB1);
        CHECK(hv_disk_write(&aux, 0, b, 4096) == 0, "aux written before");

        uint64_t ino, size, ino2, size2;
        int64_t mt, mt2;
        CHECK(hv_disk_clone(&disk, dclone, &ino, &size, &mt) == 0, "disk cloned (%d)", cached);
        CHECK(hv_disk_clone(&aux, aclone, NULL, NULL, NULL) == 0, "aux cloned");
        id_of(disk_p, &ino2, &size2, &mt2);
        CHECK(ino != ino2 && size == size2, "a file of its own, the same size");

        /* The Mac runs on: its disks change, the clones don't */
        fill(b, n, 0xA2);
        CHECK(hv_disk_write(&disk, 7 << 20, b, n) == 0, "written after");
        fill(b, n, 0xA3);
        CHECK(hv_disk_write(&disk, 100 << 20, b, n) == 0, "written after, elsewhere");
        fill(b, 4096, 0xB2);
        CHECK(hv_disk_write(&aux, 0, b, 4096) == 0, "aux written after");
        CHECK(hv_disk_flush(&disk) == 0 && hv_disk_sync(&disk) == 0, "synced");
        HvDisk c, ca;
        CHECK(hv_disk_open(&c, dclone, 0, 1) == 0 && hv_disk_open(&ca, aclone, 16384, 1) == 0, "clones open");
        CHECK(same_bytes(&c, 7 << 20, 0xA1, n), "the clone as it was (%d)", cached);
        CHECK(same_bytes(&c, 100 << 20, 0, n), "the clone without what came after (%d)", cached);
        CHECK(same_bytes(&ca, 0, 0xB1, 4096), "the aux clone as it was");
        CHECK(same_bytes(&disk, 7 << 20, 0xA2, n), "the disk as written after");
        char meta[4] = {0};
        CHECK(pread(ca.fd, meta, 4, 0) == 4 && !memcmp(meta, "META", 4), "the aux clone's first 16 KB too");
        hv_disk_close(&c), hv_disk_close(&ca);

        /* A clone never takes the place of a file already there */
        CHECK(hv_disk_clone(&disk, dclone, NULL, NULL, NULL) != 0, "not over an existing file");
        CHECK(hv_disk_open(&c, dclone, 0, 1) == 0 && same_bytes(&c, 7 << 20, 0xA1, n), "which is unchanged");
        hv_disk_close(&c);
        hv_disk_close(&disk), hv_disk_close(&aux);

        /* The restore: the machine built on the clone; a copy of it then takes the disk's place (hv_disk_place), the
           disk that was there kept aside, and the machine runs on the copy. The clone (the auto-save) is untouched. */
        char aside[1100], placing[1100];
        snprintf(aside, sizeof aside, "%s/aside.img", dir);
        snprintf(placing, sizeof placing, "%s.placing", disk_p);
        unlink(aside);
        uint64_t newer_ino, s2;
        int64_t m2;
        id_of(disk_p, &newer_ino, &s2, &m2);
        CHECK(hv_disk_open(&c, dclone, 0, 0) == 0, "built on the clone");
        CHECK(hv_disk_place(&c, disk_p, aside) == 0, "put in place");
        CHECK(access(dclone, F_OK) == 0, "the auto-save's clone is still there");
        uint64_t ai, as, pi, ps;
        int64_t am, pm;
        id_of(aside, &ai, &as, &am);
        id_of(disk_p, &pi, &ps, &pm);
        CHECK(ai == newer_ino && am == m2, "the newer disk kept aside, as it was");
        CHECK(pi != ino && pi != newer_ino && ps == size && pm == mt, "a file of its own in the disk's place, the clone's time");
        CHECK(access(placing, F_OK) != 0, "nothing left over");
        struct stat st, cst;
        CHECK(fstat(c.fd, &st) == 0 && st.st_ino == pi, "the machine runs on the file in the disk's place");
        CHECK(same_bytes(&c, 7 << 20, 0xA1, n) && same_bytes(&c, 100 << 20, 0, n), "the disk as it was");
        fill(b, n, 0xA4);
        CHECK(hv_disk_write(&c, 7 << 20, b, n) == 0, "the restored Mac writes");
        HvDisk cc, as_d;
        CHECK(hv_disk_open(&cc, dclone, 0, 1) == 0 && same_bytes(&cc, 7 << 20, 0xA1, n), "which the auto-save never sees");
        CHECK(hv_disk_open(&as_d, aside, 0, 1) == 0 && same_bytes(&as_d, 7 << 20, 0xA2, n) && same_bytes(&as_d, 100 << 20, 0xA3, n),
              "nor the disk kept aside");
        hv_disk_close(&cc), hv_disk_close(&as_d), hv_disk_close(&c);

        /* A second Carry On from the same auto-save, the disk written since (above): that newer disk is kept aside
           too, under a name of its own; the first one kept aside is never replaced */
        char aside2[1100], aside3[1100], aside4[1100];
        snprintf(aside2, sizeof aside2, "%s/aside 2.img", dir);
        snprintf(aside3, sizeof aside3, "%s/aside 3.img", dir);
        snprintf(aside4, sizeof aside4, "%s/aside 4.img", dir);
        unlink(aside2), unlink(aside3), unlink(aside4);
        uint64_t written_ino, ws;
        int64_t wm;
        id_of(disk_p, &written_ino, &ws, &wm);
        CHECK(hv_disk_open(&c, dclone, 0, 0) == 0 && hv_disk_place(&c, disk_p, aside) == 0, "placed over the written one");
        uint64_t a1i, a2i, x;
        int64_t a1m, a2m;
        id_of(aside, &a1i, &x, &a1m);
        id_of(aside2, &a2i, &x, &a2m);
        CHECK(a1i == ai && a1m == am, "the first one kept aside is as it was");
        CHECK(a2i == written_ino && a2m == wm, "the newer one kept beside it, as \"aside 2.img\"");
        hv_disk_close(&c);

        /* Again, nothing written since: the disk there is the same one, so nothing new is kept aside */
        CHECK(hv_disk_open(&c, dclone, 0, 0) == 0 && hv_disk_place(&c, disk_p, aside) == 0, "placed over an unwritten one");
        CHECK(access(aside3, F_OK) != 0, "the same disk isn't kept aside again");
        hv_disk_close(&c);

        /* A start that ended after its swap: the old disk at <disk>.placing is kept aside first, under a free name */
        int pf = open(placing, O_RDWR | O_CREAT | O_TRUNC, 0600);
        CHECK(pf >= 0 && ftruncate(pf, 256 << 20) == 0 && pwrite(pf, "OLD!", 4, 0) == 4, "a leftover");
        close(pf);
        CHECK(hv_disk_open(&c, dclone, 0, 0) == 0 && hv_disk_place(&c, disk_p, aside) == 0, "placed with one left over");
        char head[4] = {0};
        pf = open(aside3, O_RDONLY);
        CHECK(pf >= 0 && pread(pf, head, 4, 0) == 4 && !memcmp(head, "OLD!", 4), "the leftover kept aside as \"aside 3.img\"");
        if (pf >= 0) close(pf);
        id_of(aside, &a1i, &x, &a1m);
        CHECK(a1i == ai && a1m == am, "the first one still as it was");
        CHECK(access(placing, F_OK) != 0, "and gone from there");
        hv_disk_close(&c);

        /* A start that ended before its swap: <disk>.placing is a copy of the clone (the same size and time): it goes */
        CHECK(hv_disk_open(&c, dclone, 0, 0) == 0 && hv_disk_clone(&c, placing, NULL, NULL, NULL) == 0, "a copy left over");
        CHECK(hv_disk_place(&c, disk_p, aside) == 0, "placed with a copy left over");
        CHECK(access(aside4, F_OK) != 0 && access(placing, F_OK) != 0, "the copy deleted, not kept aside");
        hv_disk_close(&c);

        /* The app's undo of a Carry On that didn't finish (MacAutoSave.undoUnfinished): the disk from before found
           among those kept aside by its size and time, and swapped back in; what it swaps out has the clone's size
           and time (a copy of the auto-save's disk, which the app may delete) */
        CHECK(hv_disk_open(&c, disk_p, 0, 0) == 0, "the disk open");
        fill(b, n, 0xA5);
        CHECK(hv_disk_write(&c, 9 << 20, b, n) == 0 && hv_disk_flush(&c) == 0, "written: newer than the auto-save");
        hv_disk_close(&c);
        uint64_t bi, bs, ui, us;
        int64_t bm, um;
        id_of(disk_p, &bi, &bs, &bm);
        CHECK(hv_disk_open(&c, dclone, 0, 0) == 0 && hv_disk_place(&c, disk_p, aside) == 0, "placed (and then Henge ends)");
        hv_disk_close(&c);
        id_of(aside4, &ui, &us, &um);
        CHECK(ui == bi && us == bs && um == bm, "the disk from before, kept aside with its size and time");
        CHECK(renamex_np(aside4, disk_p, RENAME_SWAP) == 0, "swapped back");
        id_of(disk_p, &ui, &us, &um);
        CHECK(ui == bi && us == bs && um == bm, "the disk from before in its place");
        id_of(aside4, &ui, &us, &um);
        CHECK(us == size && um == mt, "what it swapped out has the clone's size and time");
        CHECK(hv_disk_open(&c, disk_p, 0, 1) == 0 && same_bytes(&c, 9 << 20, 0xA5, n), "and the disk holds what was written");
        hv_disk_close(&c);
        unlink(aside4);

        /* reopen: refused for a file of another size, the disk unchanged */
        CHECK(hv_disk_open(&c, disk_p, 0, 0) == 0, "open");
        int before = c.fd;
        CHECK(hv_disk_reopen(&c, aux_p) != 0 && c.fd == before, "not a file of another size");
        CHECK(hv_disk_reopen(&c, dclone) == 0 && fstat(c.fd, &st) == 0 && stat(dclone, &cst) == 0 && st.st_ino == cst.st_ino,
              "onto a file of the same size");
        hv_disk_close(&c);
        unlink(aside), unlink(aside2), unlink(aside3), unlink(aclone);
        free(b);
    }

    /* Big and fragmented: 64 GB sparse, 20,000 scattered writes. A moment, not a copy. */
    {
        char big[1100], bigc[1100];
        snprintf(big, sizeof big, "%s/big.img", dir);
        snprintf(bigc, sizeof bigc, "%s/big-clone.img", dir);
        unlink(big), unlink(bigc);
        fd = open(big, O_RDWR | O_CREAT | O_TRUNC, 0600);
        CHECK(fd >= 0 && ftruncate(fd, 64ull << 30) == 0, "big made");
        uint8_t blk[16384];
        memset(blk, 7, sizeof blk);
        for (int i = 0; i < 20000; i++) (void)pwrite(fd, blk, sizeof blk, (off_t)((uint64_t)(arc4random() % 4000000) * 16384));
        close(fd);
        HvDisk d;
        CHECK(hv_disk_open(&d, big, 0, 0) == 0, "big open");
        double t = now();
        CHECK(hv_disk_clone(&d, bigc, NULL, NULL, NULL) == 0, "big cloned");
        double ms = (now() - t) * 1e3;
        printf("test_autosave: a 64 GB image with 20,000 pieces cloned and on storage in %.1f ms\n", ms);
        CHECK(ms < 2000, "cloned in a moment: %.0f ms", ms);
        hv_disk_close(&d);
        unlink(big), unlink(bigc);
    }

    unlink(disk_p), unlink(aux_p), unlink(dclone), unlink(aclone);
    scratch_done();
    if (failures) {
        fprintf(stderr, "test_autosave: %d failures\n", failures);
        return 1;
    }
    printf("test_autosave: all passed\n");
    return 0;
}
