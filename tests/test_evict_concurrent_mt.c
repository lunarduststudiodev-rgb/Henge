// Zones emptied in the background while several CPUs run (core/dispatch.c, the reclaimer): every CPU walks a chain of
// more code than the cache holds, each step a call to a shared leaf and back (so the return tables fill with
// addresses in every zone), while the reclaimer empties the zone filled next with them running, and the stop that
// finds the cache full fills it at once. One thread rewrites words of the chain (the same instruction: its blocks
// retired), another sends memory warnings (zones retired once the reclaimer has emptied them). Emptied code is zeroed
// (HV_JIT_POISON), so a CPU left in, returning into, or chained into a zone the reclaimer emptied faults or counts
// wrong. Each CPU must end with the count its walk makes; the reclaimer must have emptied zones, and a stop that
// filled the zone it readied, or marked one for it, must have taken under 0.1 ms of its own work on average (emptying
// a zone here takes 1-2 ms; the longest is printed, not checked: a host thread can be preempted for that long).
// NOSPARE=1 (HV_EVICT_SYNC): every zone emptied in the stop, as before. NOWRITE=1 / NOWARN=1 leave out the writer /
// the warnings. usage: test_evict_concurrent_mt [walks]
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach_time.h>

#include "dispatch.h"
#include "machine.h"
#include "zalloc.h"

#define PAGE (1u << HV_RAM_PAGE_BITS)
#define BASE 0x80000000ull
#define STEP 32                     /* bytes a step: add x0, #1; mov x28, x30; bl LEAF; mov x30, x28; b next */
#define NCHAIN 128                  /* pages of chain (512 steps, 1024 blocks a page): more than the cache holds */
#define LEAF (BASE + NCHAIN * PAGE) /* add x2, x2, #1; ret */
#define STOP (LEAF + PAGE)          /* never translated: the dispatcher stops there */
#define LOOP (STOP + PAGE)          /* the walk's start: blr x23 into the chain; subs; b.ne; b STOP */
#define NPAGES (NCHAIN + 3)
#define NCPU 4
#define STEPS (NCHAIN * PAGE / STEP)

static HvMachine m;
static uint32_t *ram;
static int failures;
static volatile int writing;
static int rounds = 6;
#define JIT_BYTES (16u << 20) /* 4 zones of 4 MB: a 1 MB chunk for each CPU (core/translate.c chunk_take; zones
                                 smaller than that are full as soon as every CPU asks for one), 26K blocks */
#define NZONES 4

static void check(int ok, const char *what) {
    printf("%s: %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

static uint32_t *at(uint64_t va) { return ram + (va - BASE) / 4; }
static uint32_t branch(uint64_t from, uint64_t to) { return 0x14000000u | ((uint32_t)((int64_t)(to - from) / 4) & 0x03ffffff); }

static void *cpu_thread(void *arg) {
    HvCpu *c = arg;
    int r = hv_run(c);
    if (r != HV_EXIT_HALT || c->pc != STOP) {
        printf("cpu %u stopped with %d at %llx\n", c->index, r, (unsigned long long)c->pc);
        __atomic_add_fetch(&failures, 1, __ATOMIC_RELAXED);
    }
    return NULL;
}

/* The same add again, at a step's start somewhere along the chain: its page's blocks are retired. */
static void *writer(void *u) {
    (void)u;
    uint32_t seed = 1, add = 0x91000400;
    while (writing == 1) {
        seed = seed * 1103515245u + 12345u;
        hv_phys_write(&m, BASE + (uint64_t)(seed >> 8) % STEPS * STEP, &add, 4);
        usleep(300);
    }
    return NULL;
}

static void *warner(void *u) {
    (void)u;
    while (writing && !getenv("NOWARN")) {
        hv_machine_request_trim(&m, 0);
        usleep(20000);
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc > 1) rounds = atoi(argv[1]);
    setvbuf(stdout, NULL, _IONBF, 0);
    setenv("HV_JIT_POISON", "1", 1);
    int nospare = getenv("NOSPARE") && atoi(getenv("NOSPARE")) > 0;
    if (hv_machine_init(&m, JIT_BYTES) != 0) return 1;
    if (nospare) m.reclaim_off = 1;
    m.chain = 1;
    hv_machine_set_zones(&m, NZONES);
    m.trim_quiet_ns = 5000000; /* zones come back 5 ms after the last warning */
    ram = aligned_alloc(PAGE, (size_t)NPAGES * PAGE);
    if (!ram || hv_machine_add_ram(&m, BASE, (uint64_t)NPAGES * PAGE, ram, 0) != 0) return 1;
    memset(ram, 0, (size_t)NPAGES * PAGE);
    for (uint64_t i = 0; i < STEPS; i++) {
        uint64_t s = BASE + i * STEP;
        at(s)[0] = 0x91000400;                                  /* add x0, x0, #1 */
        at(s)[1] = 0xaa1e03fc;                                  /* mov x28, x30 */
        at(s)[2] = 0x94000000u | (uint32_t)(((int64_t)(LEAF - (s + 8)) / 4) & 0x03ffffff); /* bl LEAF */
        at(s)[3] = 0xaa1c03fe;                                  /* mov x30, x28 */
        at(s)[4] = i + 1 < STEPS ? branch(s + 16, s + STEP) : 0xd65f03c0; /* b next; ret (to LOOP) */
    }
    at(LEAF)[0] = 0x91000442;                                 /* add x2, x2, #1 */
    at(LEAF)[1] = 0xd65f03c0;                                 /* ret */
    at(LOOP)[0] = 0xd63f02e0;                                 /* L: blr x23 */
    at(LOOP)[1] = 0xf1000421;                                 /* subs x1, x1, #1 */
    at(LOOP)[2] = 0x54000001 | (uint32_t)(-2 & 0x7ffff) << 5; /* b.ne L */
    at(LOOP)[3] = branch(LOOP + 12, STOP);
    m.stop_pc = STOP;

    HvCpu *cpu[NCPU];
    pthread_t t[NCPU], wt, mt;
    for (int c = 0; c < NCPU; c++) {
        cpu[c] = zalloc(_Alignof(HvCpu), sizeof(HvCpu));
        hv_cpu_reset_index(cpu[c], &m, (uint32_t)c);
        cpu[c]->cpacr_el1 = 3u << 20;
        cpu[c]->el = 0, cpu[c]->spsel = 0;
        cpu[c]->pc = LOOP;
        cpu[c]->x[0] = 0, cpu[c]->x[1] = (uint64_t)rounds, cpu[c]->x[2] = 0;
        cpu[c]->x[23] = BASE + (uint64_t)c * (STEPS / NCPU) * STEP; /* each starts elsewhere along the chain */
    }
    writing = getenv("NOWRITE") ? 2 : 1;
    pthread_create(&wt, NULL, writer, NULL);
    pthread_create(&mt, NULL, warner, NULL);
    uint64_t t0 = mach_absolute_time();
    for (int c = 0; c < NCPU; c++) pthread_create(&t[c], NULL, cpu_thread, cpu[c]);
    for (int c = 0; c < NCPU; c++) pthread_join(t[c], NULL);
    writing = 0;
    pthread_join(wt, NULL);
    pthread_join(mt, NULL);
    /* The reclaimer may still be on a job (a trim's zone it owes): its last chunk, before the counts are read */
    for (int i = 0; i < 20000; i++) {
        pthread_mutex_lock(&m.tr_mu);
        int busy = m.rc_zone >= 0;
        pthread_mutex_unlock(&m.tr_mu);
        if (!busy) break;
        usleep(100);
    }

    uint64_t emptied = HV_STAT_TOTAL(&m, zone_evictions), sync = HV_STAT_TOTAL(&m, evict_sync),
             reclaims = HV_STAT_TOTAL(&m, reclaims), marks = HV_STAT_TOTAL(&m, reclaim_marks),
             aborts = HV_STAT_TOTAL(&m, reclaim_aborts);
    double quick = (double)m.stats.quick_stop_max_ticks / 24e3, stop_max = (double)m.stats.stop_max_ticks / 24e3,
           grace = (double)m.stats.reclaim_gp_max_ticks / 24e3, hold = (double)m.stats.reclaim_hold_max_ticks / 24e3;
    printf("%.2f s: %llu zones emptied, %llu by the reclaimer, %llu in a stop; %llu marked, %llu given up; %u trims, "
           "%llu zones retired, %llu back; %llu translations\n",
           (mach_absolute_time() - t0) / 24e6, (unsigned long long)emptied, (unsigned long long)reclaims,
           (unsigned long long)sync, (unsigned long long)marks, (unsigned long long)aborts, m.trims,
           (unsigned long long)HV_STAT_TOTAL(&m, zones_retired), (unsigned long long)HV_STAT_TOTAL(&m, zones_returned),
           (unsigned long long)HV_STAT_TOTAL(&m, translations));
    printf("longest stop %.3f ms (with the wait for the others), longest quick stop's own work %.3f ms, longest zone "
           "emptied in a stop %.3f ms, longest grace period %.3f ms, longest tr_mu hold by the reclaimer %.3f ms\n",
           stop_max, quick, (double)m.stats.evict_max_ticks / 24e3, grace, hold);
    int right = 1;
    for (int c = 0; c < NCPU; c++) {
        uint64_t want = (uint64_t)rounds * (STEPS - (uint64_t)c * (STEPS / NCPU));
        if (cpu[c]->x[0] != want || cpu[c]->x[2] != want) {
            printf("cpu %d counted %llu and %llu calls, not %llu\n", c, (unsigned long long)cpu[c]->x[0],
                   (unsigned long long)cpu[c]->x[2], (unsigned long long)want);
            right = 0;
        }
    }
    check(right, "every CPU counted its whole walk, every call returned");
    check(emptied >= 20, "zones were emptied many times with the others running");
    if (nospare) {
        check(sync > 0 && reclaims == 0 && marks == 0, "NOSPARE: every zone emptied in a stop, none by the reclaimer");
    } else {
        check(reclaims > 0 && marks > 0 && reclaims * 2 >= emptied, "the reclaimer emptied most zones, the stops only marked them");
        uint64_t nq = HV_STAT_TOTAL(&m, quick_stops);
        double mean = nq ? (double)HV_STAT_TOTAL(&m, quick_stop_ticks) / 24e3 / (double)nq : 0;
        printf("%llu quick stops, %.4f ms of their own work each\n", (unsigned long long)nq, mean);
        check(nq > 0 && mean < 0.1, "a stop that filled a zone made ready, or marked one, took under 0.1 ms of its own work");
        check(hold < 5.0, "the reclaimer never held tr_mu for long");
    }
    if (!getenv("NOWRITE")) check(HV_STAT_TOTAL(&m, retired) > 0, "writes retired blocks meanwhile");
    if (!getenv("NOWARN")) check(m.trims > 0 && HV_STAT_TOTAL(&m, zones_retired) > 0, "warnings retired zones");
    check(!m.giveback_pending && !m.giveback_zones && !m.giveback_left, "nothing left noted to give back, every trim counted");
    for (int c = 0; c < NCPU; c++) free(cpu[c]);
    hv_machine_free(&m);
    free(ram);
    return failures != 0;
}
