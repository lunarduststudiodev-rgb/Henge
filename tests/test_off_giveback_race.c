/* A CPU that stays off gives back what it holds for running (wait_while_off) with its lock let go: CPU_ON must not
   reset it meanwhile. A reset that met the give-back left a TLB marked ready on fresh zero pages (a guest access then
   went to a host address near 0) or ASID tables zeroed under a live user id (other CPUs' TLBIs dropped nothing).

   CPU_ON spins on parked_off, then takes the target's wake_mu. Forced here: this thread holds CPU 1's wake_mu while
   CPU 1's wait ends and it waits for the lock, then a CPU_ON caller passes its spin and waits for it too. Let go,
   CPU 1 has it first (the system's mutexes go in turn): it clears parked_off and lets the lock go for its give-back,
   held up 30 ms at its TLB's mapping (mmap below). A CPU_ON that returns within that is the race.

   And a pause: an off CPU that holds its tables wakes every threshold to see whether it may give them back, a pause
   on or not. A paused machine must read settled every time it is asked meanwhile (a save asks once, after the
   pause settled), so the CPU never reads unparked for that look. */
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "dispatch.h"
#include "exception.h"
#include "machine.h"
#include "mmu.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static HvMachine m;
static HvCpu cpus[2];
static void *tlb_page;               /* the first whole page of CPU 1's TLB: its give-back maps it fresh */
static int armed, in_giveback, cpu_on_done, held_up;

/* The engine's mmap: a give-back's fresh mapping of CPU 1's TLB waits 30 ms while armed */
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    static void *(*real)(void *, size_t, int, int, int, off_t);
    if (!real) real = (void *(*)(void *, size_t, int, int, int, off_t))dlsym(RTLD_NEXT, "mmap");
    if ((flags & MAP_FIXED) && addr == tlb_page && __atomic_load_n(&armed, __ATOMIC_SEQ_CST)) {
        __atomic_store_n(&in_giveback, 1, __ATOMIC_SEQ_CST);
        usleep(30000);
        __atomic_add_fetch(&held_up, 1, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&cpu_on_done, __ATOMIC_SEQ_CST)) {
            /* CPU_ON reset CPU 1 while it was giving back: what follows would run it on a half-zeroed TLB */
            printf("FAIL: CPU_ON returned while CPU 1 was giving back what it held (round %d)\n", held_up);
            fflush(stdout);
            _exit(1);
        }
        __atomic_store_n(&in_giveback, 0, __ATOMIC_SEQ_CST);
    }
    return real(addr, len, prot, flags, fd, off);
}

static void *run_cpu(void *arg) { return (void *)(intptr_t)hv_run(arg); }

static void *cpu_on(void *arg) {
    (void)arg;
    cpus[0].x[0] = 0xc4000003;
    cpus[0].x[1] = 1;
    cpus[0].x[2] = 0x10100;
    cpus[0].x[3] = 0;
    hv_exception_insn(&cpus[0], 0xd4000002, 0x10000);
    __atomic_store_n(&cpu_on_done, 1, __ATOMIC_SEQ_CST);
    return NULL;
}

/* CPU 1 off and waiting, holding its TLB (it ran) */
static int off_and_holding(HvCpu *c) {
    for (int n = 0; n < 5000; n++) {
        if (__atomic_load_n(&c->off, __ATOMIC_SEQ_CST) && __atomic_load_n(&c->parked_off, __ATOMIC_SEQ_CST) &&
            hv_tlb_is_ready(c))
            return 1;
        usleep(1000);
    }
    return 0;
}

int main(void) {
    alarm(60);
    enum { THRESHOLD_MS = 20 };
    uint32_t *code = aligned_alloc(16384, 16384);
    if (!code || hv_machine_init(&m, 4 << 20)) return 1;
    memset(code, 0, 16384);
    code[0] = 0xd4000002;    /* 0x10000: hvc #0 (x0 = CPU_OFF) */
    code[0x40] = 0x91000529; /* 0x10100: add x9, x9, #1 (CPU_ON's entry) */
    code[0x41] = 0xd2800040; /*          movz x0, #2 */
    code[0x42] = 0xf2b08000; /*          movk x0, #0x8400, lsl #16: CPU_OFF */
    code[0x43] = 0xd4000002; /*          hvc #0 */
    hv_machine_add_ram(&m, 0x10000, 16384, code, 1);
    m.off_giveback_ms = THRESHOLD_MS;
    for (int i = 0; i < 2; i++) {
        hv_cpu_reset_index(&cpus[i], &m, i);
        cpus[i].mpidr = i;
    }
    cpus[0].off = 1; /* CPU 0 is this thread's: it only makes the CPU_ON calls */
    cpus[0].parked_off = 1;
    HvCpu *c = &cpus[1];
    uintptr_t page = (uintptr_t)getpagesize();
    tlb_page = (void *)(((uintptr_t)c->tlb + page - 1) & ~(page - 1));
    c->pc = 0x10000;
    c->x[0] = 0x84000002;
    pthread_t t;
    pthread_create(&t, NULL, run_cpu, c);

    int rounds = 0, gave = 0;
    for (; rounds < 12 && gave < 6; rounds++) {
        if (!off_and_holding(c)) break;
        pthread_mutex_lock(&c->pc_state->wake_mu);
        __atomic_store_n(&cpu_on_done, 0, __ATOMIC_SEQ_CST);
        usleep(3 * THRESHOLD_MS * 1000); /* CPU 1's wait over: it waits for the lock */
        pthread_t on;
        pthread_create(&on, NULL, cpu_on, NULL);
        usleep(10000); /* the caller past its spin (parked_off still set): it waits for the lock too */
        int before = __atomic_load_n(&held_up, __ATOMIC_SEQ_CST);
        __atomic_store_n(&armed, 1, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&c->pc_state->wake_mu);
        pthread_join(on, NULL);
        __atomic_store_n(&armed, 0, __ATOMIC_SEQ_CST);
        CHECK(cpus[0].x[0] == 0, "CPU_ON succeeds (%lld)", (long long)cpus[0].x[0]);
        if (__atomic_load_n(&held_up, __ATOMIC_SEQ_CST) > before) gave++;
    }
    printf("%d rounds: CPU 1 gave back first in %d\n", rounds, gave);
    CHECK(gave >= 3, "the give-back had the lock first in most rounds (%d of %d)", gave, rounds);
    CHECK(off_and_holding(c), "CPU 1 runs its CPU_ON entry again each time and goes off");
    CHECK(c->x[9] == 1, "  from its reset state (x9 %llu)", (unsigned long long)c->x[9]);

    /* Paused with CPU 1 off and holding, waking every 2 ms: asked 1 s long, the machine reads settled each time */
    m.off_giveback_ms = 2;
    int asked = 0, unsettled = 0, kept = 0;
    for (int tries = 0; tries < 10 && !kept; tries++) {
        cpu_on(NULL); /* it runs its entry and goes off again, its wait now 2 ms */
        while (!(__atomic_load_n(&c->off, __ATOMIC_SEQ_CST) && __atomic_load_n(&c->parked_off, __ATOMIC_SEQ_CST)))
            sched_yield();
        hv_machine_pause(&m); /* before its first 2 ms are up, mostly */
        uint64_t gb = HV_STAT_TOTAL(&m, off_givebacks);
        CHECK(hv_machine_pause_wait(&m, 2000) == 0, "the machine pauses with CPU 1 off");
        uint64_t until = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 1000000000ull, counter;
        asked = unsettled = 0;
        while (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) < until)
            asked++, unsettled += hv_machine_paused_counter(&m, &counter) != 0;
        kept = HV_STAT_TOTAL(&m, off_givebacks) == gb && hv_tlb_is_ready(c);
        hv_machine_resume(&m);
    }
    printf("paused 1 s with CPU 1 off and holding: asked %d times, unsettled %d\n", asked, unsettled);
    CHECK(kept, "CPU 1 held its tables through a pause (waking every 2 ms)");
    CHECK(unsettled == 0, "a paused machine reads settled every time (%d of %d not)", unsettled, asked);

    hv_machine_halt(&m, HV_HALT_HOST);
    hv_cpu_wake(c);
    void *r;
    pthread_join(t, &r);
    CHECK((intptr_t)r == HV_EXIT_HALT, "CPU 1 ends on the halt");
    hv_machine_free(&m);
    free(code);
    printf("%d/%d checks passed\n", checks - fails, checks);
    return fails != 0;
}
