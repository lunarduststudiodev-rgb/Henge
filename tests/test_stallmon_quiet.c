/* The freeze recorder (host/stallmon.c) stops a thread to read it only while a stall could be forming
   (hv_stall_watch): stopping a guest CPU's thread can hold up every other (a spinlock it holds, a TLBI ack another
   waits for), and it used to stop each listed thread 100 times a second whatever happened. Here, on real threads
   with the machine around them made up (as tests/test_stallmon_live.c): two busy "guest CPU" threads and the main
   thread running its queue.
   - Frames on time for a second: no thread is stopped (it was about 300 times).
   - The frames stop for 300 ms: the threads are sampled (a stall is still recorded with what they did).
   - The frames come again: once it has seen them, no more stops.
   The frame count follows the clock, so a test thread late off its core can't make a frame late; the main thread's
   timed task can still wait under load, so a few stops (three ticks' worth) are let pass in the quiet stretches.

   Usage: test_stallmon_quiet [folder] */
#include "../host/stallmon.c"
#include "scratch_dir.h"
#include "zalloc.h"

__thread HvCpu *hv_self;
void (*hv_prof_register)(void);
static HvMachine machine;
static _Atomic int frames_on = 1, done;
static _Atomic uint64_t frozen;
HvMachine *hv_vm_machine(void) { return &machine; }
int hv_vm_paused(void) { return 0; }
uint64_t hv_vm_gpu_frames(void) { /* one every 16 ms while on */
    return atomic_load(&frames_on) ? mach_absolute_time() * tb.numer / tb.denom / 16000000 : atomic_load(&frozen);
}
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

enum { NCPU = 2, SLACK = 3 * (NCPU + 1) };
static HvCpu *cpus[NCPU];

static void *cpu_thread(void *arg) {
    int i = (int)(intptr_t)arg;
    hv_prof_register(); /* as host/vm.c hv_vm_prepare_cpu_thread does */
    hv_self = cpus[i];
    cpus[i]->pc_state->active = 1; /* running guest code, as far as the recorder can tell */
    volatile uint64_t x = 0;
    while (!atomic_load(&done)) x++;
    return NULL;
}

static unsigned stops(void) { return atomic_load(&suspends); }

static void *scenario(void *unused) {
    (void)unused;
    pthread_t t;
    for (int i = 0; i < NCPU; i++) pthread_create(&t, NULL, cpu_thread, (void *)(intptr_t)i);
    usleep(300000); /* registered, frames flowing */
    CHECK(hv_stall_threads() == NCPU + 1, "%d threads listed, want %d", hv_stall_threads(), NCPU + 1);

    unsigned s0 = stops();
    usleep(1000000);
    unsigned quiet = stops() - s0;
    CHECK(quiet <= SLACK, "frames on time for a second: %u threads stopped, want none", quiet);

    atomic_store(&frozen, hv_vm_gpu_frames());
    atomic_store(&frames_on, 0);
    s0 = stops();
    usleep(300000);
    unsigned stalled = stops() - s0;
    atomic_store(&frames_on, 1);
    CHECK(stalled >= 10 * NCPU, "frames stopped for 300 ms: %u threads stopped, want them sampled", stalled);

    usleep(50000); /* it sees the frames again */
    s0 = stops();
    usleep(500000);
    unsigned after = stops() - s0;
    CHECK(after <= SLACK, "frames back: %u threads stopped, want none", after);
    printf("stops: %u in a second of steady frames, %u in a 300 ms stall, %u after\n", quiet, stalled, after);

    atomic_store(&done, 1);
    scratch_done();
    printf("%s\n", failures ? "test_stallmon_quiet FAILED" : "test_stallmon_quiet ok");
    exit(failures != 0);
}

int main(int argc, char **argv) {
    const char *dir = scratch_dir(argc, argv, "stallmon_quiet");
    static uint32_t code[1024];
    machine.jit.base = code;
    machine.jit.capacity = 1024;
    for (int i = 0; i < NCPU; i++) {
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
