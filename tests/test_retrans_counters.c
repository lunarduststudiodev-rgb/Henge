// Why code is translated again, and what writes to code throw out (core/machine.h HvStats: retrans_evicted,
// retrans_written, lost_races, flush_smc, retired, retired_words, rewrite_same, rewrite_changed, and the dispatcher's
// entries before death: entries_written, entries_evicted), counted with the statistics off as the bench reads them:
// each moves on its own event.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include "dispatch.h"
#include "machine.h"
#include "zalloc.h"

#define PAGE (1u << HV_RAM_PAGE_BITS)
#define BASE 0x80000000ull
#define A BASE                      /* page 0: add x0, x0, #1; b STOP */
#define B (BASE + PAGE)             /* page 1: the same */
#define STOP (BASE + 2 * PAGE)      /* never translated: the dispatcher stops there */
#define CHAIN (BASE + 3 * PAGE)     /* pages of add x0, x0, #1; b next (2048 blocks a page), then b STOP */
#define NCHAIN 8
#define NPAGES (3 + NCHAIN)
#define NCPU 4

static HvMachine m;
static HvCpu *cpu[NCPU];
static uint32_t *ram;
static int failures;

static void check(int ok, const char *what) {
    printf("%s: %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

static uint32_t *at(uint64_t va) { return ram + (va - BASE) / 4; }
static uint32_t branch(uint64_t from, uint64_t to) { return 0x14000000u | ((uint32_t)((int64_t)(to - from) / 4) & 0x03ffffff); }

static void start(HvCpu *c, int index, uint64_t pc) {
    hv_cpu_reset_index(c, &m, (uint32_t)index);
    c->cpacr_el1 = 3u << 20;
    c->el = 0, c->spsel = 0;
    c->pc = pc;
    c->x[0] = 0;
}

static void *cpu_thread(void *arg) {
    HvCpu *c = arg;
    int r = hv_run(c);
    if (r != HV_EXIT_HALT || c->pc != STOP) {
        printf("cpu %u stopped with %d at %llx\n", c->index, r, (unsigned long long)c->pc);
        __atomic_add_fetch(&failures, 1, __ATOMIC_RELAXED);
    }
    return NULL;
}

static uint64_t run(uint64_t pc) {
    start(cpu[0], 0, pc);
    cpu_thread(cpu[0]);
    return cpu[0]->x[0];
}

static HvBlock *block(uint64_t va) {
    HvRam *r = &m.ram[0];
    return hv_block_find(&m, hv_block_hash(va, va, 0), va, va, 0, r->page_gen[(va - r->pa) >> HV_RAM_PAGE_BITS]);
}

#define T(f) HV_STAT_TOTAL(&m, f)

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (hv_machine_init(&m, 64u << 20) != 0) return 1; /* 8 MB zones: each CPU places code in 1 MB of one at a time */
    m.chain = 1;
    m.stats_interval = 0; /* always counted */
    m.rewrite_hash = 1;   /* HvStats.rewrite_* (off unless asked for) */
    hv_machine_set_zones(&m, 8);
    ram = aligned_alloc(PAGE, (size_t)NPAGES * PAGE);
    if (!ram || hv_machine_add_ram(&m, BASE, (uint64_t)NPAGES * PAGE, ram, 0) != 0) return 1;
    for (size_t i = 0; i < (size_t)NPAGES * PAGE / 4; i++) ram[i] = 0xd503201f; /* nop */
    at(A)[0] = at(B)[0] = 0x91000400; /* add x0, x0, #1 */
    at(A)[1] = branch(A + 4, STOP);
    at(B)[1] = branch(B + 4, STOP);
    uint64_t nchain = (uint64_t)NCHAIN * PAGE / 8;
    for (uint64_t i = 0; i < nchain; i++) {
        uint64_t va = CHAIN + i * 8;
        at(va)[0] = 0x91000400;
        at(va)[1] = branch(va + 4, i + 1 < nchain ? va + 8 : STOP);
    }
    m.stop_pc = STOP;
    for (int c = 0; c < NCPU; c++) cpu[c] = zalloc(_Alignof(HvCpu), sizeof(HvCpu));

    /* Emptied with its page unchanged, then run again: made again as evicted. */
    check(run(A) == 1 && block(A), "A runs");
    int za = block(A)->zone;
    uint64_t ev = T(retrans_evicted), wr = T(retrans_written), tr = T(translations), once = T(entries_evicted[0]);
    hv_jit_make_room(&m, -1);
    if (m.zone == za) hv_jit_make_room(&m, -1);
    hv_jit_make_room(&m, za);
    check(!block(A), "emptying its zone throws A out");
    check(T(entries_evicted[0]) == once + 1, "entries_evicted counts it entered once");
    check(run(A) == 1 && T(translations) == tr + 1, "A runs, translated again");
    check(T(retrans_evicted) == ev + 1 && T(retrans_written) == wr, "retrans_evicted counts it, retrans_written not");

    /* A write to its page: retired, the page dropped, then made again as written. The same bytes first. */
    check(run(B) == 1 && block(B), "B runs");
    uint32_t words = block(B)->words;
    uint64_t drops = T(flush_smc), ret = T(retired), rw = T(retired_words), same = T(rewrite_same), changed = T(rewrite_changed);
    ev = T(retrans_evicted), wr = T(retrans_written);
    uint32_t add1 = 0x91000400, add2 = 0x91000800; /* add x0, x0, #1 / #2 */
    uint64_t wonce = T(entries_written[0]), wmany = T(entries_written[2]);
    hv_phys_write(&m, B, &add1, 4);
    check(!block(B), "writing the same instruction over B retires it");
    check(T(entries_written[0]) == wonce + 1, "entries_written counts it entered once");
    check(T(flush_smc) == drops + 1 && T(retired) == ret + 1 && T(retired_words) == rw + words,
          "flush_smc counts the page dropped, retired and retired_words its block and its code");
    check(T(rewrite_same) == same + 1 && T(rewrite_changed) == changed, "rewrite_same counts the bytes unchanged");
    check(run(B) == 1, "B runs, translated again");
    check(T(retrans_written) == wr + 1 && T(retrans_evicted) == ev, "retrans_written counts it, retrans_evicted not");
    /* Then other bytes, after the dispatcher entered it 10 times. */
    for (int i = 0; i < 9; i++) run(B);
    check(block(B) && block(B)->entries == 10, "the dispatcher entered B 10 times");
    hv_phys_write(&m, B, &add2, 4);
    check(T(entries_written[2]) == wmany + 1 && T(entries_written[0]) == wonce + 1, "entries_written counts it in 9-64");
    check(T(rewrite_changed) == changed + 1 && T(rewrite_same) == same + 1, "rewrite_changed counts the bytes changed");
    check(run(B) == 2 && T(retrans_written) == wr + 2, "B runs the new code, made again as written");

    /* CPUs walking the same new code at once: some translate a block another places first, and throw theirs away. */
    uint64_t lost = T(lost_races);
    int tries = 0;
    for (; tries < 50 && T(lost_races) == lost; tries++) {
        hv_flush_blocks(&m);
        pthread_t t[NCPU];
        for (int c = 0; c < NCPU; c++) start(cpu[c], c, CHAIN);
        for (int c = 0; c < NCPU; c++) pthread_create(&t[c], NULL, cpu_thread, cpu[c]);
        for (int c = 0; c < NCPU; c++) pthread_join(t[c], NULL);
        for (int c = 0; c < NCPU; c++)
            if (cpu[c]->x[0] != nchain) {
                printf("cpu %d counted %llu, not %llu\n", c, (unsigned long long)cpu[c]->x[0], (unsigned long long)nchain);
                failures++;
            }
    }
    printf("%d walks of %llu new blocks by %d CPUs at once: %llu lost races\n", tries, (unsigned long long)nchain, NCPU,
           (unsigned long long)(T(lost_races) - lost));
    check(T(lost_races) > lost, "lost_races counts translations another CPU's copy beat");

    for (int c = 0; c < NCPU; c++) free(cpu[c]);
    hv_machine_free(&m);
    free(ram);
    if (failures) printf("test_retrans_counters: %d failures\n", failures);
    return failures != 0;
}
