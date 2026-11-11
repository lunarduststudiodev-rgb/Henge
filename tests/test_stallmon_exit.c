/* The freeze recorder (host/stallmon.c) while guest CPU threads end (as they do when the Mac shuts down, the
   recorder still sampling: host/vm.c hv_vm_finish) and new ones start: rounds of three "CPU" threads that register
   as the engine's do, run (busy or asleep) for a few milliseconds while the recorder samples them, and end. The
   recorder reads which CPU a thread runs through that thread's own thread-local memory (hv_self), which goes early
   in the thread's end: before its pthread key destructors run and the thread is gone. Each thread here lingers
   30 ms in such a destructor (as a thread preempted while it ends, under load, does for a moment): sampled then, a
   thread still in the list reads a stale value (0x4, a port name: seen as 0x113b and 0x124d in the crashes) and
   faults. So a thread must leave the list before its thread-local memory goes, and its place is then free for the
   next (12 places, else full by the fourth round).
   The recorder stops threads to sample them only while a stall could be forming (hv_stall_watch), so here a press
   waits for a screen that never changes, the whole time: it samples throughout (40 rounds, about 3.5 s, all at the
   full rate: HV_STALL_BUDGET_TICKS).

   Usage: test_stallmon_exit [folder] */
#include <dispatch/dispatch.h>
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
static _Atomic uint64_t pressed_ns; /* when the person pressed, 0 not yet */
static double since_press(void) {
    uint64_t p = atomic_load(&pressed_ns);
    return p ? (double)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - p) / 1e9 : 1e9;
}

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

#define ROUNDS 40
#define PER_ROUND 3
static HvCpu *cpus[PER_ROUND];
static _Atomic int started, go_home;
static pthread_key_t linger_key;

/* Runs as the thread ends, after its thread-local memory has gone (pthread key destructors do), the thread still
   there for the recorder to sample. */
static void linger(void *unused) {
    (void)unused;
    usleep(30000);
}

__attribute__((noinline)) static void work(int busy, unsigned us) {
    if (!busy) {
        usleep(us);
        return;
    }
    uint64_t until = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + us * 1000ull;
    while (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) < until) {
    }
}

static void *cpu_thread(void *arg) {
    int k = (int)(intptr_t)arg & 0xff, round = (int)(intptr_t)arg >> 8;
    hv_prof_register(); /* as host/vm.c hv_vm_prepare_cpu_thread does */
    hv_self = cpus[k];
    pthread_setspecific(linger_key, (void *)1);
    atomic_fetch_add(&started, 1);
    while (!atomic_load(&go_home)) work(0, 500); /* all three are in the recorder's list together */
    /* Busy (sampled where it runs) or asleep (seen as idle), for 2..17 ms: some end while being sampled. */
    cpus[k]->pc_state->active = (k + round) & 1;
    work((k + round) & 1, 2000 + (unsigned)((k * 7 + round * 5) % 16) * 1000);
    return NULL;
}

static void *scenario(void *unused) {
    (void)unused;
    usleep(100000); /* the main thread registers (its first timed task) */
    atomic_store(&pressed_ns, clock_gettime_nsec_np(CLOCK_UPTIME_RAW)); /* and the screen never answers */
    usleep(100000);
    int base = hv_stall_threads();
    CHECK(base == 1, "before any CPU thread: %d threads listed, want 1 (main)", base);
    for (int round = 0; round < ROUNDS; round++) {
        pthread_t t[PER_ROUND];
        atomic_store(&started, 0), atomic_store(&go_home, 0);
        for (int k = 0; k < PER_ROUND; k++) pthread_create(&t[k], NULL, cpu_thread, (void *)(intptr_t)(round << 8 | k));
        while (atomic_load(&started) < PER_ROUND) usleep(200);
        usleep(25000); /* sampled a few times each */
        int live = hv_stall_threads();
        CHECK(live == base + PER_ROUND, "round %d: %d threads listed, want %d (old ones kept, or no room for new)",
              round, live, base + PER_ROUND);
        atomic_store(&go_home, 1);
        for (int k = 0; k < PER_ROUND; k++) pthread_join(t[k], NULL);
        usleep(20000); /* the recorder samples on: it must not look at them again */
        CHECK(hv_stall_threads() == base, "round %d: %d threads listed after they ended, want %d", round,
              hv_stall_threads(), base);
    }
    CHECK(hv_stall_records() >= 1, "the press was never seen waiting (no hang recorded): nothing was sampled");
    scratch_done();
    if (failures) {
        fprintf(stderr, "test_stallmon_exit: %d failures\n", failures);
        exit(1);
    }
    printf("test_stallmon_exit: ok (%d rounds of %d threads)\n", ROUNDS, PER_ROUND);
    exit(0);
}

int main(int argc, char **argv) {
    pthread_key_create(&linger_key, linger);
    const char *dir = scratch_dir(argc, argv, "stallmon_exit");
    static uint32_t code[1024];
    machine.jit.base = code;
    machine.jit.capacity = 1024;
    for (int i = 0; i < PER_ROUND; i++) {
        cpus[i] = zalloc(_Alignof(HvCpu), sizeof(HvCpu));
        cpus[i]->index = (uint32_t)i;
        cpus[i]->pc_state = zalloc(_Alignof(HvPerCpu), sizeof(HvPerCpu));
    }
    HvStallHooks h = {.running = running, .input_idle_s = since_press, .press_idle_s = since_press};
    hv_stall_start(dir, &h);
    pthread_t t;
    pthread_create(&t, NULL, scenario, NULL);
    dispatch_main(); /* the main thread runs its queue, as the app's does */
}
