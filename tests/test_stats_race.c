/* The engine's counters are free of data races (C11), checked by ThreadSanitizer (this test is built with
   -fsanitize=thread, which fails the run on any it sees): a CPU thread counts into its own counters (HV_CPU_COUNT,
   HV_STAT_ADD on a CPU thread, hv_stat_key) while another thread reads them (HV_STAT_TOTAL, hv_stats_keys' reads),
   as the stats printer, the benchmark and the freeze recorder do. A plain += there was a race: the reads are
   atomic, the writes weren't. Threads with a bare HvCpu (no per-CPU state, as some tests build) count into the
   machine's counters together, atomically, and read their own (HV_CPU_STAT) while the others add to them: a plain
   read there was a race too. Every count must arrive. */
#include "machine.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { NCPU = 2, N = 100000 };
__thread HvCpu *hv_self;
static HvMachine m;
static HvCpu *cpus[2 * NCPU];
static int done, failures;

static void check(const char *what, uint64_t got, uint64_t want) {
    if (got == want) return;
    printf("FAIL %s: got %llu, expected %llu\n", what, (unsigned long long)got, (unsigned long long)want);
    failures++;
}

static void *counter(void *arg) {
    HvCpu *cpu = arg;
    hv_self = cpu;
    uint64_t seen = 0;
    for (uint32_t i = 0; i < N; i++) {
        HV_CPU_COUNT(cpu, blocks, 1);
        HV_CPU_COUNT(cpu, why[i & 3], 2);
        HV_STAT_ADD(&m, dev_waits, 1);
        if (cpu->pc_state) hv_stat_key(cpu->pc_state->flush_pc, i & 7);
        seen += HV_CPU_STAT(cpu, blocks); /* as the dispatcher reads its own (a bare CPU's: the machine's) */
    }
    return (void *)(uintptr_t)seen;
}

/* As hv_stats_keys reads a keyed table */
static uint64_t keyed(void) {
    uint64_t n = 0;
    for (int i = 0; i < HV_MAX_CPUS; i++)
        for (int j = 0; j < 8; j++)
            n += __atomic_load_n(&m.percpu[i].flush_pc[j].key, __ATOMIC_RELAXED) +
                 __atomic_load_n(&m.percpu[i].flush_pc[j].n, __ATOMIC_RELAXED);
    return n;
}

static void *reader(void *unused) {
    (void)unused;
    uint64_t sum = 0;
    while (!__atomic_load_n(&done, __ATOMIC_ACQUIRE))
        sum += HV_STAT_TOTAL(&m, blocks) + HV_STAT_TOTAL(&m, why[1]) + HV_STAT_TOTAL(&m, dev_waits) + keyed();
    return (void *)(uintptr_t)sum;
}

int main(void) {
    for (int c = 0; c < 2 * NCPU; c++) {
        if (posix_memalign((void **)&cpus[c], _Alignof(HvCpu), sizeof(HvCpu))) abort();
        memset(cpus[c], 0, sizeof(HvCpu));
        cpus[c]->m = &m;
        cpus[c]->index = (uint32_t)c;
        if (c < NCPU) cpus[c]->pc_state = &m.percpu[c]; /* the rest: bare, into the machine's */
    }
    pthread_t r, t[2 * NCPU];
    pthread_create(&r, NULL, reader, NULL);
    for (int c = 0; c < 2 * NCPU; c++) pthread_create(&t[c], NULL, counter, cpus[c]);
    for (int c = 0; c < 2 * NCPU; c++) pthread_join(t[c], NULL);
    __atomic_store_n(&done, 1, __ATOMIC_RELEASE);
    pthread_join(r, NULL);

    check("blocks", HV_STAT_TOTAL(&m, blocks), 2 * NCPU * N);
    check("why[1]", HV_STAT_TOTAL(&m, why[1]), 2 * NCPU * N / 4 * 2);
    check("dev_waits", HV_STAT_TOTAL(&m, dev_waits), 2 * NCPU * N);
    for (int c = 0; c < NCPU; c++) {
        uint64_t n = 0;
        for (int j = 0; j < 8; j++) n += m.percpu[c].flush_pc[j].n;
        check("a CPU's keyed table", n, N);
    }
    printf("%s\n", failures ? "test_stats_race FAILED" : "test_stats_race ok");
    return failures != 0;
}
