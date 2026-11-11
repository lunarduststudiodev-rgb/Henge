/* An auto-save's disk guard (host/disk.h): the copy of a disk is the disk as it was at the instant it was armed,
   though writers went on writing it all the while, the clone included. Writers write random runs (unaligned,
   from 512 bytes to 300 KB, some across many blocks); each round stops them (as the stop waits for no request to
   be under way), records the image (the truth at T), arms, lets them go, clones while they write, and the clone
   must be the truth byte for byte, the offset part included. A log too small voids the save without stopping a
   writer; a guard given up without a clone ends cleanly.

   usage: test_disk_guard [folder] [rounds=12] */
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

#define OFFSET 16384ull
#define SIZE ((64ull << 20) + 7 * 512) /* not a whole number of blocks */

static HvDisk disk;
static pthread_rwlock_t gate = PTHREAD_RWLOCK_INITIALIZER; /* writers read-hold it per write; the stop takes it */
static volatile int stop_writers;
static uint64_t writes[4]; /* each writer's count, read by the main thread as they run */
static uint64_t total_writes(void) {
    uint64_t n = 0;
    for (int i = 0; i < 4; i++) n += __atomic_load_n(&writes[i], __ATOMIC_RELAXED);
    return n;
}
/* Until the writers have done more than n writes, or 10 s: a fixed time, a busy Mac may give them none of. The total
   then, for the check. */
static uint64_t writes_past(uint64_t n) {
    for (uint64_t until = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 10000000000ull;
         total_writes() <= n && clock_gettime_nsec_np(CLOCK_UPTIME_RAW) < until;)
        usleep(1000);
    return total_writes();
}
typedef struct WRec { uint64_t off, len; uint8_t v; uint32_t epoch; int guarded, id; } WRec;
#define LOGN (1u << 20)
static WRec wlog[LOGN];
static uint64_t nlog;
static uint32_t epoch;
static HvDiskGuard *g_last;

static void *writer(void *arg) {
    int id = (int)(intptr_t)arg;
    uint64_t s = 0x9e3779b97f4a7c15ull * (uint64_t)(id + 1);
    uint8_t *buf = malloc(300 << 10);
    while (!stop_writers) {
        s ^= s << 13, s ^= s >> 7, s ^= s << 17;
        uint64_t len = 512 * (1 + s % (s & 1 ? 8 : 600)), off = 512 * ((s >> 20) % (SIZE / 512));
        if (off + len > SIZE) len = SIZE - off;
        memset(buf, (int)(s >> 40), len);
        pthread_rwlock_rdlock(&gate);
        {
            uint64_t k = __atomic_fetch_add(&nlog, 1, __ATOMIC_RELAXED) % LOGN;
            wlog[k] = (WRec){off, len, (uint8_t)(s >> 40), __atomic_load_n(&epoch, __ATOMIC_ACQUIRE), disk.guard != NULL, id};
        }
        if (hv_disk_write(&disk, off, buf, len) != 0) fprintf(stderr, "FAIL: write %llu+%llu\n", (unsigned long long)off, (unsigned long long)len);
        pthread_rwlock_unlock(&gate);
        __atomic_fetch_add(&writes[id], 1, __ATOMIC_RELAXED);
        if ((s & 7) == 0) usleep(50);
    }
    free(buf);
    return NULL;
}

static int read_file(const char *path, uint8_t *to, uint64_t n) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t got = pread(fd, to, n, 0);
    close(fd);
    return got == (ssize_t)n ? 0 : -1;
}

int main(int argc, char **argv) {
    const char *dir = scratch_dir(argc, argv, "test_disk_guard");
    int rounds = argc > 2 ? atoi(argv[2]) : 12;
    char img[1024], to[1024];
    snprintf(img, sizeof img, "%s/disk.img", dir);
    snprintf(to, sizeof to, "%s/clone.img", dir);
    uint64_t total = OFFSET + SIZE;
    uint8_t *truth = malloc(total), *got = malloc(total);
    for (uint64_t k = 0; k < total; k++) truth[k] = (uint8_t)(k * 2654435761u >> 13);
    int fd = open(img, O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0 && pwrite(fd, truth, total, 0) == (ssize_t)total, "the image");
    close(fd);
    CHECK(hv_disk_open(&disk, img, OFFSET, 0) == 0 && disk.size == SIZE, "opened");
    pthread_t w[4];
    for (int i = 0; i < 4; i++) pthread_create(&w[i], NULL, writer, (void *)(intptr_t)i);

    uint64_t logged = 0;
    for (int r = 0; r < rounds && !failures; r++) {
        usleep(5000 + (r * 3001) % 20000);
        HvDiskGuard *g = hv_disk_guard_new(&disk, dir, 512ull << 20, 0);
        CHECK(g != NULL, "round %d: a guard", r);
        if (!g) break;
        pthread_rwlock_wrlock(&gate); /* the stop: no write under way */
        CHECK(read_file(img, truth, total) == 0, "round %d: the image at T", r);
        hv_disk_guard_arm(&disk, g);
        uint64_t log_at_t = nlog, writes_at_t = total_writes();
        __atomic_store_n(&epoch, r + 1, __ATOMIC_RELEASE);
        pthread_rwlock_unlock(&gate);
        usleep((r * 7919) % 30000);
        unlink(to);
        uint64_t ino = 0, size = 0;
        int64_t mt = 0;
        int rc = hv_disk_clone_guarded(&disk, to, &ino, &size, &mt);
        CHECK(rc == 0 && size == total && ino, "round %d: cloned: %d", r, rc);
        logged += hv_disk_guard_logged(g);
        g_last = g;
        CHECK(disk.guard == NULL, "round %d: ended", r);
        struct stat st;
        CHECK(stat(to, &st) == 0 && (uint64_t)st.st_ino == ino && st.st_mtimespec.tv_sec * 1000000000ll + st.st_mtimespec.tv_nsec == mt,
              "round %d: the identity is the clone's as it ended", r);
        CHECK(read_file(to, got, total) == 0, "round %d: read the clone", r);
        if (memcmp(got, truth, total) != 0) {
            uint64_t k = 0;
            while (got[k] == truth[k]) k++;
            uint64_t b = (k - OFFSET) / HV_DISK_GUARD_BLOCK, bad = 0, end = 0;
            for (uint64_t q = k; q < total; q++)
                if (got[q] != truth[q]) bad++, end = q;
            CHECK(0, "round %d: the clone isn't the disk at T (first difference at %llu, block %llu, +%llu; %llu bytes differ up to %llu; logged %d)", r,
                  (unsigned long long)k, (unsigned long long)b, (unsigned long long)((k - OFFSET) % HV_DISK_GUARD_BLOCK),
                  (unsigned long long)bad, (unsigned long long)end, hv_disk_guard_test_logged(g_last, b));
            static uint8_t ent[HV_DISK_GUARD_BLOCK];
            uint8_t *now = malloc(total);
            read_file(img, now, total);
            for (uint64_t q = b; q <= (end - OFFSET) / HV_DISK_GUARD_BLOCK; q++) {
                int k = hv_disk_guard_test_entry(g_last, q, ent);
                uint64_t at = OFFSET + q * HV_DISK_GUARD_BLOCK;
                int same_t = 0, same_clone = 0, same_now = 0;
                if (k >= 0) same_t = !memcmp(ent, truth + at, HV_DISK_GUARD_BLOCK), same_clone = !memcmp(ent, got + at, HV_DISK_GUARD_BLOCK),
                            same_now = !memcmp(ent, now + at, HV_DISK_GUARD_BLOCK);
                fprintf(stderr, "  block %llu: entry %d; log==T %d log==clone %d log==now %d; clone==now %d; T %02x.. clone %02x.. now %02x.. log %02x..\n",
                        (unsigned long long)q, k, same_t, same_clone, same_now, !memcmp(got + at, now + at, HV_DISK_GUARD_BLOCK),
                        truth[at], got[at], now[at], ent[0]);
            }
            free(now);
            uint64_t bo = (b * HV_DISK_GUARD_BLOCK);
            for (uint64_t q = nlog > LOGN ? nlog - LOGN : 0; q < nlog; q++) {
                WRec *w = &wlog[q % LOGN];
                if (w->off < bo + HV_DISK_GUARD_BLOCK && w->off + w->len > bo && (q + 2000 > log_at_t))
                    fprintf(stderr, "  write #%llu (T at #%llu) by %d: %llu+%llu (blocks %llu..%llu) value %02x epoch %u guarded %d\n", (unsigned long long)q,
                            (unsigned long long)log_at_t, w->id, (unsigned long long)w->off, (unsigned long long)w->len,
                            (unsigned long long)(w->off / HV_DISK_GUARD_BLOCK), (unsigned long long)((w->off + w->len - 1) / HV_DISK_GUARD_BLOCK), w->v,
                            w->epoch, w->guarded);
            }
        }
        /* a write done after T: each writer counts its write after letting go of the gate, so up to 4 counted after T
           were done before it */
        uint64_t since = writes_past(writes_at_t + 4) - writes_at_t;
        CHECK(read_file(img, got, total) == 0 && memcmp(got, truth, total) != 0,
              "round %d: (the disk was written since: %llu writes counted)", r, (unsigned long long)since);
        hv_disk_guard_free(&disk, g);
        g_last = NULL;
    }

    /* A log of 3 blocks: void, and the writers never stopped */
    {
        HvDiskGuard *g = hv_disk_guard_new(&disk, dir, 3 * HV_DISK_GUARD_BLOCK, 0);
        pthread_rwlock_wrlock(&gate);
        hv_disk_guard_arm(&disk, g);
        uint64_t before = total_writes();
        pthread_rwlock_unlock(&gate);
        /* Until 20 more writes are done (the log is full after the first few): a writer the full log held would never
           get there */
        uint64_t n = writes_past(before + 20);
        CHECK(n > before + 20, "the writers ran on with the log full: %llu writes in 10 s", (unsigned long long)(n - before));
        unlink(to);
        CHECK(hv_disk_clone_guarded(&disk, to, NULL, NULL, NULL) == 1, "a full log: void");
        CHECK(access(to, F_OK) != 0, "nothing left at the clone's path");
        hv_disk_guard_free(&disk, g);
    }
    /* Given up before the clone: ended by the free, the writers unaffected */
    {
        HvDiskGuard *g = hv_disk_guard_new(&disk, dir, 64ull << 20, 0);
        pthread_rwlock_wrlock(&gate);
        hv_disk_guard_arm(&disk, g);
        pthread_rwlock_unlock(&gate);
        usleep(10000);
        hv_disk_guard_free(&disk, g);
        CHECK(disk.guard == NULL, "given up: ended");
        CHECK(hv_disk_clone_guarded(&disk, to, NULL, NULL, NULL) == -1, "no guard: no guarded clone");
    }
    /* No room for a log at all */
    CHECK(hv_disk_guard_new(&disk, dir, 64ull << 20, UINT64_MAX / 2) == NULL, "no room: no guard");

    stop_writers = 1;
    for (int i = 0; i < 4; i++) pthread_join(w[i], NULL);
    hv_disk_close(&disk);
    if (failures) {
        printf("test_disk_guard: %d failures\n", failures);
        return 1;
    }
    scratch_done();
    printf("test_disk_guard: all passed (%d rounds, %llu writes, %llu blocks logged)\n", rounds,
           (unsigned long long)total_writes(), (unsigned long long)logged);
    return 0;
}
