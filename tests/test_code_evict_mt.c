// Emptying zones while several CPUs run, code is written and memory warnings come (core/dispatch.c,
// stop_the_world, make_room_from, hv_machine_give_back): every CPU walks a chain of more blocks than the code cache
// holds, so zones are emptied again and again with the others stopped; one thread keeps rewriting words of the
// chain (with the same instruction, so the answer stays the same, but the blocks are retired); another sends
// memory warnings, so stops retire zones and give their tables back after, and zones come back between warnings.
// Emptied code is zeroed (HV_JIT_POISON), so a CPU left running in, or returning into, an emptied zone faults.
// Each CPU must end with the count its walk makes, and nothing may hang. It found a CPU giving up on a block
// ("cannot translate") while a stop waited for it: the check after a failed lookup read flush_pending unordered.
// NOWRITE=1 / NOWARN=1 leave out the writer / the warnings. usage: test_code_evict_mt [walks]
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
#define NCHAIN 24                   /* pages of chain: add x0, x0, #1; b next (2048 blocks a page) */
#define STOP (BASE + NCHAIN * PAGE) /* never translated: the dispatcher stops there */
#define LOOP (STOP + PAGE)          /* the walk's start: blr x23 into the chain; subs; b.ne; b STOP */
#define NPAGES (NCHAIN + 2)
#define NCPU 4
#define BLOCKS (NCHAIN * PAGE / 8)

static HvMachine m;
static uint32_t *ram;
static int failures;
static volatile int writing;
static int rounds = 12; /* walks of the chain each: argv[1] (at 4 the unordered read above was seldom caught) */

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

/* The same add again, at a block's start somewhere along the chain: its page's blocks are retired. */
static void *writer(void *u) {
    (void)u;
    uint32_t seed = 1, add = 0x91000400;
    while (writing == 1) {
        seed = seed * 1103515245u + 12345u;
        hv_phys_write(&m, BASE + (uint64_t)(seed >> 8) % BLOCKS * 8, &add, 4);
        usleep(200);
    }
    return NULL;
}

static void *warner(void *u) {
    (void)u;
    while (writing && !getenv("NOWARN")) {
        hv_machine_request_trim(&m, 0);
        usleep(3000);
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc > 1) rounds = atoi(argv[1]);
    setvbuf(stdout, NULL, _IONBF, 0);
    setenv("HV_JIT_POISON", "1", 1);
    if (hv_machine_init(&m, 8u << 20) != 0) return 1;
    m.chain = 1;
    hv_machine_set_zones(&m, 8);
    m.trim_quiet_ns = 2000000; /* zones come back 2 ms after the last warning */
    ram = aligned_alloc(PAGE, (size_t)NPAGES * PAGE);
    if (!ram || hv_machine_add_ram(&m, BASE, (uint64_t)NPAGES * PAGE, ram, 0) != 0) return 1;
    for (uint64_t i = 0; i < BLOCKS; i++) {
        at(BASE + i * 8)[0] = 0x91000400; /* add x0, x0, #1 */
        at(BASE + i * 8)[1] = i + 1 < BLOCKS ? branch(BASE + i * 8 + 4, BASE + i * 8 + 8) : 0xd65f03c0; /* b next; ret */
    }
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
        cpu[c]->x[0] = 0, cpu[c]->x[1] = (uint64_t)rounds;
        /* each starts somewhere else along the chain, so the CPUs fill and use different zones */
        cpu[c]->x[23] = BASE + (uint64_t)c * (BLOCKS / NCPU) * 8;
    }
    writing = 1;
    if (getenv("NOWRITE")) writing = 2;
    pthread_create(&wt, NULL, writer, NULL);
    pthread_create(&mt, NULL, warner, NULL);
    uint64_t t0 = mach_absolute_time();
    for (int c = 0; c < NCPU; c++) pthread_create(&t[c], NULL, cpu_thread, cpu[c]);
    for (int c = 0; c < NCPU; c++) pthread_join(t[c], NULL);
    writing = 0;
    pthread_join(wt, NULL);
    pthread_join(mt, NULL);
    /* The reclaimer may still be on a job (a trim's zone it owes, counted in giveback_left): its last chunk first */
    for (int i = 0; i < 20000; i++) {
        pthread_mutex_lock(&m.tr_mu);
        int busy = m.rc_zone >= 0;
        pthread_mutex_unlock(&m.tr_mu);
        if (!busy) break;
        usleep(100);
    }

    printf("%.2f s: %llu zones emptied (%llu live blocks, %llu retired), %llu blocks retired by writes, "
           "%u trims, %llu zones retired, %llu back, %llu translations\n",
           (mach_absolute_time() - t0) / 24e6, (unsigned long long)HV_STAT_TOTAL(&m, zone_evictions),
           (unsigned long long)HV_STAT_TOTAL(&m, evict_live), (unsigned long long)HV_STAT_TOTAL(&m, evict_dead),
           (unsigned long long)HV_STAT_TOTAL(&m, retired), m.trims, (unsigned long long)HV_STAT_TOTAL(&m, zones_retired),
           (unsigned long long)HV_STAT_TOTAL(&m, zones_returned), (unsigned long long)HV_STAT_TOTAL(&m, translations));
    int right = 1;
    for (int c = 0; c < NCPU; c++) {
        uint64_t want = (uint64_t)rounds * (BLOCKS - (uint64_t)c * (BLOCKS / NCPU));
        if (cpu[c]->x[0] != want) {
            printf("cpu %d counted %llu, not %llu\n", c, (unsigned long long)cpu[c]->x[0], (unsigned long long)want);
            right = 0;
        }
    }
    check(right, "every CPU counted its whole walk");
    check(HV_STAT_TOTAL(&m, zone_evictions) >= 20, "zones were emptied many times with the others running");
    check(HV_STAT_TOTAL(&m, retired) > 0, "writes retired blocks meanwhile");
    check(m.trims > 0 && HV_STAT_TOTAL(&m, zones_retired) > 0 && HV_STAT_TOTAL(&m, giveback_ticks) > 0, "warnings retired zones and gave their tables back");
    check(HV_STAT_TOTAL(&m, zones_returned) > 0, "and zones came back between warnings");
    check(!m.giveback_pending && !m.giveback_zones && !m.giveback_left, "nothing left noted to give back, every trim counted");
    for (int c = 0; c < NCPU; c++) free(cpu[c]);
    hv_machine_free(&m);
    free(ram);
    return failures != 0;
}
