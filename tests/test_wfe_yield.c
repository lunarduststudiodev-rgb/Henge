/* The in-place WFE wait gives the host core up when a guest spin goes nowhere (dispatch.c wfe). A guest CPU
   spins LDAXR/WFE on a word: nobody writing it, it gives the core up once every four timed-out waits (~200 us);
   the word moving every 20 us (the owner making progress), it doesn't; the word moving between the guest's waits
   rather than during them, it doesn't either; the word moving and then stopping, it starts again once the word
   has stood still for four waits.

   The counts hold however loaded the host is. Each part lasts a given amount of the guest CPU thread's own CPU time,
   not of wall-clock time, so a busy Mac doesn't shorten its waits to too few to tell; a yield is checked against the
   waits, which time out by the wall clock; and the writer counts the gaps where it was itself held off its core (a
   still word then is a fair reason to give the core up). */
#include <mach/mach.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach_time.h>
#include "cpu.h"
#include "dispatch.h"
#include "machine.h"

#define PG 16384
static HvMachine m;
static volatile uint32_t *word;
static int failures;

static void spin_until(uint64_t t) { while (mach_absolute_time() < t) {} }

/* A thread's CPU time so far, user and system, in microseconds */
static uint64_t cpu_us(thread_act_t t) {
    thread_basic_info_data_t ti;
    mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
    if (thread_info(t, THREAD_BASIC_INFO, (thread_info_t)&ti, &n) != KERN_SUCCESS) return 0;
    return ((uint64_t)ti.user_time.seconds + (uint64_t)ti.system_time.seconds) * 1000000u +
           (uint64_t)ti.user_time.microseconds + (uint64_t)ti.system_time.microseconds;
}

/* writes: every `every_us` microseconds while the guest's thread uses `busy_ms` of CPU time, then nothing while it
   uses `quiet_ms` more, then bit 0 (the guest leaves). late: how many yields the writer's own time off its core
   allows. A yield takes four timed-out waits of 50 us on a word that stood still, so a gap between writes allows
   one for each 200 us of it; counted per 150 us, and one more for a gap that straddles a yield's start. stuck: the
   guest's thread got no CPU time for 5 s (it left hv_run, or never ran). */
typedef struct {
    unsigned every_us, busy_ms, quiet_ms, late;
    thread_act_t guest;
    _Atomic int started;
    int stuck;
} Plan;

/* Until the guest's thread has used `us` more CPU time than at c0, calling each() every 20 us or so */
static int until_guest(Plan *p, uint64_t c0, uint64_t us, void (*each)(Plan *, uint64_t *), uint64_t *t) {
    uint64_t seen = c0, moved = mach_absolute_time();
    for (;;) {
        uint64_t c = cpu_us(p->guest), now = mach_absolute_time();
        if (c - c0 >= us) return 1;
        if (c != seen) seen = c, moved = now;
        else if (now - moved > 24000000ull * 5) return p->stuck = 1, 0;
        if (each) each(p, t);
        else spin_until(now + 24 * 20);
    }
}

static uint64_t last;
static void write_next(Plan *p, uint64_t *t) {
    spin_until(*t += 24ull * p->every_us);
    __atomic_fetch_add(word, 2, __ATOMIC_RELEASE);
    uint64_t now = mach_absolute_time();
    if (now - last >= 24 * 150) p->late += 1 + (unsigned)((now - last) / (24 * 150));
    last = now, *t = now > *t ? now : *t;
}

static void *writer(void *arg) {
    Plan *p = arg;
    uint64_t c0 = cpu_us(p->guest), t = mach_absolute_time();
    last = t;
    atomic_store(&p->started, 1); /* the guest starts now: its word is still until the first write */
    if (p->every_us && until_guest(p, c0, 1000ull * p->busy_ms, write_next, &t)) c0 = cpu_us(p->guest);
    if (!p->stuck) until_guest(p, c0, 1000ull * p->quiet_ms, NULL, NULL);
    __atomic_fetch_or(word, 1, __ATOMIC_RELEASE);
    return NULL;
}

typedef struct { uint64_t yields, waits; } Got;
static Got run(HvCpu *cpu, uint32_t *code, Plan *plan) {
    *word = 0;
    hv_cpu_reset_index(cpu, &m, 0);
    cpu->cpacr_el1 = 3u << 20;
    cpu->el = 0, cpu->spsel = 0;
    cpu->x[19] = (uint64_t)(uintptr_t)word;
    cpu->pc = (uint64_t)(uintptr_t)code;
    uint64_t waits = HV_STAT_TOTAL(&m, wfe_waits), yields = HV_STAT_TOTAL(&m, wfe_yields);
    plan->guest = mach_thread_self();
    atomic_store(&plan->started, 0);
    pthread_t t;
    pthread_create(&t, NULL, writer, plan);
    while (!atomic_load(&plan->started)) {} /* a writer not yet running is not one held off its core */
    int r = hv_run(cpu);
    pthread_join(t, NULL);
    mach_port_deallocate(mach_task_self(), plan->guest);
    if (plan->stuck) printf("FAIL: the guest's thread got no CPU time for 5 s\n"), failures++;
    if (r != HV_EXIT_HALT) printf("FAIL: run ended %d at %llx\n", r, (unsigned long long)cpu->pc), failures++;
    Got g = {HV_STAT_TOTAL(&m, wfe_yields) - yields, HV_STAT_TOTAL(&m, wfe_waits) - waits};
    if (!g.waits) printf("FAIL: no WFE waited in place (the test is not testing it)\n"), failures++;
    return g;
}

int main(void) {
    if (hv_machine_init(&m, 64u << 20) != 0) return 1;
    m.chain = 1, m.stats_interval = 1;
    uint8_t *buf = aligned_alloc(PG, 2 * PG);
    memset(buf, 0, 2 * PG);
    hv_machine_add_ram(&m, (uint64_t)(uintptr_t)buf, 2 * PG, buf, 0);
    uint32_t *code = (uint32_t *)buf;
    word = (volatile uint32_t *)(buf + PG);
    /* A: waits for the word to change */
    code[0] = 0x885ffe61; /* loop: ldaxr w1, [x19] */
    code[1] = 0x37000061; /* tbnz w1, #0, out */
    code[2] = 0xd503205f; /* wfe */
    code[3] = 0x17fffffd; /* b loop */
    code[4] = 0xd503201f; /* out: nop, stop here */
    /* B: the same, and after each wait the word moves on (ldadd), as if its owner ran while the guest was away */
    uint32_t *moves = code + 8;
    moves[0] = 0x885ffe61; /* loop: ldaxr w1, [x19] */
    moves[1] = 0x370000a1; /* tbnz w1, #0, out */
    moves[2] = 0xd503205f; /* wfe */
    moves[3] = 0x52800042; /* mov w2, #2 */
    moves[4] = 0xb8220263; /* ldadd w2, w3, [x19] */
    moves[5] = 0x17fffffb; /* b loop */
    moves[6] = 0xd503201f; /* out: nop, stop here */
    HvCpu *cpu = aligned_alloc(_Alignof(HvCpu), (sizeof(HvCpu) + _Alignof(HvCpu) - 1) & ~(_Alignof(HvCpu) - 1));
    memset(cpu, 0, sizeof *cpu);
    m.cpus[0] = cpu, m.ncpus = 1;

    m.stop_pc = (uint64_t)(uintptr_t)&code[4];
    Plan p = {.quiet_ms = 10};
    Got idle = run(cpu, code, &p);
    printf("nobody writing for 10 ms of its CPU time: gave the core up %llu times in %llu waits\n", (unsigned long long)idle.yields,
           (unsigned long long)idle.waits);
    /* every wait but the last (bit 0 set) timed out on the same word and value: 10 ms of its CPU time in waits of
       at most 50 us each is 200 of them, so 50 yields */
    if (idle.yields < 30 || idle.yields + 1 < idle.waits / 4 || idle.yields > idle.waits / 4)
        printf("FAIL: expected one each 4 timed-out waits\n"), failures++;

    p = (Plan){.every_us = 20, .busy_ms = 10};
    Got moving = run(cpu, code, &p);
    printf("the word moving every 20 us for 10 ms: gave the core up %llu times (writer late %u times)\n",
           (unsigned long long)moving.yields, p.late);
    if (moving.yields > 2 + p.late) printf("FAIL: an owner making progress is waited for in place\n"), failures++;

    m.stop_pc = (uint64_t)(uintptr_t)&moves[6];
    p = (Plan){.quiet_ms = 10};
    Got between = run(cpu, moves, &p);
    printf("the word moving between waits for 10 ms: gave the core up %llu times in %llu waits\n",
           (unsigned long long)between.yields, (unsigned long long)between.waits);
    if (between.waits < 100) printf("FAIL: too few waits to tell (%llu)\n", (unsigned long long)between.waits), failures++;
    if (between.yields) printf("FAIL: a word that moved since the last wait is progress, not a spin\n"), failures++;

    m.stop_pc = (uint64_t)(uintptr_t)&code[4];
    p = (Plan){.every_us = 20, .busy_ms = 10, .quiet_ms = 3};
    Got after = run(cpu, code, &p);
    printf("moving for 10 ms, then 3 ms of nothing: gave the core up %llu times in %llu waits (writer late %u times)\n",
           (unsigned long long)after.yields, (unsigned long long)after.waits, p.late);
    /* 3 ms of its CPU time in waits of at most 50 us each: 60 timed out at least, so 15 yields; one each 4 at most
       (and the 3 rounds a run before may have left) */
    if (after.yields < 10 || after.yields > after.waits / 4 + 1)
        printf("FAIL: expected one each 4 timed-out waits of the quiet 3 ms (at least 10)\n"), failures++;

    printf("%s\n", failures ? "FAILED" : "ok");
    return failures != 0;
}
