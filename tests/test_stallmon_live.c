/* The freeze recorder (host/stallmon.c) on real threads, with the machine around it made up: two "guest CPU" threads
   that register as the engine's do, a frame counter, and the main thread running its queue. The frames stop for
   400 ms while cpu1 waits on a lock and cpu0 works in host code; later the main thread is kept busy for 500 ms. The
   records must say so: a hitch with cpu1 waiting in stuck_on_lock and cpu0 in burn, and an app stall with the main
   thread in main_busy. This is what reading other threads' stacks has to get right on the device.

   Usage: test_stallmon_live [folder] */
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../core/cpu.h"
#include "../core/machine.h"
#include "../host/stallmon.h"
#include "../host/vm.h"
#include "scratch_dir.h"
#include "zalloc.h"

/* What the engine would provide. */
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

static _Atomic int done, frames_on = 1, cpu0_burn, cpu1_stall;
static pthread_mutex_t held = PTHREAD_MUTEX_INITIALIZER;
static HvCpu *cpus[2];

__attribute__((noinline, used)) void burn(void) {
    volatile uint64_t x = 0;
    for (int i = 0; i < 2000000; i++) x += (uint64_t)i;
}

__attribute__((noinline, used)) void stuck_on_lock(void) {
    pthread_mutex_lock(&held);
    pthread_mutex_unlock(&held);
}

static volatile int main_busy_done;
__attribute__((noinline, used)) void main_busy(void *unused) {
    (void)unused;
    usleep(500000);
    main_busy_done = 1; /* not a tail call: main_busy stays on the stack while it sleeps */
}

static void *cpu_thread(void *arg) {
    int i = (int)(intptr_t)arg;
    hv_prof_register(); /* as host/vm.c hv_vm_prepare_cpu_thread does */
    hv_self = cpus[i];
    HvPerCpu *p = cpus[i]->pc_state;
    while (!atomic_load(&done)) {
        if (i == 0 && atomic_load(&cpu0_burn)) {
            p->active = 1;
            burn();
        } else if (i == 1 && atomic_load(&cpu1_stall)) {
            p->active = 1;
            stuck_on_lock();
            atomic_store(&cpu1_stall, 0);
        } else { /* asleep in WFI, as far as the recorder can tell */
            p->active = 0;
            usleep(2000);
        }
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

/* Is there an "app+0x..." label in s that falls in fn's first 512 bytes? */
static int names_fn(const char *s, void *fn) {
    Dl_info di;
    if (!dladdr(fn, &di)) return 0;
    uint64_t lo = (uint64_t)(uintptr_t)fn - (uint64_t)(uintptr_t)di.dli_fbase;
    for (const char *p = strstr(s, "app+0x"); p; p = strstr(p + 1, "app+0x")) {
        uint64_t off = strtoull(p + 6, NULL, 16);
        if (off >= lo && off < lo + 512) return 1;
    }
    return 0;
}

/* The count after "key": inside the named thread's part of the line. */
static unsigned thread_count(const char *line, const char *thread, const char *key) {
    char name[64], k[64];
    snprintf(name, sizeof name, "\"name\":\"%s\"", thread);
    snprintf(k, sizeof k, "\"%s\":", key);
    const char *t = strstr(line, name);
    const char *v = t ? strstr(t, k) : NULL;
    return v ? (unsigned)strtoul(v + strlen(k), NULL, 10) : 0;
}

/* The thread's part of the line, as its own string. */
static char *thread_part(const char *line, const char *thread) {
    char name[64];
    snprintf(name, sizeof name, "{\"name\":\"%s\"", thread);
    const char *t = strstr(line, name);
    if (!t) return strdup("");
    const char *end = strstr(t + 1, "{\"name\":");
    return strndup(t, end ? (size_t)(end - t) : strlen(t));
}

static const char *dir;

static void *scenario(void *unused) {
    (void)unused;
    pthread_t t;
    for (int i = 0; i < 2; i++) pthread_create(&t, NULL, cpu_thread, (void *)(intptr_t)i);
    pthread_create(&t, NULL, frame_thread, NULL);
    usleep(1000000); /* frames flowing, CPUs asleep */

    pthread_mutex_lock(&held);
    atomic_store(&cpu1_stall, 1);
    atomic_store(&cpu0_burn, 1);
    atomic_store(&frames_on, 0);
    usleep(400000);
    atomic_store(&frames_on, 1);
    atomic_store(&cpu0_burn, 0);
    pthread_mutex_unlock(&held);
    usleep(600000);

    dispatch_async_f(dispatch_get_main_queue(), NULL, main_busy);
    usleep(1500000);
    atomic_store(&done, 1);
    usleep(50000);

    char path[1200];
    snprintf(path, sizeof path, "%s/henge-freezes.jsonl", dir);
    FILE *f = fopen(path, "r");
    CHECK(f, "no freeze log at %s", path);
    static char line[1 << 16];
    int hitches = 0, apps = 0;
    while (f && fgets(line, sizeof line, f)) {
        if (strstr(line, "\"kind\":\"hitch\"")) {
            hitches++;
            char *c0 = thread_part(line, "cpu0"), *c1 = thread_part(line, "cpu1");
            CHECK(thread_count(line, "cpu1", "waiting") >= 10, "cpu1 waiting: %s", line);
            CHECK(names_fn(c1, (void *)stuck_on_lock), "cpu1's stack doesn't show stuck_on_lock: %s", c1);
            CHECK(thread_count(line, "cpu0", "engine") >= 10, "cpu0 working: %s", line);
            CHECK(names_fn(c0, (void *)burn), "cpu0's stack doesn't show burn: %s", c0);
            CHECK(strstr(line, "\"ms\":4") || strstr(line, "\"ms\":3"), "hitch length: %s", line);
            free(c0), free(c1);
        } else if (strstr(line, "\"kind\":\"app\"")) {
            apps++;
            char *mt = thread_part(line, "main");
            CHECK(strstr(line, "\"verdict\":\"app\""), "app verdict: %s", line);
            CHECK(names_fn(mt, (void *)main_busy), "the main thread's stack doesn't show main_busy: %s", mt);
            free(mt);
        } else {
            CHECK(0, "unexpected record: %s", line);
        }
    }
    if (f) fclose(f);
    CHECK(hitches == 1, "%d hitches", hitches);
    CHECK(apps == 1, "%d app stalls", apps);
    CHECK(hv_stall_records() == hitches + apps, "hv_stall_records %d", hv_stall_records());
    scratch_done();
    if (failures) {
        fprintf(stderr, "test_stallmon_live: %d failures\n", failures);
        exit(1);
    }
    printf("test_stallmon_live: ok\n");
    exit(0);
}

int main(int argc, char **argv) {
    dir = scratch_dir(argc, argv, "stallmon");
    static uint32_t code[1024];
    machine.jit.base = code;
    machine.jit.capacity = 1024;
    for (int i = 0; i < 2; i++) {
        cpus[i] = zalloc(_Alignof(HvCpu), sizeof(HvCpu));
        cpus[i]->index = (uint32_t)i;
        cpus[i]->pc_state = zalloc(_Alignof(HvPerCpu), sizeof(HvPerCpu));
    }
    HvStallHooks h = {.running = running, .input_idle_s = no_input, .press_idle_s = no_press};
    hv_stall_start(dir, &h);
    pthread_t t;
    pthread_create(&t, NULL, scenario, NULL);
    dispatch_main(); /* the main thread runs its queue, as the app's does */
}
