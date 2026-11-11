// Emptying a code-cache zone costs that zone, not the pages its blocks share with the other zones (core/dispatch.c
// evict_unhook; the evict cost notes section 7.2):
//   A. a zone's blocks and exits come off their pages' lists in time that does not grow with what other zones hold there;
//   C. a retired block in the middle of a page's list, then the zones on either side of it emptied;
//   D. lookups walking a bucket lock-free while blocks in it are retired and made again find the one after them;
//   E. exits chained into the last page of the eighth RAM region (the page key's region and page bits);
//   G. the block record's size and layout.
// Most of it uses records made by hand, as publishing (dispatch.c lookup) and chaining (chain()) make them, so a zone
// can hold 64K blocks on four pages without translating them; E runs real guest code.
#include <mach/mach_time.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dispatch.h"
#include "machine.h"
#include "evict_lists.h"

#define PAGE (1u << HV_RAM_PAGE_BITS)
#define BASE 0x80000000ull
#define NPAGES 64
#define FILL 7 /* the zone being filled while the others are made by hand */

static HvMachine m;
static uint32_t *ram;
static int failures;

static void check(int ok, const char *what) {
    printf("%s: %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

static double us(uint64_t ticks) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)ticks * tb.numer / tb.denom / 1e3;
}

/* Record i of zone z, made from page pg of RAM 0 at va, as publishing leaves it: on its page's list and in the hash. */
static HvBlock *fab_block(int z, uint32_t i, uint32_t pg, uint64_t va, uint32_t gen) {
    HvBlock *b = &m.blocks[(size_t)z * m.zone_blocks + i];
    HvRam *r = &m.ram[0];
    uint64_t pa = BASE + (uint64_t)pg * PAGE + (va & (PAGE - 4));
    *b = (HvBlock){.jit_gen = m.zones[z].born, .va = va, .pa = pa, .ninsns = 1, .words = 1, .gen = gen, .ram = r,
                   .pg = pg, .code = m.jit.base + (size_t)z * m.zone_words + i, .zone = (uint8_t)z};
    hv_page_list_insert(&m, r, pg, b);
    uint32_t h = hv_block_hash(va, pa, 0);
    hv_hash_insert(&m, b, h);
    m.hash_used |= HV_PART(h, 21);
    return b;
}

/* Exit j of zone z chained into block to, as chain() leaves an exit to another page: on to's page's list. */
static void fab_exit(int z, uint32_t j, const HvBlock *to) {
    uint32_t s = (uint32_t)z * m.zone_slots + j;
    m.slots[s] = (uint64_t)(uintptr_t)to->code;
    m.slot_site[s] = m.slot_stub[s] = m.slot_cond[s] = NULL; /* nothing to patch */
    m.slot_xa[s] = 0;
    hv_xlist_insert(&m, to->ram, to->pg, s);
}

/* Zone z as zone_save leaves one filled. */
static void filled(int z, uint32_t nblocks, uint32_t nslots) {
    m.zones[z].nblocks = nblocks, m.zones[z].nslots = nslots, m.zones[z].used = nblocks, m.zones[z].filled = (uint64_t)z + 1;
}

/* Everything emptied, zone FILL the one being filled: zones 0 to FILL - 1 are made by hand. */
static void fresh(void) {
    hv_flush_blocks(&m);
    hv_jit_zone_fill(&m, FILL);
}

static uint64_t va_of(int z, uint32_t i) { return ((uint64_t)(z + 1) << 32) + (uint64_t)i * 4; }

static uint32_t page_gen(uint32_t pg) { return m.ram[0].page_gen[pg]; }

#define NSTEP 3
/* A. Zone 0: 64 blocks on pages 0 to 3, each with an exit from another of its pages (its own exits, on the target
   pages' lists). Zones 1 to 6: n blocks each on the same four pages, each with an exit into its own zone, also on
   those pages' lists; zone 1 has 16 exits into zone 0 as well. Zone 0's blocks and exits went on first, so they are
   at the far end of every list: finding them from a list's head would walk all of it. Zone 0 is emptied; the best of
   3 runs' step times. */
static void zone_cost(uint32_t n, uint64_t best[NSTEP], uint64_t *hops) {
    for (int k = 0; k < NSTEP; k++) best[k] = ~0ull;
    for (int rep = 0; rep < 3; rep++) {
        fresh();
        HvBlock *z0[64];
        for (uint32_t i = 0; i < 64; i++) z0[i] = fab_block(0, i, i % 4, va_of(0, i), page_gen(i % 4));
        for (uint32_t j = 0; j < 64; j++) fab_exit(0, j, z0[(j + 1) % 64]);
        filled(0, 64, 64);
        for (int z = 1; z < FILL; z++) {
            for (uint32_t i = 0; i < n; i++) fab_block(z, i, i % 4, va_of(z, i), page_gen(i % 4));
            for (uint32_t j = 0; j < n; j++) fab_exit(z, j, &m.blocks[(size_t)z * m.zone_blocks + (j + 1) % n]);
            filled(z, n, n);
        }
        for (uint32_t j = 0; j < 16; j++) fab_exit(1, n + j, z0[j]);
        filled(1, n, n + 16);
        if (rep == 0) check(check_lists(&m, "made by hand") == 0, "the hand-made zones hold together");
        uint64_t unl = HV_STAT_TOTAL(&m, evict_page_unlinks), del = HV_STAT_TOTAL(&m, evict_delists),
                 cut = HV_STAT_TOTAL(&m, evict_unchained), live = HV_STAT_TOTAL(&m, evict_live),
                 hop = HV_STAT_TOTAL(&m, evict_hash_hops), t[NSTEP];
        for (int k = 0; k < NSTEP; k++) t[k] = HV_STAT_TOTAL(&m, evict_step_ticks[k]);
        uint32_t chained = m.nxchained;
        hv_jit_make_room(&m, 0);
        for (int k = 0; k < NSTEP; k++) {
            uint64_t d = HV_STAT_TOTAL(&m, evict_step_ticks[k]) - t[k];
            if (d < best[k]) best[k] = d;
        }
        *hops = HV_STAT_TOTAL(&m, evict_hash_hops) - hop;
        if (rep) continue;
        unl = HV_STAT_TOTAL(&m, evict_page_unlinks) - unl, del = HV_STAT_TOTAL(&m, evict_delists) - del;
        cut = HV_STAT_TOTAL(&m, evict_unchained) - cut, live = HV_STAT_TOTAL(&m, evict_live) - live;
        printf("n %u: %llu blocks out of the hash, %llu off page lists, %llu exits cut, %llu off lists\n", n,
               (unsigned long long)live, (unsigned long long)unl, (unsigned long long)cut, (unsigned long long)del);
        check(live == 64 && unl == 64, "  each of zone 0's blocks came out of the hash and off its page's list once");
        check(cut == 16 && del == 80, "  the 16 exits into it cut, and they and its own 64 off their lists");
        check(m.nxchained == chained - 80, "  the count of listed exits went down by as many");
        uint32_t left = 0;
        for (uint32_t pg = 0; pg < 4; pg++)
            for (uint32_t i = m.ram[0].block_heads[pg]; i; i = m.blocks[i - 1].page_next) left++;
        check(left == (FILL - 1) * n, "  the other zones' blocks are all still on the four pages' lists");
        check(check_lists(&m, "zone 0 emptied") == 0, "  the hash and the lists hold together");
    }
}

/* C. Page Q holds a block of zones 1, 2 and 3, zone 2's in the middle of its list; a write to Q retires that one
   only (the other two were made after the generation moves), then zones 2 and 1 are emptied. */
static void middle(void) {
    const uint32_t q = 10;
    fresh();
    uint32_t g = page_gen(q);
    HvBlock *n1 = fab_block(1, 0, q, va_of(1, 0), g + 1), *mid = fab_block(2, 0, q, va_of(2, 0), g),
            *n3 = fab_block(3, 0, q, va_of(3, 0), g + 1);
    for (int z = 1; z <= 3; z++) filled(z, 1, 0);
    fab_exit(4, 0, n1); /* two exits of zone 4 into the page: the first one in has a back link */
    fab_exit(4, 1, n3);
    filled(4, 0, 2);
    check(mid->page_prev == hv_block_index(&m, n3) && mid->page_next == hv_block_index(&m, n1),
          "C: zone 2's block is in the middle of its page's list");
    m.ram[0].code_pages[q] = 1;
    hv_invalidate_code(&m, BASE + (uint64_t)q * PAGE);
    check(mid->dead && !n1->dead && !n3->dead && !mid->page_prev && !mid->page_next, "a write retires it alone, off the list");
    uint32_t s4 = 4 * m.zone_slots;
    check(!m.ram[0].chain_heads[q] && !m.nxchained && !m.slots[s4] && !m.slots[s4 + 1] && !m.slot_xpg[s4] && !m.slot_xprev[s4] &&
              !m.slot_xnext[s4] && !m.slot_xpg[s4 + 1] && !m.slot_xprev[s4 + 1] && !m.slot_xnext[s4 + 1],
          "and the exits into the page are cut and off its list, their links cleared");
    check(m.ram[0].block_heads[q] == hv_block_index(&m, n3) && n3->page_next == hv_block_index(&m, n1) &&
              n1->page_prev == hv_block_index(&m, n3),
          "its neighbours linked to each other");
    check(check_lists(&m, "middle retired") == 0, "the lists hold together");
    hv_jit_make_room(&m, 2);
    check(m.ram[0].block_heads[q] == hv_block_index(&m, n3) && n3->page_next == hv_block_index(&m, n1),
          "emptying its zone leaves the page's list as it was (the retired block is passed over)");
    check(check_lists(&m, "zone 2 emptied") == 0, "the lists hold together");
    hv_jit_make_room(&m, 1);
    check(m.ram[0].block_heads[q] == hv_block_index(&m, n3) && !n3->page_next && !n3->page_prev,
          "emptying zone 1 leaves zone 3's block alone on it");
    check(check_lists(&m, "zone 1 emptied") == 0, "the lists hold together");
}

/* D. Bucket: X (page A) -> M (page B) -> T (page C). Threads look T up without the lock over and over while M and X
   are retired (M first: it is in the middle) and made again, M before X so the bucket is X' -> M' -> T again. A
   lookup standing on a retired block must go on to T. */
#define PG_A 20
#define PG_B 21
#define PG_C 22
#define ROUNDS 20000
static uint64_t t_va, t_pa;
static uint32_t t_h, t_gen;
static int walking;
static uint64_t found, missed;

static void *walker(void *arg) {
    (void)arg;
    uint64_t f = 0, x = 0;
    while (__atomic_load_n(&walking, __ATOMIC_ACQUIRE)) {
        if (hv_block_find(&m, t_h, t_va, t_pa, 0, t_gen)) f++;
        else x++;
    }
    __atomic_add_fetch(&found, f, __ATOMIC_RELAXED);
    __atomic_add_fetch(&missed, x, __ATOMIC_RELAXED);
    return NULL;
}

static uint64_t va_into(uint32_t pg, uint64_t from) { /* a va on page pg whose key falls in T's bucket */
    for (uint64_t va = from;; va += 4)
        if (hv_block_hash(va, BASE + (uint64_t)pg * PAGE + (va & (PAGE - 4)), 0) == t_h) return va;
}

static HvBlock *fab_next(uint32_t pg, uint64_t va) { /* in the zone being filled, as publishing does */
    HvBlock *b = fab_block(FILL, (uint32_t)m.nblocks - FILL * m.zone_blocks, pg, va, page_gen(pg));
    m.nblocks++;
    return b;
}

static void lookups(void) {
    fresh();
    t_va = 0x5000;
    t_pa = BASE + (uint64_t)PG_C * PAGE + (t_va & (PAGE - 4));
    t_h = hv_block_hash(t_va, t_pa, 0);
    t_gen = page_gen(PG_C);
    uint64_t x_va = va_into(PG_A, 1ull << 40), m_va = va_into(PG_B, 2ull << 40);
    pthread_mutex_lock(&m.tr_mu);
    HvBlock *t = fab_next(PG_C, t_va), *mb = fab_next(PG_B, m_va), *xb = fab_next(PG_A, x_va);
    pthread_mutex_unlock(&m.tr_mu);
    check(m.hash[t_h] == hv_block_index(&m, xb) && xb->hash_next == hv_block_index(&m, mb) && mb->hash_next == hv_block_index(&m, t),
          "D: the bucket is X -> M -> T");
    __atomic_store_n(&walking, 1, __ATOMIC_RELEASE);
    pthread_t w[2];
    for (int k = 0; k < 2; k++) pthread_create(&w[k], NULL, walker, NULL);
    for (int round = 0; round < ROUNDS; round++) {
        m.ram[0].code_pages[PG_A] = m.ram[0].code_pages[PG_B] = 1;
        hv_invalidate_code(&m, BASE + (uint64_t)PG_B * PAGE); /* M, in the middle */
        hv_invalidate_code(&m, BASE + (uint64_t)PG_A * PAGE); /* X, first */
        pthread_mutex_lock(&m.tr_mu);
        fab_next(PG_B, m_va);
        fab_next(PG_A, x_va);
        pthread_mutex_unlock(&m.tr_mu);
    }
    __atomic_store_n(&walking, 0, __ATOMIC_RELEASE);
    for (int k = 0; k < 2; k++) pthread_join(w[k], NULL);
    printf("%d rounds of retiring and making again: %llu lookups found T, %llu missed it\n", ROUNDS,
           (unsigned long long)found, (unsigned long long)missed);
    check(found > 0 && missed == 0, "every lookup found T, past blocks retired under it");
    check(m.zones[FILL].ndead == 2 * ROUNDS, "every M and X retired");
    check(check_lists(&m, "after the rounds") == 0, "the lists hold together");
}

/* E. Eight RAM regions of 4 pages, one after another. S, on region 0's first page, branches to T on region 7's last
   page, which loops back to S x1 times: S's exit chains onto that page's list with the key of region 7, page 3, and
   T's onto S's page's. The zone they are in is emptied. */
#define E_PAGES 4
#define E_S BASE
#define E_STOP (BASE + PAGE)
#define E_T (BASE + 7ull * E_PAGES * PAGE + (E_PAGES - 1) * PAGE)
static HvMachine e;
static HvCpu cpu;
static uint32_t *e_ram[HV_MAX_RAM];

static uint32_t branch(uint64_t from, uint64_t to) { return 0x14000000u | ((uint32_t)((int64_t)(to - from) / 4) & 0x03ffffff); }

static uint32_t *e_at(uint64_t va) {
    uint64_t off = va - BASE;
    return e_ram[off / (E_PAGES * PAGE)] + off % (E_PAGES * PAGE) / 4;
}

static uint64_t e_run(uint64_t pc, uint64_t x1) {
    hv_cpu_reset(&cpu, &e);
    cpu.el = 0;
    cpu.spsel = 0;
    cpu.pc = pc;
    cpu.x[0] = 0;
    cpu.x[1] = x1;
    cpu.cpacr_el1 = 3u << 20;
    e.stop_pc = E_STOP;
    int reason = hv_run(&cpu);
    if (reason != HV_EXIT_HALT || cpu.pc != E_STOP) {
        printf("run from %llx stopped with %d at %llx\n", (unsigned long long)pc, reason, (unsigned long long)cpu.pc);
        failures++;
    }
    return cpu.x[0];
}

static int e_listed(HvRam *r, uint32_t pg) {
    int n = 0;
    for (uint32_t l = r->chain_heads[pg]; l; l = e.slot_xnext[l - 1]) n += e.slot_xpg[l - 1] == hv_page_key(&e, r, pg);
    return n;
}

static void regions(void) {
    /* The key, both ways, for each region at its first, second and last page */
    int ok = 1;
    for (int ri = 0; ri < HV_MAX_RAM; ri++) {
        const uint32_t pgs[3] = {0, 1, (1u << HV_PAGE_KEY_BITS) - 1};
        for (int k = 0; k < 3; k++) {
            uint32_t key = hv_page_key(&e, &e.ram[ri], pgs[k]);
            ok &= key != 0 && hv_page_key_ram(&e, key) == &e.ram[ri] && hv_page_key_pg(key) == pgs[k];
        }
    }
    check(ok, "E: a page key gives back its region and page, for every region up to its last possible page");

    if (hv_machine_init(&e, 8 << 20) != 0) {
        check(0, "a machine");
        return;
    }
    for (int ri = 0; ri < HV_MAX_RAM; ri++) {
        e_ram[ri] = aligned_alloc(PAGE, (size_t)E_PAGES * PAGE);
        if (!e_ram[ri] || hv_machine_add_ram(&e, BASE + (uint64_t)ri * E_PAGES * PAGE, (uint64_t)E_PAGES * PAGE, e_ram[ri], 0) != 0) {
            check(0, "eight RAM regions");
            return;
        }
        for (size_t i = 0; i < (size_t)E_PAGES * PAGE / 4; i++) e_ram[ri][i] = 0xd503201f; /* nop */
    }
    e.stats_interval = 0;
    e_at(E_S)[0] = branch(E_S, E_T);
    e_at(E_T)[0] = 0x91001400; /* add x0, x0, #5 */
    e_at(E_T)[1] = 0xf1000421; /* subs x1, x1, #1 */
    e_at(E_T)[2] = 0x54000001 | (uint32_t)((((int64_t)E_S - (int64_t)(E_T + 8)) / 4) & 0x7ffff) << 5; /* b.ne S */
    e_at(E_T)[3] = branch(E_T + 12, E_STOP);
    hv_machine_set_zones(&e, 4);
    HvRam *r0 = hv_ram_lookup(&e, E_S), *r7 = hv_ram_lookup(&e, E_T);
    check(r0 == &e.ram[0] && r7 == &e.ram[7], "S is in the first region, T in the eighth");
    check(e_run(E_S, 3) == 15, "S and T run");
    check(e_listed(r7, E_PAGES - 1) == 1 && e_listed(r0, 0) == 1 && e.nxchained == 2,
          "S's exit is on T's page's list with region 7's last page as its key, T's on S's page's");
    check(check_lists(&e, "chained across regions") == 0, "the lists hold together");
    hv_jit_make_room(&e, -1);
    hv_jit_make_room(&e, 0);
    check(!r7->chain_heads[E_PAGES - 1] && !r0->chain_heads[0] && !e.nxchained && !r7->block_heads[E_PAGES - 1],
          "emptying their zone takes both exits off their lists, and T off its page's");
    int whole = check_lists(&e, "their zone emptied") == 0;
    check(whole, "the lists hold together");
    /* Not on broken lists: a stale link can lead the next chaining round in a circle (the test would hang, not fail). */
    if (whole) check(e_run(E_S, 3) == 15 && e_run(E_S, 2) == 10 && e_listed(r7, E_PAGES - 1) == 1, "they run and chain again");
    hv_machine_free(&e);
    for (int ri = 0; ri < HV_MAX_RAM; ri++) free(e_ram[ri]);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    /* G. The record's layout: 80 bytes, the two 32-bit links where the hash pointer was. */
    printf("G: a block record is %zu bytes, hash_next at %zu, page_prev at %zu; buckets of %zu bytes, page keys of %zu\n",
           sizeof(HvBlock), offsetof(HvBlock, hash_next), offsetof(HvBlock, page_prev), sizeof m.hash[0], sizeof *m.slot_xpg);
    check(sizeof(HvBlock) == 80 && offsetof(HvBlock, hash_next) == 64 && offsetof(HvBlock, page_prev) == 68 &&
              sizeof m.hash[0] == 4 && sizeof *m.slot_xpg == 4,
          "G: the block record and the tables keep their sizes");

    if (hv_machine_init(&m, 128u << 20) != 0) return 1;
    ram = aligned_alloc(PAGE, (size_t)NPAGES * PAGE);
    if (!ram || hv_machine_add_ram(&m, BASE, (uint64_t)NPAGES * PAGE, ram, 0) != 0) return 1;
    memset(ram, 0, (size_t)NPAGES * PAGE);
    m.stats_interval = 0;
    hv_machine_set_zones(&m, 8);
    if (m.nzones != 8 || m.zone_blocks < 65536 || m.zone_slots < 65536 + 16) {
        printf("zones %d of %u blocks: too small for the test\n", m.nzones, m.zone_blocks);
        return 1;
    }

    /* A */
    uint64_t small[NSTEP], big[NSTEP], hs, hb;
    zone_cost(1024, small, &hs);
    zone_cost(65536, big, &hb);
    uint32_t per_page = 16 + (FILL - 1) * 65536 / 4;
    printf("A: emptying a zone of 64 blocks (best of 3), with 6 x 1K / 6 x 64K other blocks on its 4 pages:\n"
           "   its blocks %.1f / %.1f us, the other zones' exits (a scan of them all) %.1f / %.1f us, its own exits %.1f / %.1f us;"
           " hash links walked past %llu / %llu.\n"
           "   Walking its pages' lists, as before, would visit %u blocks and %u exits at 64K.\n",
           us(small[0]), us(big[0]), us(small[1]), us(big[1]), us(small[2]), us(big[2]), (unsigned long long)hs,
           (unsigned long long)hb, 4 * per_page, 4 * per_page);
    check(us(big[0]) < 4 * us(small[0]) + 100, "A: its blocks come off in time that does not grow with the other zones'");
    check(us(big[2]) < 4 * us(small[2]) + 100, "A: and its own exits");

    middle();
    lookups();
    regions();

    hv_machine_free(&m);
    free(ram);
    if (failures) printf("test_evict_lists: %d failures\n", failures);
    return failures != 0;
}
