/* Code changed by one thread while other CPUs keep calling it (the Nov 2026
   audit's stale-code race). hv_phys_write, like every device write, must copy
   the new bytes before it drops the page's translations: invalidating first let
   a CPU translate the old bytes in between and stamp them with the new
   generation, so it ran the old code from then on.

   Readers (HvCpus on threads) loop:  ldar w1,[flag]; isb; blr F; (x0 < w1 ? stale++); stlr w0,[ack]
   F is "movz x0,#k; ret" in its own 16 KB page. The writer rewrites the movz to
   k, then release-stores flag = k, then waits for every reader to ack k. A
   reader that sees flag == k and still runs the old movz after an ISB is stale.

   Each round also chains `links` unused slots into F's page, as a busy guest
   has many exits chained into a hot page: unlinking them makes invalidation
   slow, which widens the window the wrong order leaves open (the test fails
   within a few rounds if hv_phys_write invalidates before it copies).

   No kicks by default: nothing but the write itself may get a reader to the
   new code (a kick every kick_us microseconds would hide a CPU that only
   leaves stale code when made to).

   A second pass makes the rewritten code the readers' own loop: F's page holds
       loop: ldar w1,[flag]; isb; movz x0,#k; cmp; b.hs; add x24; stlr; cmp; b.lo loop
   so a reader never comes back to the dispatcher by itself (the back edge and
   the exit after the isb are chained in the page), and only the gate a write
   puts on the blocks can take it to the new movz.

   usage: test_smc_race [readers] [rounds] [kick_us] [links] [call|loop] */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach_time.h>
#include "cpu.h"
#include "dispatch.h"
#include "machine.h"
#include "zalloc.h"

static HvMachine m;
static uint8_t *buf;
#define PG 16384
static uint32_t *code0, *fpage; static volatile uint32_t *flag, *ack;
static uint64_t stop_at;
static int nread, rounds, kick_us, selfloop;
static volatile int done;

static void *reader(void *arg) {
    HvCpu *cpu = arg;

    int r = hv_run(cpu);
    if (r != HV_EXIT_HALT) fprintf(stderr, "cpu %u: exit %d pc %llx\n", cpu->index, r, (unsigned long long)cpu->pc);
    return NULL;
}
/* Chain n unused slots (the top of the table, never handed out here) into F's page. */
static void add_links(uint32_t n) {
    HvRam *r = hv_ram_lookup(&m, (uint64_t)(uintptr_t)fpage);
    uint32_t pg = (uint32_t)(((uint64_t)(uintptr_t)fpage - r->pa) >> HV_RAM_PAGE_BITS);
    if (n > m.max_slots / 2) n = m.max_slots / 2;
    pthread_mutex_lock(&m.tr_mu);
    for (uint32_t k = 0; k < n; k++) {
        uint32_t idx = m.max_slots - 1 - k;
        if (m.slot_xpg[idx]) continue; /* still linked from the last round */
        m.slot_site[idx] = NULL;
        hv_xlist_insert(&m, r, pg, idx);
    }
    pthread_mutex_unlock(&m.tr_mu);
}

static void *kicker(void *u) { (void)u; while (!done) { usleep(kick_us); hv_machine_kick_all(&m); } return NULL; }

static int pass(uint32_t links);

int main(int argc, char **argv) {
    nread = argc > 1 ? atoi(argv[1]) : 3;
    rounds = argc > 2 ? atoi(argv[2]) : 1000;
    kick_us = argc > 3 ? atoi(argv[3]) : 0;
    uint32_t links = argc > 4 ? (uint32_t)atoi(argv[4]) : 50000;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 5) { selfloop = !strcmp(argv[5], "loop"); return pass(links); }
    int bad = 0;
    for (selfloop = 0; selfloop < 2; selfloop++) bad |= pass(links);
    return bad;
}

static int pass(uint32_t links) {
    memset(&m, 0, sizeof m);
    done = 0;
    if (hv_machine_init(&m, 256u << 20) != 0) return 1;
    m.chain = 1;
    buf = aligned_alloc(PG, 4 * PG); memset(buf, 0, 4 * PG);
    hv_machine_add_ram(&m, (uint64_t)(uintptr_t)buf, 4 * PG, buf, 0);
    code0 = (uint32_t *)buf; fpage = (uint32_t *)(buf + PG);
    flag = (volatile uint32_t *)(buf + 2 * PG); ack = flag + 16;
    /* F */
    fpage[0] = 0xd2800000; /* movz x0,#0 */
    fpage[1] = 0xd65f03c0; /* ret */
    /* reader loop: in code0, calling F; or in F's page, in place of F */
    uint32_t *lp = selfloop ? fpage : code0, *movz = fpage;
    int i = 0;
    uint32_t loop = i;
    lp[i++] = 0x88dffe61;                /* ldar w1,[x19] */
    lp[i++] = 0xd5033fdf;                /* isb */
    if (selfloop) movz = &lp[i], lp[i++] = 0xd2800000; /* movz x0,#k: the instruction rewritten */
    else lp[i++] = 0xd63f02e0;           /* blr x23 */
#define code0 lp
    code0[i++] = 0x6b01001f;             /* cmp w0,w1 */
    code0[i++] = 0x54000042;             /* b.hs +8 */
    code0[i++] = 0x91000718;             /* add x24,x24,#1 */
    code0[i++] = 0x889ffec0;             /* stlr w0,[x22] */
    code0[i++] = 0x7100001f | ((uint32_t)4000 << 10) | (1u << 5); /* cmp w1,#4000 */
    code0[i] = 0x54000003 | ((uint32_t)((int32_t)(loop - i) & 0x7ffff) << 5); i++; /* b.lo loop */
    stop_at = (uint64_t)(uintptr_t)&code0[i];
    code0[i++] = 0xd503201f;
#undef code0
    m.stop_pc = stop_at;
    uint8_t *stack = aligned_alloc(PG, 8 * PG);
    hv_machine_add_ram(&m, (uint64_t)(uintptr_t)stack, 8 * PG, stack, 0);
    HvCpu **cpus = calloc(nread, sizeof *cpus);
    pthread_t *t = calloc(nread, sizeof *t);
    for (int c = 0; c < nread; c++) {
        cpus[c] = zalloc(_Alignof(HvCpu), sizeof(HvCpu));
        hv_cpu_reset_index(cpus[c], &m, c);
        cpus[c]->cpacr_el1 = 3u << 20;
        cpus[c]->el = 0, cpus[c]->spsel = 0;
        cpus[c]->x[19] = (uint64_t)(uintptr_t)flag;
        cpus[c]->x[22] = (uint64_t)(uintptr_t)(ack + c);  /* one ack word each */
        cpus[c]->x[23] = (uint64_t)(uintptr_t)fpage;
        cpus[c]->x[24] = 0;
        cpus[c]->sp = (uint64_t)(uintptr_t)stack + (c + 1) * PG - 64;
        cpus[c]->pc = (uint64_t)(uintptr_t)lp;
    }
    for (int c = 0; c < nread; c++) pthread_create(&t[c], NULL, reader, cpus[c]);
    pthread_t kt;
    if (kick_us > 0) pthread_create(&kt, NULL, kicker, NULL);
    usleep(20000);
    int stuck_rounds = 0, slow_rounds = 0, ran = 0;
    uint64_t t0 = mach_absolute_time(), wsum = 0, slowest = 0;
    for (uint32_t k = 1; k <= (uint32_t)rounds && k < 4000; k++) {
        uint32_t insn = 0xd2800000 | (k << 5);
        add_links(links);
        uint64_t w0 = mach_absolute_time();
        hv_phys_write(&m, (uint64_t)(uintptr_t)movz, &insn, 4);
        wsum += mach_absolute_time() - w0;
        ran++;
        __atomic_store_n(flag, k, __ATOMIC_RELEASE);
        /* Every reader must come to return k. A reader on the old code never does: 2 s without it is stuck. A slow
           round (a reader late by over 50 ms) is the host's scheduling under load, not stale code (the stale count
           says that): printed, not failed. */
        uint64_t f0 = mach_absolute_time(), until = f0 + 24000 * 2000;
        int bad = 0;
        for (int c = 0; c < nread; c++)
            while (__atomic_load_n(&ack[c], __ATOMIC_ACQUIRE) < k)
                if (mach_absolute_time() > until) { bad++; break; }
        uint64_t waited = mach_absolute_time() - f0;
        if (waited > slowest) slowest = waited;
        if (bad) {
            stuck_rounds++;
            fprintf(stderr, "round %u: %d reader(s) still run the old code 2 s after flag=%u: stopping\n", k, bad, k);
            break; /* each further round would wait its 2 s out too */
        } else if (waited > 24000 * 50 && ++slow_rounds <= 5)
            fprintf(stderr, "round %u: slow, the last reader came to the new code after %.1f ms\n", k, waited / 24e3);
    }
    /* The last round as the others: code first, then the flag that ends the loop. */
    { uint32_t insn = 0xd2800000 | (4000u << 5); hv_phys_write(&m, (uint64_t)(uintptr_t)movz, &insn, 4); }
    __atomic_store_n(flag, 4000, __ATOMIC_RELEASE);
    done = 1;
    for (int c = 0; c < nread; c++) pthread_join(t[c], NULL);
    if (kick_us > 0) pthread_join(kt, NULL);
    uint64_t stale = 0; for (int c = 0; c < nread; c++) stale += cpus[c]->x[24];
    printf("%s: readers %d rounds %d of %d kick %d us links %u: stuck rounds %d, slow rounds %d (slowest %.1f ms), "
           "stale calls after ISB %llu, hv_phys_write avg %.1f us, %.1f s, translations %llu\n",
           selfloop ? "own loop" : "call", nread, ran, rounds, kick_us, links, stuck_rounds, slow_rounds, slowest / 24e3,
           (unsigned long long)stale, ran ? wsum / 24.0 / ran : 0.0,
           (mach_absolute_time() - t0) / 24e6, (unsigned long long)HV_STAT_TOTAL(&m, translations));
    for (int c = 0; c < nread; c++) free(cpus[c]);
    free(cpus), free(t), free(stack), free(buf);
    hv_machine_free(&m);
    return stuck_rounds != 0 || stale != 0;
}
