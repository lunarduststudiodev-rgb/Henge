/* The freeze recorder (host/stallmon.c) tells apart what a blocked or busy guest CPU thread is doing, on real
   threads with the machine around them made up (as tests/test_stallmon_live.c):
   - in the in-place WFE wait (dispatch.c wfe sets pc_state->wfe_waiting): a guest spin, macOS's CPUs waiting on
     each other's spinlocks; it was counted as Henge's own code ("engine");
   - held in page faults (reading a file mapping whose pages were just dropped): a fault, memory; it was counted as
     a lock wait ("waiting"), so memory pressure looked like Henge's locks;
   - blocked on a mutex: still a wait.
   The frames stop for 400 ms twice: once while cpu0 spins, once while cpu1 faults and cpu2 waits.

   Usage: test_stallmon_kinds [folder] */
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../core/cpu.h"
#include "../core/machine.h"
#include "../host/stallmon.h"
#include "../host/vm.h"
#include "scratch_dir.h"
#include "zalloc.h"

__thread HvCpu *hv_self;
void (*hv_prof_register)(void);
static HvMachine machine;
static _Atomic uint64_t frames;
HvMachine *hv_vm_machine(void) { return &machine; }
int hv_vm_paused(void) { return 0; }
uint64_t hv_vm_gpu_frames(void) { return atomic_load(&frames); }
void hv_vm_gfx_stages(uint32_t ms[5]) { memset(ms, 0, 5 * sizeof *ms); }
static int running(void) { return 1; }
static double no_input(void) { return -1; }
static double no_press(void) { return 1e9; }

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

enum { NCPU = 3 };
static HvCpu *cpus[NCPU];
static _Atomic int done, frames_on = 1, spin, fault, wait_lock;
static pthread_mutex_t held = PTHREAD_MUTEX_INITIALIZER;
#define MAP_LEN (64u << 20)
static volatile uint8_t *map;

/* As dispatch.c wfe: the flag up, then LDAXR/WFE on a word nobody writes */
__attribute__((noinline, used)) static void wfe_wait(HvPerCpu *p) {
    static uint32_t word;
    __atomic_store_n(&p->wfe_waiting, 1, __ATOMIC_RELAXED);
    while (atomic_load(&spin)) {
        uint32_t w;
        __asm__ volatile("ldaxr %w0, [%1]" : "=r"(w) : "r"(&word) : "memory");
        if (w) break;
        __asm__ volatile("wfe" ::: "memory");
    }
    __asm__ volatile("clrex" ::: "memory");
    __atomic_store_n(&p->wfe_waiting, 0, __ATOMIC_RELAXED);
}

/* Guest RAM the host has let go of: every page comes back from the file */
__attribute__((noinline, used)) static void fault_in(void) {
    volatile uint64_t sum = 0;
    while (atomic_load(&fault))
        for (size_t i = 0; i < MAP_LEN && atomic_load(&fault); i += 16384) sum += map[i];
}

/* Drops the mapping's pages while cpu1 reads them, from its own thread: an msync on cpu1 would be a call to the
   kernel, rightly sampled as waiting. */
static void *invalidator(void *arg) {
    (void)arg;
    while (!atomic_load(&done)) {
        if (atomic_load(&fault)) msync((void *)map, MAP_LEN, MS_INVALIDATE);
        else usleep(1000);
    }
    return NULL;
}

/* As dispatch.c marks a CPU running or parked */
static void set_active(HvPerCpu *p, int on) { __atomic_store_n(&p->active, on, __ATOMIC_SEQ_CST); }

static void *cpu_thread(void *arg) {
    int i = (int)(intptr_t)arg;
    hv_prof_register();
    hv_self = cpus[i];
    HvPerCpu *p = cpus[i]->pc_state;
    while (!atomic_load(&done)) {
        if (i == 0 && atomic_load(&spin)) set_active(p, 1), wfe_wait(p);
        else if (i == 1 && atomic_load(&fault)) set_active(p, 1), fault_in();
        else if (i == 2 && atomic_load(&wait_lock)) {
            set_active(p, 1);
            pthread_mutex_lock(&held);
            pthread_mutex_unlock(&held);
            atomic_store(&wait_lock, 0);
        } else set_active(p, 0), usleep(2000);
    }
    return NULL;
}

static void *frame_thread(void *unused) {
    (void)unused;
    while (!atomic_load(&done)) {
        if (atomic_load(&frames_on)) atomic_fetch_add(&frames, 1);
        usleep(16000);
    }
    return NULL;
}

static unsigned thread_count(const char *line, const char *thread, const char *key) {
    char name[64], k[64];
    snprintf(name, sizeof name, "\"name\":\"%s\"", thread);
    snprintf(k, sizeof k, "\"%s\":", key);
    const char *t = strstr(line, name);
    const char *v = t ? strstr(t, k) : NULL;
    return v ? (unsigned)strtoul(v + strlen(k), NULL, 10) : 0;
}

static const char *dir;

static void stall(_Atomic int *a, _Atomic int *b) {
    atomic_store(a, 1);
    if (b) atomic_store(b, 1);
    atomic_store(&frames_on, 0);
    usleep(400000);
    atomic_store(&frames_on, 1);
    atomic_store(a, 0);
    if (b) atomic_store(b, 0);
    usleep(1200000); /* past the recorder's one record a second */
}

static void *scenario(void *unused) {
    (void)unused;
    pthread_t t;
    for (int i = 0; i < NCPU; i++) pthread_create(&t, NULL, cpu_thread, (void *)(intptr_t)i);
    pthread_create(&t, NULL, frame_thread, NULL);
    pthread_create(&t, NULL, invalidator, NULL);
    usleep(1000000);

    stall(&spin, NULL);
    pthread_mutex_lock(&held);
    atomic_store(&wait_lock, 1);
    stall(&fault, NULL);
    pthread_mutex_unlock(&held);
    atomic_store(&done, 1);
    usleep(50000);

    char path[1200];
    snprintf(path, sizeof path, "%s/henge-freezes.jsonl", dir);
    FILE *f = fopen(path, "r");
    CHECK(f, "no freeze log at %s", path);
    static char line[1 << 16];
    int hitches = 0;
    while (f && fgets(line, sizeof line, f)) {
        if (!strstr(line, "\"kind\":\"hitch\"")) {
            CHECK(0, "unexpected record: %s", line);
            continue;
        }
        if (++hitches == 1) {
            CHECK(strstr(line, "\"verdict\":\"guest_spin\""), "a guest CPU in the in-place wait is a guest spin: %s", line);
            CHECK(thread_count(line, "cpu0", "guest_spin") >= 10, "cpu0 spin samples: %s", line);
            CHECK(thread_count(line, "cpu0", "engine") == 0, "cpu0 counted as engine: %s", line);
        } else {
            unsigned f1 = thread_count(line, "cpu1", "fault"), w1 = thread_count(line, "cpu1", "waiting");
            CHECK(f1 >= 10 && f1 > 4 * w1, "cpu1 in page faults: fault %u, waiting %u: %s", f1, w1, line);
            CHECK(thread_count(line, "cpu2", "waiting") >= 10, "cpu2 on a mutex is waiting: %s", line);
            CHECK(thread_count(line, "cpu2", "fault") == 0, "cpu2 on a mutex counted as a fault: %s", line);
        }
    }
    if (f) fclose(f);
    CHECK(hitches == 2, "%d hitches", hitches);
    scratch_done();
    if (failures) {
        fprintf(stderr, "test_stallmon_kinds: %d failures\n", failures);
        exit(1);
    }
    printf("test_stallmon_kinds: ok\n");
    exit(0);
}

int main(int argc, char **argv) {
    dir = scratch_dir(argc, argv, "stallmon_kinds");
    char path[1200];
    snprintf(path, sizeof path, "%s/guest-ram", dir);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return 1;
    fcntl(fd, F_NOCACHE, 1);
    static uint8_t chunk[1 << 20];
    for (size_t i = 0; i < sizeof chunk; i++) chunk[i] = (uint8_t)(i * 2654435761u >> 24);
    for (size_t o = 0; o < MAP_LEN; o += sizeof chunk)
        if (write(fd, chunk, sizeof chunk) != (ssize_t)sizeof chunk) return 1;
    fsync(fd);
    map = mmap(NULL, MAP_LEN, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    unlink(path);
    if (map == MAP_FAILED) return 1;

    static uint32_t code[1024];
    machine.jit.base = code;
    machine.jit.capacity = 1024;
    for (int i = 0; i < NCPU; i++) {
        cpus[i] = zalloc(_Alignof(HvCpu), sizeof(HvCpu));
        cpus[i]->index = (uint32_t)i;
        cpus[i]->pc_state = zalloc(_Alignof(HvPerCpu), sizeof(HvPerCpu)); /* its counters are on 128-byte lines */
    }
    HvStallHooks h = {.running = running, .input_idle_s = no_input, .press_idle_s = no_press};
    hv_stall_start(dir, &h);
    pthread_t t;
    pthread_create(&t, NULL, scenario, NULL);
    dispatch_main();
}
