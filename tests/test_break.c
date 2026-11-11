/* hengevm's --break: core/machine.c hv_machine_set_break, which host/main.c's CPU 0 thread calls as it starts, and
   hv_cpu_pass_stop, which it calls to go on past each hit until the brk_hits-th.
   Without --break it must leave the stop alone: a hook may be set there already (hooksha, from the watch thread, can
   come first when a Suspend is resumed), and a stop it published would wipe it.
   With --break, CPU 0 (the CPU hengevm follows) halts at the break, and the other CPUs run past it: a secondary that
   halted there would end its thread for good ("cpu N stopped"), its share of the guest's work left undone. Each CPU
   runs a loop through the break in the dispatcher: CPU 1 must go round it many times until the test halts it; CPU 0
   must stop at it before its first time round, and, passing it each time, after each time round; but not pass it
   when its run starts elsewhere, or takes an interrupt first, and comes to it later; nor keep a pass past the first
   stop its run comes to, or past a start where the stop no longer is. The stop must never move
   meanwhile: moving it while CPUs run takes a flush each time, and a CPU may run past it in between.
   Before it all, with no stop set: a guest jump to -1 (the "none" value of the stop's pcs) takes its fault.
   Then all of it again with chaining on (hengevm turns it off with --break; the hooks' stops are for every CPU): a CPU
   that runs past the stop must not chain a jump into it, or the CPU it is for runs past it too. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cpu.h"
#include "dispatch.h"
#include "machine.h"

#define PG 16384
static HvMachine m;
static int failures;
static void hook(HvCpu *cpu) { (void)cpu; }

typedef struct { HvCpu *cpu; int r, done; } Run;
static void *run(void *arg) {
    Run *a = arg;
    a->r = hv_run(a->cpu);
    __atomic_store_n(&a->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

static HvCpu *new_cpu(uint32_t index, uint64_t pc) {
    size_t al = _Alignof(HvCpu);
    HvCpu *c = aligned_alloc(al, (sizeof(HvCpu) + al - 1) & ~(al - 1));
    memset(c, 0, sizeof(HvCpu));
    hv_cpu_reset_index(c, &m, index);
    c->cpacr_el1 = 3u << 20;
    c->el = 0, c->spsel = 0;
    c->pc = pc;
    return c;
}

/* Runs c until it returns, or until it has been round the loop `rounds` times in all (or 2 s have gone), when the
   test halts it. Returns the run; *halted_by_test says which. */
static Run go(HvCpu *c, uint64_t rounds, int *halted_by_test) {
    Run a = {c, 0, 0};
    pthread_t t;
    pthread_create(&t, NULL, run, &a);
    uint64_t give_up = now_ns() + 2000000000ull;
    while (!__atomic_load_n(&a.done, __ATOMIC_ACQUIRE) && __atomic_load_n(&c->x[0], __ATOMIC_RELAXED) < rounds &&
           now_ns() < give_up) {}
    *halted_by_test = !__atomic_load_n(&a.done, __ATOMIC_ACQUIRE);
    if (*halted_by_test) hv_machine_halt(&m, HV_HALT_HOST);
    pthread_join(t, NULL);
    return a;
}

int main(void) {
    if (hv_machine_init(&m, 64u << 20) != 0) return 1;

    hv_machine_set_stop(&m, 0x1000, ~0ull, hook, 0);
    hv_machine_set_break(&m, ~0ull); /* hengevm without --break */
    if (m.stop_pc != 0x1000 || m.stop_hook != hook)
        printf("FAIL a run without --break wiped the hook set before it (stop %llx)\n", (unsigned long long)m.stop_pc),
            failures++;

    uint8_t *buf = aligned_alloc(PG, PG);
    memset(buf, 0, PG);
    hv_machine_add_ram(&m, (uint64_t)(uintptr_t)buf, PG, buf, 0);
    uint32_t *code = (uint32_t *)buf;

    /* No stop at all (~0, as hv_machine_init leaves it) and a guest that jumps to -1, ~0 too: the CPU must take the PC
       alignment fault, as a real one does, not halt there (a secondary's thread would end, on CPU 0 the whole Mac). */
    hv_machine_set_stop(&m, ~0ull, ~0ull, NULL, 0);
    /* The rule itself, as the run below is also kept from halting by the step past's own "none", ~0 too */
    if (hv_stop_hook(&m, ~0ull, 0).here) printf("FAIL pc ~0 met \"no stop\" as a stop\n"), failures++;
    code[8] = 0x92800001; /* mov x1, #-1 */
    code[9] = 0xd61f0020; /* br x1 */
    code[0x400 / 4] = 0x91000400; /* vbar + 0x400, a synchronous exception from EL0: add x0, x0, #1 */
    code[0x404 / 4] = 0x17ffffff; /* b .-4 */
    HvCpu *wild = new_cpu(0, (uint64_t)(uintptr_t)&code[8]);
    wild->vbar_el1 = (uint64_t)(uintptr_t)code;
    int halted_by_test;
    Run w = go(wild, 1000, &halted_by_test);
    printf("a jump to -1: %s; pc %llx el %u esr %llx far %llx\n",
           halted_by_test ? "the fault taken, its vector run until the test halted it" : "the run ended on its own",
           (unsigned long long)wild->pc, (unsigned)wild->el, (unsigned long long)wild->esr_el1,
           (unsigned long long)wild->far_el1);
    if (!halted_by_test || wild->el != 1 || wild->esr_el1 >> 26 != 0x22 || wild->far_el1 != ~0ull)
        printf("FAIL a jump to -1 %s, not the PC alignment fault\n", w.r == HV_EXIT_HALT ? "halted the CPU" : "ran on"),
            failures++;
    code[0] = 0x91000400; /* brk: add x0, x0, #1 */
    code[1] = 0x17ffffff; /* b brk */
    uint64_t brk = (uint64_t)(uintptr_t)code;
    m.chain = 0; /* as hengevm sets it with --break: every block comes back to the dispatcher, and so to the stop */
    hv_machine_set_break(&m, brk); /* hengevm --break brk */
    uint32_t seq = m.stop_seq;

    /* CPU 0 first: a halt by the test, below, would halt it too */
    HvCpu *zero = new_cpu(0, brk);
    Run r;
    for (uint64_t hit = 0; hit < 5; hit++) {
        if (hit) hv_cpu_pass_stop(zero); /* what hengevm does after each hit but the last */
        r = go(zero, hit + 1, &halted_by_test);
        printf("CPU 0: round the loop %llu times, %s at %llx\n", (unsigned long long)zero->x[0],
               halted_by_test ? "then halted by the test" : "then it stopped", (unsigned long long)zero->pc);
        if (halted_by_test || r.r != HV_EXIT_HALT || zero->halted || zero->pc != brk || zero->x[0] != hit) {
            printf("FAIL CPU 0 did not stop at its break %s\n", hit ? "the next time round, having passed it" : "");
            failures++;
            break;
        }
    }

    /* A pass is for the place the run starts: one that goes elsewhere first (here a jump to the break; an interrupt
       taken there, for hengevm) has no pass when it comes to the break, and stops. */
    code[2] = 0x17fffffe; /* b brk */
    uint64_t before = zero->x[0];
    hv_cpu_pass_stop(zero);
    zero->pc = brk + 8;
    r = go(zero, before + 1, &halted_by_test);
    printf("CPU 0, passing the break but starting elsewhere: round the loop %llu more times, %s at %llx\n",
           (unsigned long long)(zero->x[0] - before), halted_by_test ? "then halted by the test" : "then it stopped",
           (unsigned long long)zero->pc);
    if (halted_by_test || r.r != HV_EXIT_HALT || zero->pc != brk || zero->x[0] != before)
        printf("FAIL CPU 0 ran past its break though it did not start there\n"), failures++;

    /* As hengevm meets it: the pass set at the break, and an interrupt taken there first. Its vector goes back to the
       break, as the guest's thread would after it: it must stop there, not go round. */
    code[0x480 / 4] = 0x17fffee0; /* vbar + 0x480, IRQ from EL0: b brk */
    zero->vbar_el1 = brk;
    uint64_t daif = zero->daif;
    zero->daif = 0; /* IRQs unmasked: a CPU starts with them masked */
    zero->pc = brk;
    hv_cpu_pass_stop(zero);
    zero->pc_state->irq_line = 1;
    r = go(zero, before + 1, &halted_by_test);
    printf("CPU 0, passing the break but taking an interrupt first: round the loop %llu more times, %s at %llx "
           "(el %u)\n",
           (unsigned long long)(zero->x[0] - before), halted_by_test ? "then halted by the test" : "then it stopped",
           (unsigned long long)zero->pc, (unsigned)zero->el);
    if (halted_by_test || r.r != HV_EXIT_HALT || zero->pc != brk || zero->el != 1 || zero->x[0] != before)
        printf("FAIL CPU 0 ran past its break though an interrupt came first\n"), failures++;
    zero->pc_state->irq_line = 0;
    zero->el = 0, zero->daif = daif;

    HvCpu *one = new_cpu(1, brk);
    r = go(one, 1000, &halted_by_test);
    printf("CPU 1: round the loop %llu times, %s\n", (unsigned long long)one->x[0],
           halted_by_test ? "then halted by the test" : "then it left on its own");
    if (!halted_by_test || one->x[0] < 1000)
        printf("FAIL CPU 1 stopped at CPU 0's break (pc %llx, run ended %d)\n", (unsigned long long)one->pc, r.r),
            failures++;

    if (m.stop_seq != seq || m.stop_pc != brk)
        printf("FAIL the stop moved while the CPUs ran (%u moves)\n", (m.stop_seq - seq) / 2), failures++;

    /* A pass is used up by the first stop the run comes to, or by none being there: it never waits for a later one.
       CPU 0 with the stop moved away from its pass; then CPU 1 with a pass at CPU 0's break, which it runs past. */
    zero->halted = HV_HALT_NONE; /* the test's halt of CPU 1 halted it too */
    zero->pc = brk;
    hv_cpu_pass_stop(zero);
    hv_machine_set_stop(&m, brk + 0x100, ~0ull, NULL, 1u << 0);
    before = zero->x[0];
    go(zero, before + 1000, &halted_by_test);
    if (zero->pc_state->stop_pass != ~0ull)
        printf("FAIL CPU 0's pass outlived a run that started where the stop no longer was\n"), failures++;
    hv_machine_set_break(&m, brk);
    HvCpu *one3 = new_cpu(1, brk);
    hv_cpu_pass_stop(one3);
    go(one3, 1000, &halted_by_test);
    if (one3->pc_state->stop_pass != ~0ull)
        printf("FAIL CPU 1's pass outlived the first stop it came to, CPU 0's\n"), failures++;

    /* Again with chaining on, a loop of its own: CPU 1 first, so that a jump it chained into the stop (it runs past
       it) would take CPU 0 past it too, for good. */
    code[4] = 0x91000400; /* brk2: add x0, x0, #1 */
    code[5] = 0x17ffffff; /* b brk2 */
    uint64_t brk2 = brk + 16;
    m.chain = 1;
    hv_machine_set_break(&m, brk2);
    HvCpu *one2 = new_cpu(1, brk2);
    r = go(one2, 1000, &halted_by_test);
    printf("chained, CPU 1: round the loop %llu times, %s\n", (unsigned long long)one2->x[0],
           halted_by_test ? "then halted by the test" : "then it left on its own");
    if (!halted_by_test || one2->x[0] < 1000)
        printf("FAIL chained, CPU 1 stopped at CPU 0's break (pc %llx)\n", (unsigned long long)one2->pc), failures++;
    zero->halted = HV_HALT_NONE; /* the test's halt of CPU 1 halted it too */
    zero->pc = brk2, zero->x[0] = 0;
    for (uint64_t hit = 0; hit < 3; hit++) {
        if (hit) hv_cpu_pass_stop(zero);
        Run z = go(zero, hit + 1, &halted_by_test);
        printf("chained, CPU 0: round the loop %llu times, %s at %llx\n", (unsigned long long)zero->x[0],
               halted_by_test ? "then halted by the test" : "then it stopped", (unsigned long long)zero->pc);
        if (halted_by_test || z.r != HV_EXIT_HALT || zero->halted || zero->pc != brk2 || zero->x[0] != hit) {
            printf("FAIL chained, CPU 0 did not stop at its break %s\n", hit ? "the next time round" : ""), failures++;
            break;
        }
    }

    printf("%s\n", failures ? "FAILED" : "ok");
    return failures != 0;
}
