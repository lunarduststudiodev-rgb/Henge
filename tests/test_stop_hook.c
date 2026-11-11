/* The dispatcher's stop, moved while CPUs run (core/machine.h: hv_stop_publish, the stores hv_machine_set_stop makes
   under its lock; hv_at_stop and hv_stop_hook, what a CPU reads, a seqlock's reader). host/main.c's debug hooks move
   it from the watch thread and from a CPU: one hook at one place, then another hook at another. A CPU that matches a
   place must get that place's hook: never none where there is a hook (NULL is "halt here"), never another place's,
   and a hook where the stop is a halt.
   A stop for CPU 0 only (E, a halt: --break's) is one the other CPUs never meet: a CPU that read E's pc with the CPUs
   of another stop (D's: every CPU) would halt there.
   The mover goes in three parts. First the race: RUN_NS of moves through the stops as fast as they go, A, B, A while a
   CPU reads (the ABA it makes thousands of times). Then each place in turn, held until a reader has met it, so every
   place is checked however a loaded Mac schedules the readers. Then the stop set again and again at A, for RUN_NS / 3
   and on until the readers have met it there SAME_MEETS times: a CPU there must meet it every time, as it is at A
   before and after every move, and a reader that takes a move under way for "not here" misses it. Nothing waits on a
   clock but the race and that minimum; all of it gives up, and fails, after GIVE_UP_NS.
   What it catches, as shipped (test_stop_hook, 10 runs each) and under ThreadSanitizer (test_stop_hook_tsan, which
   fails on any plain access, 2 runs each): plain stores and loads, 10 of 10 and 2 of 2; a reader that does not try
   again when the stop moved under it, 10 and 2; one that does not check the pc again inside the seqlock, 10 and 2; one
   missing its fence, 10 and 1 (ThreadSanitizer does not model fences). A reader whose first stop_seq load is relaxed,
   not acquire, seldom fails as shipped (1 run of 10 unloaded, 2 of 140 under load) and never under ThreadSanitizer:
   this test does not catch it, and only review does. Nor does it catch the writer missing its fence or release: they
   are the seqlock's rule, and this Mac's cores never showed one missing (0 runs of 10 each). */
#include "machine.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

enum { READERS = 3, PLACES = 5, SAME_MEETS = 1000 };
#define RUN_NS 300000000ull /* 0.3 s of moves: enough for a missing barrier to show on the build as shipped */
#define GIVE_UP_NS 20000000000ull /* the readers still not having met everything after 20 s: fail, not wait for ever */
#define A 0x1000ull
#define B 0x2000ull
#define C 0x3000ull /* B's second place (stop_pc2) */
#define D 0x4000ull /* a halt: no hook */
#define E 0x5000ull /* a halt for CPU 0 only */

static HvMachine m;
static int ready, done, same; /* same: the mover now only sets A again */
static unsigned long long met[PLACES], again_all; /* how often each place, and A set again, was met: for the mover */
static uint64_t start;
static void hook_a(HvCpu *cpu) { (void)cpu; }
static void hook_b(HvCpu *cpu) { (void)cpu; }
static const uint64_t at[PLACES] = {A, B, C, D, E};
static void (*const want[PLACES])(HvCpu *) = {hook_a, hook_b, hook_b, NULL, NULL};
static const uint32_t cpus[PLACES] = {0, 0, 0, 0, 1u << 0}; /* who meets each place: 0, every CPU */

static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static int given_up(void) { return now_ns() - start > GIVE_UP_NS; }

/* Moves the stop to the one that has place k */
static void put(int k) {
    switch (k) {
    case 0: hv_stop_publish(&m, A, ~0ull, hook_a, 0); break;
    case 1: case 2: hv_stop_publish(&m, B, C, hook_b, 0); break;
    case 3: hv_stop_publish(&m, D, ~0ull, NULL, 0); break;
    default: hv_stop_publish(&m, E, ~0ull, NULL, cpus[4]); break;
    }
}

static void *mover(void *unused) {
    (void)unused;
    while (__atomic_load_n(&ready, __ATOMIC_ACQUIRE) < READERS) {}
    start = now_ns();
    /* The race: RUN_NS of moves through the four stops, as fast as they go. */
    static const int stops[4] = {0, 1, 3, 4};
    for (int i = 0; (i & 1023) || now_ns() - start < RUN_NS; i++) put(stops[i % 4]);
    /* Every place met, however the readers are scheduled (a loaded Mac may give them no core while the stops race
       past): each in turn, held until a reader has met it. */
    for (int k = 0; k < PLACES; k++) {
        unsigned long long before = __atomic_load_n(&met[k], __ATOMIC_RELAXED);
        put(k);
        while (__atomic_load_n(&met[k], __ATOMIC_RELAXED) == before && !given_up()) sched_yield();
    }
    /* A set again and again: for RUN_NS / 3, and on until the readers have met it there SAME_MEETS times. */
    put(0);
    __atomic_store_n(&same, 1, __ATOMIC_RELEASE);
    uint64_t same_from = now_ns();
    for (int i = 1;; i++) {
        put(0);
        if (i & 1023) continue;
        if ((now_ns() - same_from > RUN_NS / 3 && __atomic_load_n(&again_all, __ATOMIC_RELAXED) >= SAME_MEETS) ||
            given_up())
            break;
        sched_yield();
    }
    __atomic_store_n(&done, 1, __ATOMIC_RELEASE);
    return NULL;
}

typedef struct { uint32_t index; unsigned long long hits[PLACES], none, wrong, foreign, again, missed; } Seen;

static void *cpu(void *arg) {
    Seen *s = arg;
    __atomic_add_fetch(&ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&done, __ATOMIC_ACQUIRE)) {
        if (__atomic_load_n(&same, __ATOMIC_ACQUIRE)) {
            HvStopAt stop = {0, NULL};
            if (hv_at_stop(&m, A)) stop = hv_stop_hook(&m, A, s->index);
            if (!stop.here) s->missed++;
            else if (stop.hook != hook_a) s->wrong++;
            else s->again++, __atomic_add_fetch(&again_all, 1, __ATOMIC_RELAXED);
            continue;
        }
        for (int k = 0; k < PLACES; k++) {
            if (!hv_at_stop(&m, at[k])) continue;
            HvStopAt stop = hv_stop_hook(&m, at[k], s->index);
            if (!stop.here) continue;
            if (cpus[k] && !(cpus[k] >> s->index & 1)) s->foreign++;
            else if (stop.hook == want[k]) s->hits[k]++, __atomic_add_fetch(&met[k], 1, __ATOMIC_RELAXED);
            else if (!stop.hook) s->none++;
            else s->wrong++;
        }
    }
    return NULL;
}

int main(void) {
    m.stop_pc = m.stop_pc2 = ~0ull;
    pthread_t w, r[READERS];
    Seen seen[READERS];
    memset(seen, 0, sizeof seen);
    for (int i = 0; i < READERS; i++) seen[i].index = (uint32_t)i;
    for (int i = 0; i < READERS; i++) pthread_create(&r[i], NULL, cpu, &seen[i]);
    pthread_create(&w, NULL, mover, NULL);
    pthread_join(w, NULL);
    unsigned long long hits[PLACES] = {0}, none = 0, wrong = 0, foreign = 0, again = 0, missed = 0;
    for (int i = 0; i < READERS; i++) {
        pthread_join(r[i], NULL);
        for (int k = 0; k < PLACES; k++) hits[k] += seen[i].hits[k];
        none += seen[i].none, wrong += seen[i].wrong, foreign += seen[i].foreign, again += seen[i].again, missed += seen[i].missed;
    }
    int failures = 0;
    printf("stops met at A %llu, B %llu, C %llu, D (halt) %llu, E (CPU 0's halt) %llu; with no hook %llu, with a wrong "
           "one %llu, by another CPU %llu\n", hits[0], hits[1], hits[2], hits[3], hits[4], none, wrong, foreign);
    if (none) printf("FAIL a CPU met a stop without its hook: it would halt\n"), failures++;
    if (wrong) printf("FAIL a CPU met a stop with another place's hook\n"), failures++;
    if (foreign) printf("FAIL a CPU met a stop that is for another CPU: it would halt\n"), failures++;
    for (int k = 0; k < PLACES; k++)
        if (!hits[k]) printf("FAIL place %c never met with its hook: the test checked nothing there\n", 'A' + k), failures++;
    printf("the stop set again at A: met %llu times, missed %llu\n", again, missed);
    if (missed) printf("FAIL a CPU missed a stop that was at its place before and after each move\n"), failures++;
    if (again < SAME_MEETS)
        printf("FAIL the stop set again at A was met only %llu times in 20 s: the test checked too little\n", again),
            failures++;
    printf("%s\n", failures ? "FAILED" : "ok");
    return failures != 0;
}
