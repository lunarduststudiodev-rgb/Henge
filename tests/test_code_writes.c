// Writes into guest code pages (core/machine.c, hv_phys_written and invalidate_page) and the code cache making room
// (core/dispatch.c, make_room_from and evict_zone):
//   1. a write to code retires just that page's blocks: each one's entry goes to its gate, out of the hash, and no
//      other CPU is asked to leave or to start its branch caches over;
//   2. whatever still leads to a retired block (a branch cache, a return table, a chained exit) reaches new code,
//      a return into the caller's code after the callee rewrote it included;
//   3. a write to data on a code page that misses every piece code came from keeps the code, and after
//      HV_CODE_MISS_LIMIT such guest stores the page is dropped, so the stores after them go the fast way;
//   4. emptying a zone makes its chained exits' code changes all at once, not one cache maintenance each;
//   5. a zone whose blocks are all retired is emptied first, before one whose blocks are still in use;
//   6. a CPU calling code two threads keep rewriting neither hangs nor runs what was never written;
//   7. a write that lands while its page's code is being translated: the block runs that once and is not kept;
//      the code a write retires stops counting as held; a device's write anywhere on a code page drops its code.
// Each fails on the code before (base c395831). With -DBENCH it also times emptying a zone with many links into it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach_time.h>
#include <pthread.h>

#include "dispatch.h"
#include "machine.h"
#include "evict_lists.h"
#include "zalloc.h"

#define PAGE (1u << HV_RAM_PAGE_BITS)
#define BASE 0x80000000ull
#define CALLS BASE                 /* page 0: the callers */
#define FPAGE (BASE + PAGE)        /* page 1: F, and data far from it (another piece) */
#define F FPAGE
#define LOOPF (FPAGE + 0x100)      /* a loop on F's page, chained to itself */
#define DATA (FPAGE + 0x2000)
#define GPAGE (BASE + 2 * PAGE)    /* page 2: G */
#define G GPAGE
#define STOP (BASE + 3 * PAGE)     /* never translated: the dispatcher stops there */
#define LINKS (BASE + 4 * PAGE)    /* pages of linked pairs, for emptying a zone (4) */
#define NLINKPAGES 8
#define PAIRS (PAGE / 32)          /* a pair is 8 words */
#define RPAGE (LINKS + NLINKPAGES * PAGE) /* page for zone choice (5): retired */
#define SPAGE (RPAGE + PAGE)              /* and live */
#define RCPAGE (SPAGE + PAGE)             /* a caller whose code after the call its callee rewrites (2b) */
#define HPAGE (RCPAGE + PAGE)             /* and the callees */
#define NPAGES (8 + NLINKPAGES)

/* Callers on page 0. */
#define C_CALL (CALLS + 0x00)      /* blr x23; b STOP */
#define C_LOOP (CALLS + 0x40)      /* L: blr x23; subs x1, x1, #1; b.ne L; b STOP */
#define C_STORE (CALLS + 0x80)     /* str w2, [x21]; b STOP */
#define C_ATOMIC (CALLS + 0xc0)    /* ldadd w2, w3, [x21]; b STOP */
#define C_SLOOP (CALLS + 0x100)    /* L: str w2, [x21]; subs x1, x1, #1; b.ne L; b STOP */
#ifndef HV_CODE_MISS_LIMIT
#define HV_CODE_MISS_LIMIT 64
#endif

static HvMachine machine;
static HvCpu *cpu[2];
static uint32_t *ram;
static int failures;

static void check(int ok, const char *what) {
    printf("%s: %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

static uint32_t *at(uint64_t va) { return ram + (va - BASE) / 4; }
static uint32_t branch(uint64_t from, uint64_t to) { return 0x14000000u | ((uint32_t)((int64_t)(to - from) / 4) & 0x03ffffff); }
static uint32_t movz_x0(uint32_t k) { return 0xd2800000u | k << 5; }

static uint64_t run(int c, uint64_t pc, uint64_t x1, uint64_t x23) {
    HvCpu *p = cpu[c];
    p->pc = pc;
    p->x[0] = 0, p->x[1] = x1, p->x[2] = 1, p->x[21] = DATA, p->x[23] = x23;
    int reason = hv_run(p);
    if (reason != HV_EXIT_HALT || p->pc != STOP) {
        printf("cpu %d from %llx stopped with %d at %llx\n", c, (unsigned long long)pc, reason, (unsigned long long)p->pc);
        failures++;
    }
    return p->x[0];
}

static HvBlock *block(uint64_t va) {
    HvRam *r = &machine.ram[0];
    uint64_t pg = (va - r->pa) >> HV_RAM_PAGE_BITS;
    return hv_block_find(&machine, hv_block_hash(va, va, 0), va, va, 0, r->page_gen[pg]);
}

/* A write as C makes it: the bytes, then the code noticed (hv_phys_write). */
static void write32(uint64_t va, uint32_t v) { hv_phys_write(&machine, va, &v, 4); }

/* Code-side cache maintenance, counted: the engine's calls come here instead of the system's. Armed (7), the next
   one writes F: as the translator places F's new code, after it has read the bytes, before it publishes. */
static uint64_t icache_calls;
static uint32_t poke_word;
static void write32(uint64_t va, uint32_t v);
void sys_icache_invalidate(void *start, size_t len) {
    icache_calls++;
    if (poke_word) {
        uint32_t v = poke_word;
        poke_word = 0;
        write32(F, v);
    }
    uintptr_t a = (uintptr_t)start & ~(uintptr_t)63, end = (uintptr_t)start + len;
    __asm__ volatile("dsb ish" ::: "memory");
    for (; a < end; a += 64) __asm__ volatile("ic ivau, %0" ::"r"(a) : "memory");
    __asm__ volatile("dsb ish\n\tisb" ::: "memory");
}

/* 6: two threads keep rewriting F (movz x0, #4 / #5) until told to stop. */
static volatile int writing;
static void *rewriter(void *arg) {
    uint32_t k = (uint32_t)(uintptr_t)arg, n = 0;
    while (writing) write32(F, movz_x0(4 + (k + n++) % 2));
    return NULL;
}

static int quiet(void) { /* nobody asked to leave, or to start its branch caches over */
    for (int c = 0; c < 2; c++) {
        if (__atomic_load_n(&cpu[c]->exit_req, __ATOMIC_SEQ_CST)) return 0;
#ifdef HV_REQ_IJC_RESET /* the code before: every CPU's branch cache dropped by request */
        if (cpu[c]->pc_state->req_flags & HV_REQ_IJC_RESET) return 0;
#endif
    }
    return 1;
}

static void calm(void) {
    for (int c = 0; c < 2; c++) {
        cpu[c]->exit_req = 0;
        cpu[c]->pc_state->req_flags = 0;
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (hv_machine_init(&machine, 64u << 20) != 0) return 1;
    machine.chain = 1;
    ram = aligned_alloc(PAGE, (size_t)NPAGES * PAGE);
    if (!ram || hv_machine_add_ram(&machine, BASE, (uint64_t)NPAGES * PAGE, ram, 0) != 0) return 1;
    for (size_t i = 0; i < (size_t)NPAGES * PAGE / 4; i++) ram[i] = 0xd503201f; /* nop */
    at(C_CALL)[0] = 0xd63f02e0, at(C_CALL)[1] = branch(C_CALL + 4, STOP);
    at(C_LOOP)[0] = 0xd63f02e0;                                         /* L: blr x23 */
    at(C_LOOP)[1] = 0xf1000421;                                         /* subs x1, x1, #1 */
    at(C_LOOP)[2] = 0x54000001 | (uint32_t)(-2 & 0x7ffff) << 5;         /* b.ne L */
    at(C_LOOP)[3] = branch(C_LOOP + 12, STOP);
    at(C_STORE)[0] = 0xb90002a2, at(C_STORE)[1] = branch(C_STORE + 4, STOP);   /* str w2, [x21] */
    at(C_ATOMIC)[0] = 0xb82202a3, at(C_ATOMIC)[1] = branch(C_ATOMIC + 4, STOP); /* ldadd w2, w3, [x21] */
    at(C_SLOOP)[0] = 0xb90002a2;                                        /* L: str w2, [x21] */
    at(C_SLOOP)[1] = 0xf1000421;                                        /* subs x1, x1, #1 */
    at(C_SLOOP)[2] = 0x54000001 | (uint32_t)(-2 & 0x7ffff) << 5;        /* b.ne L */
    at(C_SLOOP)[3] = branch(C_SLOOP + 12, STOP);
    at(F)[0] = movz_x0(1), at(F)[1] = 0xd65f03c0;                       /* movz x0, #1; ret */
    at(G)[0] = movz_x0(7), at(G)[1] = 0xd65f03c0;
    at(LOOPF)[0] = 0x91000400;                                          /* add x0, x0, #1 */
    at(LOOPF)[1] = 0xf1000421;                                          /* subs x1, x1, #1 */
    at(LOOPF)[2] = 0x54000001 | (uint32_t)(-2 & 0x7ffff) << 5;          /* b.ne LOOPF */
    at(LOOPF)[3] = branch(LOOPF + 12, STOP);
    for (int c = 0; c < 2; c++) {
        cpu[c] = zalloc(_Alignof(HvCpu), sizeof(HvCpu));
        hv_cpu_reset_index(cpu[c], &machine, (uint32_t)c);
        cpu[c]->cpacr_el1 = 3u << 20;
        cpu[c]->el = 0, cpu[c]->spsel = 0;
    }
    machine.stop_pc = STOP;

    /* 1. A write to F's code retires F's blocks only, and asks nothing of the CPUs. */
    check(run(0, C_CALL, 0, F) == 1 && run(1, C_LOOP, 50, G) == 7, "cpu 0 calls F, cpu 1 calls G in a loop");
    HvBlock *f0 = block(F), *g0 = block(G);
    check(f0 && g0, "both translated");
    uint64_t retired0 = 0;
#ifndef OLD
    retired0 = HV_STAT_TOTAL(&machine, retired);
#endif
    calm();
    write32(F, movz_x0(2));
    check(quiet(), "no CPU asked to leave or to reset its branch caches");
    check(f0 && (f0->code[0] & 0xfc000000u) == 0x14000000u, "F's old block: its entry is a branch to its gate");
    check(!block(F), "and nothing finds it");
    check(block(G) == g0 && g0 && g0->code[0] == 0xd503201fu, "G's block is as it was");
#ifndef OLD
    check(f0 && f0->dead && HV_STAT_TOTAL(&machine, retired) == retired0 + 1, "one block retired");
#else
    (void)retired0;
#endif

    /* 2. The ways back into F reach the new code: cpu 1's branch cache (its loop called G; now F through the same
       cache, then a write again), a return through the return table, and a loop chained to itself. */
    check(run(0, C_CALL, 0, F) == 2, "cpu 0 calls the new F");
    check(run(1, C_LOOP, 20, F) == 2, "cpu 1 calls it in a loop (its branch cache fills)");
    calm();
    write32(F, movz_x0(3));
    check(quiet(), "a write again: nobody asked anything");
    check(run(1, C_LOOP, 20, F) == 3, "cpu 1's loop, its caches kept, calls the newest F");
    check(run(0, LOOPF, 10, 0) == 10, "the loop on F's page counts by 1");
    write32(LOOPF, 0x91000800); /* add x0, x0, #2 */
    check(run(0, LOOPF, 10, 0) == 20, "after the write, by 2");

    /* 2b. A callee rewrites the code after its call, then returns. The return goes through the return table to H,
       the exit in the caller's (now retired) block that is chained to the code after the call; that code is gated,
       so the return reaches the new instruction. RC: blr x23; add x0, x0, #1; b STOP. RET0: ret.
       RW: str w4, [x22]; ret, with x22 = RC + 4 and w4 = add x0, x0, #2. */
    {
        uint32_t *rc = at(RCPAGE), *h = at(HPAGE);
        rc[0] = 0xd63f02e0, rc[1] = 0x91000400, rc[2] = branch(RCPAGE + 8, STOP);
        h[0] = 0xd65f03c0;                          /* RET0: ret */
        h[16] = 0xb90002c4, h[17] = 0xd65f03c0;     /* RW: str w4, [x22]; ret */
        check(run(0, RCPAGE, 0, HPAGE) == 1 && run(0, RCPAGE, 0, HPAGE) == 1, "the caller and the code after its call, made");
        HvBlock *after = block(RCPAGE + 4);
        cpu[0]->x[22] = RCPAGE + 4, cpu[0]->x[4] = 0x91000800;
        uint64_t made0 = HV_STAT_TOTAL(&machine, translations);
        check(run(0, RCPAGE, 0, HPAGE + 64) == 2, "a callee rewrites the code after its call: the return runs the new code");
        check(after && after->dead && (after->code[0] & 0xfc000000u) == 0x14000000u, "the old code after the call was gated");
        check(HV_STAT_TOTAL(&machine, translations) > made0, "and made again");
        cpu[0]->x[22] = 0;
    }

    /* 3. Data on a code page, in another piece: the code stays. As C writes it, as a guest store does (the page is
       code, so its stores come to C), and as an atomic does. */
    run(0, C_CALL, 0, F);
    HvBlock *f1 = block(F);
    uint64_t made = HV_STAT_TOTAL(&machine, translations);
    calm();
    write32(DATA, 5);
    check(block(F) == f1 && f1, "a write to data on F's page keeps F's block");
    run(0, C_STORE, 0, 0);
    check(*at(DATA) == 1 && block(F) == f1, "a guest store there keeps it");
    run(0, C_ATOMIC, 0, 0);
    check(*at(DATA) == 2 && block(F) == f1, "a guest atomic there keeps it");
    check(run(0, C_CALL, 0, F) == 3 && HV_STAT_TOTAL(&machine, translations) == made + 2, "F runs on, not translated again");
    write32(F + 0x80, 0xd503201f); /* in F's piece, not one of its words: the page's code goes (a piece at a time) */
    check(!block(F), "a write in F's own piece drops it");

    /* 3b. Guest stores to data on a code page all come to C (the page is code: no TLB entry lets them in straight),
       however many miss the code. HV_CODE_MISS_LIMIT of them drop the page, as a write to its code would; the stores
       after that go the fast way, until code from the page is translated again. slow_why[1]: slow stores to a code
       page (counted under --stats). */
    run(0, C_CALL, 0, F);
    HvBlock *f2 = block(F);
    machine.stats_interval = 1 << 30;
    uint64_t slow0 = HV_STAT_TOTAL(&machine, slow_why[1]), slow_all0 = 0;
    for (int k = 0; k < 6; k++) slow_all0 += HV_STAT_TOTAL(&machine, slow_why[k]);
#ifndef OLD
    uint64_t drops0 = HV_STAT_TOTAL(&machine, smc_miss_drops);
#endif
    run(0, C_SLOOP, HV_CODE_MISS_LIMIT - 1, 0);
    check(f2 && block(F) == f2, "stores to data on F's page, one short of the limit: F's block kept");
    check(HV_STAT_TOTAL(&machine, slow_why[1]) - slow0 == HV_CODE_MISS_LIMIT - 1, "  each one went the slow way");
    run(0, C_SLOOP, 1000, 0);
    uint64_t slow_all = 0;
    for (int k = 0; k < 6; k++) slow_all += HV_STAT_TOTAL(&machine, slow_why[k]);
    check(!block(F), "the next one drops the page");
#ifndef OLD
    check(HV_STAT_TOTAL(&machine, smc_miss_drops) == drops0 + 1, "  counted once");
#endif
    check(HV_STAT_TOTAL(&machine, slow_why[1]) - slow0 == HV_CODE_MISS_LIMIT, "and none after it is slowed by the page being code");
    check(slow_all - slow_all0 <= HV_CODE_MISS_LIMIT + 2, "  1,000 more stores: at most 2 more slow ones (refilling the TLB)");
    machine.stats_interval = 0;
    check(run(0, C_CALL, 0, F) == 3 && block(F), "F is translated again, and runs");

    /* 4. Emptying a zone: each pair is E: add x0, x0, #1; cbnz x1, O; b next E and O: add x0, x0, #2; b next E.
       E all in zone 0 (run with x1 = 0), the O in zone 1 (x1 = 1), each chained into the next E: links from zone 1
       into zone 0, cut when zone 0 is emptied. Their code changes, done together: no cache maintenance call each. */
    for (int p = 0; p < NLINKPAGES; p++)
        for (uint32_t k = 0; k < PAIRS; k++) {
            uint64_t e = LINKS + (uint64_t)p * PAGE + 32ull * k, next = e + 32;
            if (k == PAIRS - 1) next = p == NLINKPAGES - 1 ? STOP : LINKS + (uint64_t)(p + 1) * PAGE;
            at(e)[0] = 0x91000400;
            at(e)[1] = 0xb5000001u | (uint32_t)(4 - 1) << 5; /* cbnz x1, e + 16 */
            at(e)[2] = branch(e + 8, next);
            at(e + 16)[0] = 0x91000800;
            at(e + 16)[1] = branch(e + 20, next);
        }
    hv_machine_set_zones(&machine, 4);
    machine.gen_checks = 0;
    uint64_t pairs = (uint64_t)NLINKPAGES * PAIRS;
    check(run(0, LINKS, 0, 0) == pairs, "the pairs run, through E only");
    check(check_lists(&machine, "pairs run") == 0, "the hash and the page lists hold together (pairs run)");
    int z0 = machine.zone;
    hv_jit_make_room(&machine, -1);
    check(run(0, LINKS, 1, 0) == 3 * pairs, "and through O");
    uint64_t cut0 = HV_STAT_TOTAL(&machine, evict_unchained), calls0 = icache_calls, t0 = mach_absolute_time();
    hv_jit_make_room(&machine, z0);
    uint64_t ticks = mach_absolute_time() - t0;
    uint64_t cut = HV_STAT_TOTAL(&machine, evict_unchained) - cut0, calls = icache_calls - calls0;
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    printf("emptied a zone of %llu blocks with %llu links into it: %.2f ms, %llu cache maintenance calls\n",
           (unsigned long long)pairs, (unsigned long long)cut, (double)ticks * tb.numer / tb.denom / 1e6, (unsigned long long)calls);
    check(cut >= pairs - 1, "every link into it cut");
    check(check_lists(&machine, "after emptying the pairs' zone") == 0, "the hash and the page lists hold together (after emptying the pairs' zone)");
    check(calls < 4, "their code changed together, not one maintenance each");
    check(run(0, LINKS, 1, 0) == 3 * pairs, "and the pairs run as before");

    /* 5. Choosing the zone to empty. R's blocks, used a lot, then retired by a write to R's page; S's, used once and
       live. With every zone filled the next emptied is R's: losing it loses nothing in use. */
    hv_machine_set_zones(&machine, 4);
    for (int k = 0; k < 64; k++) at(RPAGE)[k] = branch(RPAGE + 4ull * k, RPAGE + 4ull * k + 4), at(SPAGE)[k] = branch(SPAGE + 4ull * k, SPAGE + 4ull * k + 4);
    at(RPAGE)[64] = branch(RPAGE + 256, STOP), at(SPAGE)[64] = branch(SPAGE + 256, STOP);
    int zr = machine.zone;
    for (int k = 0; k < 200; k++) run(0, RPAGE, 0, 0); /* each run's first block is looked up: R's zone is used */
    hv_jit_make_room(&machine, -1);
    int zs = machine.zone;
    run(0, SPAGE, 0, 0);
    run(0, SPAGE, 0, 0);
    hv_jit_make_room(&machine, -1); /* the other two, empty, filled in turn */
    run(0, C_CALL, 0, G);
    hv_jit_make_room(&machine, -1);
    run(0, C_CALL, 0, F);
    write32(RPAGE + 0x2000, 0); /* R's page written where no code came from: */
    check(block(RPAGE) != NULL, "  (R's blocks kept by a write elsewhere on the page)");
    write32(RPAGE + 4 * 70, 0xd503201f); /* in their piece: all retired */
    check(block(RPAGE) == NULL, "R's blocks retired");
    hv_jit_make_room(&machine, -1);
    printf("zones: R's %d, S's %d; emptied and filled next: %d\n", zr, zs, machine.zone);
    check(machine.zone == zr, "the zone emptied is R's, whose blocks are all retired");
    check(check_lists(&machine, "after emptying R's zone") == 0, "the hash and the page lists hold together (after emptying R's zone)");
    check(block(SPAGE) != NULL, "S's blocks are still there");
    check(run(0, SPAGE, 0, 0) == 0 && run(0, RPAGE, 0, 0) == 0, "both run");

    /* 6. A CPU calls F in a loop while two threads keep rewriting it: the bytes may change between being read and
       being checked (the block runs just that once), and two writes may retire the page at once (the second waits
       for the first). It must neither hang nor run anything that was never written. */
    write32(F, movz_x0(4));
    uint64_t once0 = 0;
#ifndef OLD
    once0 = HV_STAT_TOTAL(&machine, tr_once);
#endif
    writing = 1;
    pthread_t w[2];
    for (int k = 0; k < 2; k++) pthread_create(&w[k], NULL, rewriter, (void *)(uintptr_t)k);
    int good = 1;
    for (int k = 0; k < 200 && good; k++) {
        uint64_t x0 = run(0, C_LOOP, 100, F);
        good = x0 == 4 || x0 == 5;
    }
    writing = 0;
    for (int k = 0; k < 2; k++) pthread_join(w[k], NULL);
    check(good, "F rewritten all the while by two threads: a loop of calls ends, with values written");
#ifndef OLD
    printf("  (blocks run just once: %llu)\n", (unsigned long long)(HV_STAT_TOTAL(&machine, tr_once) - once0));
#else
    (void)once0;
#endif
    write32(F, movz_x0(9));
    check(run(0, C_CALL, 0, F) == 9, "and the last write is what runs after");

    /* 7. F written (movz #11) just as a CPU places the code it translated from movz #10. */
    HvBlock *f9 = block(F);
#ifndef OLD
    HvJitZone *fz = &machine.zones[f9->zone];
    uint64_t live0 = hv_jit_words_live(&machine), dead0 = fz->dead_words, w9 = f9->words;
#endif
    write32(F, movz_x0(10));
#ifndef OLD
    uint64_t live1 = hv_jit_words_live(&machine), dead1 = fz->dead_words;
    printf("  (F's block %llu words; live code %llu -> %llu words)\n", (unsigned long long)w9, (unsigned long long)live0, (unsigned long long)live1);
    check(w9 > 0 && dead1 - dead0 >= w9 && live0 - live1 == dead1 - dead0, "the code a write retires no longer counts as held");
    uint64_t once1 = HV_STAT_TOTAL(&machine, tr_once);
#endif
    poke_word = movz_x0(11);
    uint64_t x0 = run(0, C_CALL, 0, F);
    check(!poke_word && (x0 == 10 || x0 == 11), "F, written as it is placed, runs what was in memory");
#ifndef OLD
    check(HV_STAT_TOTAL(&machine, tr_once) == once1 + 1, "that block runs just the once");
#endif
    check(block(F) == NULL || *at(F) == movz_x0(11), "and no block of the old bytes is kept");
    check(run(0, C_CALL, 0, F) == 11, "the next call runs the new code");
    hv_dma_written(&machine, DATA, 4); /* a device (no idea which bytes its code came from): the page's code goes */
    check(block(F) == NULL, "a device's write to data on F's page drops F");
    check(run(0, C_CALL, 0, F) == 11, "and F is made again");

    printf(failures ? "FAILED: %d\n" : "all ok\n", failures);
    return failures != 0;
}
