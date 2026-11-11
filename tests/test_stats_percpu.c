/* The engine's counters with several CPUs at once (core/machine.h HvStats): each CPU thread counts into its own
   (HvPerCpu.stats, no shared cache line, nothing lost), other threads into the machine's atomically, and the readers
   add them up. Also each CPU's exception history (HvPerCpu.trace) holds only that CPU's entries. */
#include "exception.h"
#include "machine.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { NCPU = 4, N = 200000 };

static HvMachine m;
static HvCpu *cpus[NCPU];
static int go;
static int failures;

static void check(const char *what, uint64_t got, uint64_t want) {
    if (got == want) return;
    printf("FAIL %s: got %llu, expected %llu\n", what, (unsigned long long)got, (unsigned long long)want);
    failures++;
}

static void *cpu_thread(void *arg) {
    HvCpu *cpu = arg;
    hv_self = cpu;
    while (!__atomic_load_n(&go, __ATOMIC_ACQUIRE)) {}
    for (uint32_t i = 0; i < N; i++) {
        /* esr carries the CPU's index: the history it leaves must be its own */
        hv_take_exception(cpu, HV_EXC_SYNC, cpu->index, 0, 0, 0x1000 + i * 4);
        cpu->el = 0; /* back to EL0 without hv_eret's bookkeeping */
        HV_STAT_ADD(&m, syncs, 1); /* the shared-code form, on a CPU thread */
    }
    hv_self = NULL;
    return NULL;
}

static void *device_thread(void *arg) {
    (void)arg;
    while (!__atomic_load_n(&go, __ATOMIC_ACQUIRE)) {}
    for (uint32_t i = 0; i < N; i++) HV_STAT_ADD(&m, syncs, 1); /* no CPU of its own: the machine's, atomically */
    return NULL;
}

int main(void) {
    if (hv_machine_init(&m, 8u << 20) != 0) return 1;
    pthread_t t[NCPU + 1];
    for (int c = 0; c < NCPU; c++) {
        if (posix_memalign((void **)&cpus[c], _Alignof(HvCpu), sizeof(HvCpu))) abort(); /* calloc's 16 is too little */
        memset(cpus[c], 0, sizeof(HvCpu));
        hv_cpu_reset_index(cpus[c], &m, (uint32_t)c);
        m.cpus[c] = cpus[c];
    }
    m.ncpus = NCPU;
    for (int c = 0; c < NCPU; c++) pthread_create(&t[c], NULL, cpu_thread, cpus[c]);
    pthread_create(&t[NCPU], NULL, device_thread, NULL);
    __atomic_store_n(&go, 1, __ATOMIC_RELEASE);
    for (int c = 0; c <= NCPU; c++) pthread_join(t[c], NULL);

    check("exceptions, every CPU's counted", HV_STAT_TOTAL(&m, exceptions), (uint64_t)NCPU * N);
    check("syncs from CPU threads and another thread", HV_STAT_TOTAL(&m, syncs), (uint64_t)(NCPU + 1) * N);
    HvStats sum;
    hv_stats_sum(&m, &sum);
    check("hv_stats_sum agrees", sum.exceptions + sum.syncs, (uint64_t)NCPU * N + (uint64_t)(NCPU + 1) * N);
    check("the device thread's count is the machine's", m.stats.syncs, N);
    for (int c = 0; c < NCPU; c++) {
        const HvPerCpu *p = cpus[c]->pc_state;
        check("a CPU's own count", p->stats.exceptions, N);
        check("a CPU's history: all its entries", p->trace.nelchg, N);
        for (int k = 0; k < 16; k++) check("a CPU's history: only its own", p->trace.elchg[k].to, (uint64_t)c);
    }
    /* Keyed tables merge by key across CPUs. */
    hv_stat_key(m.percpu[0].flush_pc, 0x40);
    hv_stat_key(m.percpu[1].flush_pc, 0x40);
    hv_stat_key(m.percpu[1].flush_pc, 0x80);
    hv_stat_key(m.percpu[1].flush_pc, 0x40);
    HvStatKey keys[8];
    hv_stats_keys(&m, offsetof(HvPerCpu, flush_pc), keys);
    check("keyed: first key", keys[0].key, 0x40);
    check("keyed: its count, both CPUs", keys[0].n, 3);
    check("keyed: second key", keys[1].key, 0x80);
    check("keyed: no third", keys[2].n, 0);
    printf("%s\n", failures ? "test_stats_percpu FAILED" : "test_stats_percpu ok");
    return failures != 0;
}
