/* Piece-granular SMC invalidation (core/machine.c, dispatch.c, mmu.c):
   1. Sub-page hv_code_pieces partial invalidation: writing one 256-byte piece retires only overlapping blocks
      (including cross-piece blocks) while keeping untouched blocks on the same 16 KB page live and protected;
      HV_CODE_MISS_LIMIT stores to an untouched piece still drop the whole page.
   2. Concurrent writes hitting HV_CODE_RETIRING: two writers rewriting distinct pieces of the same page while
      reader CPUs execute both functions via blr/ijc_resolve/lookup never leave stale code unretired.
   3. lookup/ijc_resolve vs retire_page ordering: invalidating piece 0 while another CPU calls piece 2 never
      creates duplicate live blocks in block_heads[pg] or m->hash. */
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cpu.h"
#include "dispatch.h"
#include "machine.h"
#include "zalloc.h"

#define PAGE (1u << HV_RAM_PAGE_BITS)
#define BASE 0x80000000ull
#define CALLS BASE
#define FPAGE (BASE + PAGE)
#define DATA (FPAGE + 0x2000)
#define FLAG (BASE + 2 * PAGE)
#define STOP (BASE + 3 * PAGE)
#define NPAGES 4

#define F1 (FPAGE + 0x000)
#define F2 (FPAGE + 0x200)
#define F3 (FPAGE + 0x4f8) /* spans piece 4 (0x400..0x4ff) and piece 5 (0x500..0x5ff) */
#define F4 (FPAGE + 0x700)

static HvMachine m;
static uint32_t *ram;
static int failures;

static void check(int ok, const char *what) {
    printf("%s: %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

static uint32_t *at(uint64_t va) { return ram + (va - BASE) / 4; }
static uint32_t branch(uint64_t from, uint64_t to) { return 0x14000000u | ((uint32_t)((int64_t)(to - from) / 4) & 0x03ffffff); }
static uint32_t movz_x0(uint32_t k) { return 0xd2800000u | (k & 0xffffu) << 5; }
static void write32(uint64_t va, uint32_t v) { hv_phys_write(&m, va, &v, 4); }

static HvBlock *block(uint64_t va) {
    HvRam *r = &m.ram[0];
    uint64_t pg = (va - r->pa) >> HV_RAM_PAGE_BITS;
    uint32_t gen = __atomic_load_n(&r->page_gen[pg], __ATOMIC_ACQUIRE);
    return hv_block_find(&m, hv_block_hash(va, va, 0), va, va, 0, gen);
}

static uint64_t run_call(HvCpu *c, uint64_t pc, uint64_t target) {
    c->pc = pc;
    c->x[0] = 0;
    c->x[23] = target;
    int r = hv_run(c);
    if (r != HV_EXIT_HALT || c->pc != STOP) {
        printf("cpu %u stopped with %d at %llx\n", c->index, r, (unsigned long long)c->pc);
        failures++;
    }
    return c->x[0];
}

static int count_live_blocks(uint64_t va) {
    HvRam *r = &m.ram[0];
    uint64_t pg = (va - r->pa) >> HV_RAM_PAGE_BITS;
    int n = 0;
    pthread_mutex_lock(&m.tr_mu);
    for (uint32_t i = r->block_heads[pg]; i;) {
        HvBlock *b = &m.blocks[i - 1];
        i = b->page_next;
        if (b->va == va && b->pa == va && b->flags == 0 && !b->dead) n++;
    }
    pthread_mutex_unlock(&m.tr_mu);
    return n;
}

/* ---- Test 1: sub-page partial invalidation ---- */
static void test_partial_invalidation(void) {
    memset(&m, 0, sizeof m);
    if (hv_machine_init(&m, 64u << 20) != 0) { failures++; return; }
    m.chain = 1;
    ram = aligned_alloc(PAGE, (size_t)NPAGES * PAGE);
    if (!ram || hv_machine_add_ram(&m, BASE, (uint64_t)NPAGES * PAGE, ram, 0) != 0) { failures++; return; }
    for (size_t i = 0; i < (size_t)NPAGES * PAGE / 4; i++) ram[i] = 0xd503201fu;
    at(CALLS)[0] = 0xd63f02e0u; /* blr x23 */
    at(CALLS)[1] = branch(CALLS + 4, STOP);
    at(F1)[0] = movz_x0(11); at(F1)[1] = 0xd65f03c0u;
    at(F2)[0] = movz_x0(22); at(F2)[1] = 0xd65f03c0u;
    /* F3 starts at 0x4f8 (piece 4): nop, nop, movz x0,#33 (at 0x500, piece 5), ret */
    at(F3)[0] = 0xd503201fu; at(F3)[1] = 0xd503201fu; at(F3)[2] = movz_x0(33); at(F3)[3] = 0xd65f03c0u;
    at(F4)[0] = movz_x0(44); at(F4)[1] = 0xd65f03c0u;
    m.stop_pc = STOP;

    HvCpu *c = zalloc(_Alignof(HvCpu), sizeof(HvCpu));
    hv_cpu_reset_index(c, &m, 0);
    c->cpacr_el1 = 3u << 20;
    c->el = 0;
    c->spsel = 0;

    check(run_call(c, CALLS, F1) == 11 && run_call(c, CALLS, F2) == 22 &&
          run_call(c, CALLS, F3) == 33 && run_call(c, CALLS, F4) == 44,
          "all four functions on FPAGE translated and run");
    HvBlock *b1 = block(F1), *b2 = block(F2), *b3 = block(F3), *b4 = block(F4);
    check(b1 && b2 && b3 && b4, "all four blocks found");

    uint64_t tr0 = HV_STAT_TOTAL(&m, translations);
    write32(F1, movz_x0(12));
    check(b1 && b1->dead && (b1->code[0] & 0xfc000000u) == 0x14000000u && !block(F1),
          "write to piece 0 retires F1");
    check(block(F2) == b2 && !b2->dead && b2->code[0] == HV_BLOCK_NOP &&
          block(F3) == b3 && !b3->dead && b3->code[0] == HV_BLOCK_NOP &&
          block(F4) == b4 && !b4->dead && b4->code[0] == HV_BLOCK_NOP,
          "untouched blocks F2, F3, F4 survive");
    check(m.ram[0].code_pages[1] == 1, "code_pages[FPAGE] remains protected");
    check(run_call(c, CALLS, F2) == 22 && HV_STAT_TOTAL(&m, translations) == tr0,
          "F2 runs without retranslation");
    check(run_call(c, CALLS, F1) == 12, "F1 runs new code");

    /* Cross-piece block F3: writing piece 5 retires F3 while F1, F2, F4 survive. */
    b1 = block(F1);
    write32(F3 + 8, movz_x0(34));
    check(b3 && b3->dead && !block(F3), "write to piece 5 retires cross-piece F3");
    check(block(F1) == b1 && block(F2) == b2 && block(F4) == b4, "F1, F2, F4 still survive");
    check(run_call(c, CALLS, F3) == 34, "F3 runs new code");

    /* HV_CODE_MISS_LIMIT misses in an untouched piece still drop the whole page. */
    for (int k = 0; k < HV_CODE_MISS_LIMIT - 1; k++) write32(DATA, (uint32_t)k);
    check(block(F2) == b2 && m.ram[0].code_pages[1] == 1, "one short of miss limit keeps F2");
    write32(DATA, 999);
    check(!block(F1) && !block(F2) && !block(F4) && m.ram[0].code_pages[1] == 0,
          "miss limit drops the whole page");

    free(c);
    free(ram);
    hv_machine_destroy(&m);
}

/* ---- Test 2 & 3: concurrent writes hitting HV_CODE_RETIRING & lookup/ijc_resolve ordering ---- */
static volatile int stop_stress;
static volatile uint32_t *flag1, *flag2;
static uint64_t f1_pc, f2_pc;

struct ReaderArgs {
    HvCpu *cpu;
    int rounds;
    int check_f1_only;
};

static void *reader_thread(void *arg) {
    struct ReaderArgs *a = arg;
    hv_run(a->cpu);
    return NULL;
}

static void *writer_thread(void *arg) {
    int which = (int)(uintptr_t)arg;
    uint64_t target = which == 0 ? f1_pc : f2_pc;
    volatile uint32_t *fl = which == 0 ? flag1 : flag2;
    for (uint32_t k = 1; k <= 1500 && !stop_stress; k++) {
        write32(target, movz_x0(k));
        __atomic_store_n(fl, k, __ATOMIC_RELEASE);
        if ((k & 31) == 0) sched_yield();
    }
    return NULL;
}

static void test_concurrent_retiring_and_ordering(void) {
    memset(&m, 0, sizeof m);
    if (hv_machine_init(&m, 128u << 20) != 0) { failures++; return; }
    m.chain = 1;
    ram = aligned_alloc(PAGE, (size_t)NPAGES * PAGE);
    if (!ram || hv_machine_add_ram(&m, BASE, (uint64_t)NPAGES * PAGE, ram, 0) != 0) { failures++; return; }
    for (size_t i = 0; i < (size_t)NPAGES * PAGE / 4; i++) ram[i] = 0xd503201fu;
    f1_pc = F1;
    f2_pc = F2;
    at(F1)[0] = movz_x0(0); at(F1)[1] = 0xd65f03c0u;
    at(F2)[0] = movz_x0(0); at(F2)[1] = 0xd65f03c0u;

    flag1 = (volatile uint32_t *)at(FLAG);
    flag2 = flag1 + 16;
    *flag1 = 0;
    *flag2 = 0;

    /* Reader loop:
       ldar w1, [x19]; isb; blr x23; cmp w0, w1; b.hs +8; add x24, x24, #1;
       ldar w1, [x20]; isb; blr x22; cmp w0, w1; b.hs +8; add x24, x24, #1;
       subs x21, x21, #1; b.ne loop; b STOP */
    uint32_t *lp = at(CALLS);
    int i = 0;
    int loop = i;
    lp[i++] = 0x88dffe61u; /* ldar w1, [x19] */
    lp[i++] = 0xd5033fdfu; /* isb */
    lp[i++] = 0xd63f02e0u; /* blr x23 (F1) */
    lp[i++] = 0x6b01001fu; /* cmp w0, w1 */
    lp[i++] = 0x54000042u; /* b.hs +8 */
    lp[i++] = 0x91000718u; /* add x24, x24, #1 (stale) */
    lp[i++] = 0x88dffe81u; /* ldar w1, [x20] */
    lp[i++] = 0xd5033fdfu; /* isb */
    lp[i++] = 0xd63f02c0u; /* blr x22 (F2) */
    lp[i++] = 0x6b01001fu; /* cmp w0, w1 */
    lp[i++] = 0x54000042u; /* b.hs +8 */
    lp[i++] = 0x91000718u; /* add x24, x24, #1 (stale) */
    lp[i++] = 0xf10006b5u; /* subs x21, x21, #1 */
    lp[i] = 0x54000001u | ((uint32_t)((int32_t)(loop - i) & 0x7ffff) << 5); i++;
    lp[i++] = branch(CALLS + 4ull * i, STOP);
    m.stop_pc = STOP;

    HvCpu *cpus[2];
    pthread_t rt[2], wt[2];
    struct ReaderArgs rargs[2];
    for (int c = 0; c < 2; c++) {
        cpus[c] = zalloc(_Alignof(HvCpu), sizeof(HvCpu));
        hv_cpu_reset_index(cpus[c], &m, (uint32_t)c);
        cpus[c]->cpacr_el1 = 3u << 20;
        cpus[c]->el = 0;
        cpus[c]->spsel = 0;
        cpus[c]->pc = CALLS;
        cpus[c]->x[19] = FLAG;
        cpus[c]->x[20] = FLAG + 64;
        cpus[c]->x[21] = 20000;
        cpus[c]->x[22] = F2;
        cpus[c]->x[23] = F1;
        cpus[c]->x[24] = 0;
        rargs[c].cpu = cpus[c];
        pthread_create(&rt[c], NULL, reader_thread, &rargs[c]);
    }
    stop_stress = 0;
    pthread_create(&wt[0], NULL, writer_thread, (void *)(uintptr_t)0);
    pthread_create(&wt[1], NULL, writer_thread, (void *)(uintptr_t)1);

    for (int c = 0; c < 2; c++) pthread_join(rt[c], NULL);
    stop_stress = 1;
    pthread_join(wt[0], NULL);
    pthread_join(wt[1], NULL);

    uint64_t stale = cpus[0]->x[24] + cpus[1]->x[24];
    check(stale == 0, "concurrent writes to distinct pieces + readers: zero stale executions");
    check(count_live_blocks(F1) <= 1 && count_live_blocks(F2) <= 1,
          "no duplicate live blocks for F1 or F2");

    for (int c = 0; c < 2; c++) free(cpus[c]);
    free(ram);
    hv_machine_destroy(&m);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    test_partial_invalidation();
    test_concurrent_retiring_and_ordering();
    return failures ? 1 : 0;
}
