/* The freeze recorder's sample (host/stallmon.c) never calls a thread that woke while it was being looked at a
   fault. A sample reads the thread's state, then stops it to read its PC. A thread seen blocked that wakes before
   the stop is stopped in its own code, which is how a page fault looks; looking at its state again once it is let
   go (as the recorder did) only narrows that: it may have blocked again by then. The thread's CPU time tells: it is
   unchanged from the first look to the stop only if the thread never woke.

   The race is made to happen every time, on a semaphore, through the recorder's test seam (HV_STALL_LOOK_HOOK):
   - woken between the first look and the stop, running its own code at the stop, and blocked again once let go,
     at every look: never a fault (it was, every time, before the CPU-time check), and no sample at all: at the
     fresh look too it woke, and its own code then can't be told from a new page fault there;
   - the same once: the recorder looks again, fresh, and finds it blocked where it is now, on the semaphore:
     waiting (blocked in a new page fault instead, this is what finds the fault);
   - woken once, and still in its own code at the fresh look: running there, so in Henge's code;
   - blocked all along, on the semaphore: waiting.
   Then, as it happens by chance: a thread that sleeps and wakes all the time, sampled 8000 times, is never a fault.
   No check here counts on how busy the Mac is. */
#include <mach/semaphore.h>
#include <mach/task.h>

static void look_hook(int where);
#define HV_STALL_LOOK_HOOK(where) look_hook(where)
#include "../host/stallmon.c"

__thread HvCpu *hv_self;
void (*hv_prof_register)(void);
static HvMachine machine;
HvMachine *hv_vm_machine(void) { return &machine; }
int hv_vm_paused(void) { return 0; }
uint64_t hv_vm_gpu_frames(void) { return 0; }
void hv_vm_gfx_stages(uint32_t ms[5]) { memset(ms, 0, 5 * sizeof *ms); }

static int failures;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            failures++;                                                                                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

enum { RACES = 200, SAMPLES = 8000 };
static semaphore_t sem;
static _Atomic int slot = -1, stop, waits, in_code, back;
/* the hook wakes the thread: 1 at every look, 2 at the first only, 3 at the first only and leaves it in its own code
   (0: it does nothing) */
static _Atomic int race;

/* Its own CPU time, in nanoseconds: not wall-clock time, which a busy Mac stretches */
static uint64_t own_cpu_ns(void) { return clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID); }

static void *sleeper(void *unused) {
    (void)unused;
    atomic_store(&slot, add_thread(NULL, "sleeper"));
    while (!atomic_load(&stop)) {
        atomic_fetch_add(&waits, 1);
        semaphore_wait(sem);
        /* 10 us of its own code, then wait in it (not in the kernel) to be sent back */
        for (uint64_t end = own_cpu_ns() + 10000; own_cpu_ns() < end;) {}
        atomic_store(&in_code, 1);
        while (!atomic_load(&back) && !atomic_load(&stop)) {}
        atomic_store(&back, 0);
        atomic_store(&in_code, 0);
    }
    return NULL;
}

static void *napper(void *unused) {
    (void)unused;
    atomic_store(&slot, add_thread(NULL, "napper"));
    while (!atomic_load(&stop)) {
        usleep(5);
        for (uint64_t end = own_cpu_ns() + 5000; own_cpu_ns() < end;) {}
    }
    return NULL;
}

/* Until the sleeper has asked to wait n times and is blocked (not running, nor ready to) */
static void until_blocked(int n) {
    int running = 1;
    uint64_t t;
    while (atomic_load(&waits) < n || (thread_now(threads[atomic_load(&slot)].port, &running, &t) && running)) {}
}

static void look_hook(int where) {
    int r = atomic_load(&race);
    if (!r) return;
    if (where == 0) { /* seen blocked: wake it, and let it run its own code before it is stopped */
        semaphore_signal(sem);
        while (!atomic_load(&in_code)) {}
    } else if (r == 3) { /* let go: it runs on in its own code */
        atomic_store(&race, 0);
    } else { /* let go: it blocks again, so a second look at its state finds it blocked too */
        int n = atomic_load(&waits);
        atomic_store(&back, 1);
        until_blocked(n + 1);
        if (r == 2) atomic_store(&race, 0);
    }
}

static unsigned kinds[HV_SAMPLE_KINDS];
static unsigned sample_once(void) {
    uint32_t before = ring_head;
    sample(atomic_load(&slot), &machine, now_us());
    if (ring_head == before) return HV_SAMPLE_KINDS;
    unsigned what = ring[before % RING].what;
    kinds[what]++;
    return what;
}

int main(void) {
    mach_timebase_info(&tb);
    static uint32_t code[64];
    machine.jit.base = code, machine.jit.capacity = 64;
    if (semaphore_create(mach_task_self(), &sem, SYNC_POLICY_FIFO, 0) != KERN_SUCCESS) return 1;
    pthread_t t;
    pthread_create(&t, NULL, sleeper, NULL);
    while (atomic_load(&slot) < 0) usleep(1000);

    unsigned woke_faults = 0, woke_none = 0, still_waiting = 0, once_waiting = 0, once_faults = 0, ran_engine = 0;
    for (int k = 0; k < RACES; k++) {
        until_blocked(atomic_load(&waits));
        atomic_store(&race, 1);
        unsigned before = atomic_load(&skipped);
        unsigned what = sample_once();
        atomic_store(&race, 0);
        woke_faults += what == HV_SAMPLE_FAULT, woke_none += what == HV_SAMPLE_KINDS && atomic_load(&skipped) == before + 1;

        until_blocked(atomic_load(&waits));
        atomic_store(&race, 2);
        what = sample_once();
        atomic_store(&race, 0);
        once_waiting += what == HV_SAMPLE_WAITING, once_faults += what == HV_SAMPLE_FAULT;

        until_blocked(atomic_load(&waits));
        atomic_store(&race, 3);
        ran_engine += sample_once() == HV_SAMPLE_ENGINE;
        atomic_store(&race, 0);
        int n = atomic_load(&waits);
        atomic_store(&back, 1); /* sent back to wait */
        until_blocked(n + 1);

        until_blocked(atomic_load(&waits));
        still_waiting += sample_once() == HV_SAMPLE_WAITING;
    }
    printf("woken at both looks: %u not sampled, %u fault of %d; woken once, running on: %u engine of %d; blocked all "
           "along: %u waiting of %d\n", woke_none, woke_faults, RACES, ran_engine, RACES, still_waiting, RACES);
    CHECK(woke_faults == 0, "a thread woken while it was looked at was called a fault %u times in %d", woke_faults,
          RACES);
    CHECK(woke_none == RACES, "a thread woken at both looks (its own code, or a new fault) is not sampled: %u of %d",
          woke_none, RACES);
    CHECK(ran_engine == RACES, "a thread that woke and runs on in its own code is in Henge's code: %u of %d",
          ran_engine, RACES);
    printf("woken once, blocked again by the fresh look: %u waiting, %u fault of %d\n", once_waiting, once_faults, RACES);
    CHECK(once_waiting == RACES, "a thread that woke, then blocked again, is judged as it is now: waiting %u of %d",
          once_waiting, RACES);
    CHECK(still_waiting == RACES, "a thread blocked on a semaphore all along is waiting: %u of %d", still_waiting,
          RACES);

    atomic_store(&stop, 1);
    atomic_store(&back, 1);
    semaphore_signal(sem);
    pthread_join(t, NULL);

    /* By chance: a thread that sleeps 5 us at a time and runs 5 us */
    memset(kinds, 0, sizeof kinds);
    unsigned skipped_before = atomic_load(&skipped);
    atomic_store(&slot, -1);
    atomic_store(&stop, 0);
    pthread_create(&t, NULL, napper, NULL);
    while (atomic_load(&slot) < 0) usleep(1000);
    unsigned n = 0;
    for (int k = 0; k < SAMPLES; k++) {
        n += sample_once() != HV_SAMPLE_KINDS;
        usleep(200 + (unsigned)(k * 7919) % 300);
    }
    atomic_store(&stop, 1);
    pthread_join(t, NULL);
    unsigned none = atomic_load(&skipped) - skipped_before;
    printf("by chance, %u samples: engine %u, waiting %u, fault %u; %u woke at both looks, not sampled\n", n,
           kinds[HV_SAMPLE_ENGINE], kinds[HV_SAMPLE_WAITING], kinds[HV_SAMPLE_FAULT], none);
    CHECK(n + none == SAMPLES, "%u samples of %d read, %u not sampled", n, SAMPLES, none);
    CHECK(kinds[HV_SAMPLE_FAULT] == 0, "a thread that sleeps and wakes was called a fault %u times",
          kinds[HV_SAMPLE_FAULT]);

    printf("%s\n", failures ? "test_stallmon_wake FAILED" : "test_stallmon_wake ok");
    return failures != 0;
}

