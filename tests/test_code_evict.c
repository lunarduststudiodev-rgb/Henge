// The code cache's zones (core/dispatch.c, make_room): when the cache is full only
// one zone is emptied, so code in use elsewhere stays translated, and nothing is
// left leading into the emptied zone: chained exits from other zones go back to
// their stubs, the hash, the front cache and the branch caches forget its blocks.
// Emptied code is zeroed here (HV_JIT_POISON), so a jump left into it crashes.
#include <stdio.h>
#include <stdlib.h>

#include "dispatch.h"
#include "machine.h"
#include "evict_lists.h"

#define PAGE (1u << HV_RAM_PAGE_BITS) /* a RAM page: its code has its own generation */
#define BASE 0x80000000ull
#define HOT BASE                    /* page 0: a loop */
#define S1 (BASE + PAGE)            /* page 1: cbnz x1 to T1 further on the page */
#define T1 (BASE + PAGE + 0x100)
#define S2 (BASE + 2 * PAGE)        /* page 2: b to T2 on page 3, which loops back x1 times (the exit to another
                                       page chains on its second pass: the first finds the page not yet mapped) */
#define T2 (BASE + 3 * PAGE)
#define STOP (BASE + 4 * PAGE)      /* never translated: the dispatcher stops there */
#define COLD (BASE + 5 * PAGE)      /* pages of one-instruction blocks, one chunk per page */
#define NCHUNKS 40
#define NPAGES (5 + NCHUNKS)
#define INSNS (PAGE / 4)

static HvMachine machine;
static HvCpu cpu;
static uint32_t *ram;
static int failures;

static void check(int ok, const char *what) {
    printf("%s: %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

static uint32_t branch(uint64_t from, uint64_t to) {
    return 0x14000000u | ((uint32_t)((int64_t)(to - from) / 4) & 0x03ffffff);
}

static uint32_t *at(uint64_t va) { return ram + (va - BASE) / 4; }

static uint64_t run(uint64_t pc, uint64_t x1) {
    hv_cpu_reset(&cpu, &machine);
    cpu.el = 0;
    cpu.spsel = 0;
    cpu.pc = pc;
    cpu.x[0] = 0;
    cpu.x[1] = x1;
    cpu.cpacr_el1 = 3u << 20;
    machine.stop_pc = STOP;
    int reason = hv_run(&cpu);
    if (reason != HV_EXIT_HALT || cpu.pc != STOP) {
        printf("run from %llx stopped with %d at %llx\n", (unsigned long long)pc, reason, (unsigned long long)cpu.pc);
        failures++;
    }
    return cpu.x[0];
}

/* On, without a reset: what the CPU remembers (its front cache, its branch caches) stays. */
static uint64_t run_on(uint64_t pc, uint64_t x1) {
    cpu.pc = pc;
    cpu.x[0] = 0;
    cpu.x[1] = x1;
    int reason = hv_run(&cpu);
    if (reason != HV_EXIT_HALT || cpu.pc != STOP) {
        printf("run on from %llx stopped with %d at %llx\n", (unsigned long long)pc, reason, (unsigned long long)cpu.pc);
        failures++;
    }
    return cpu.x[0];
}

static int in_zone(uint64_t code, int z) {
    uint64_t lo = (uint64_t)(uintptr_t)(machine.jit.base + (size_t)z * machine.zone_words);
    return code - lo < (uint64_t)machine.zones[z].used * 4 + (z == machine.zone ? (uint64_t)machine.zone_words * 4 : 0);
}

static HvBlock *block(uint64_t va) {
    HvRam *r = &machine.ram[0];
    uint64_t pg = (va - r->pa) >> HV_RAM_PAGE_BITS;
    return hv_block_find(&machine, hv_block_hash(va, va, 0), va, va, 0, r->page_gen[pg]);
}

/* Code in use all along (the loop) and code run once (the chunks) through a cache that fills many times over. Returns
   how many times the loop was translated again. */
static uint64_t hot_and_cold(int zones) {
    hv_machine_set_zones(&machine, zones);
    machine.gen_checks = 1; /* emptied code is zeroed */
    check(run(HOT, 10) == 10, "the loop runs");
    uint64_t again = 0, emptied = HV_STAT_TOTAL(&machine, zone_evictions), flushes = HV_STAT_TOTAL(&machine, block_flushes), full = HV_STAT_TOTAL(&machine, flush_full);
    for (int c = 0; c < NCHUNKS; c++) {
        run(COLD + (uint64_t)c * PAGE, 0);
        for (int r = 0; r < 50; r++) {
            uint64_t t = HV_STAT_TOTAL(&machine, translations);
            if (run(HOT, 10) != 10) failures++;
            again += HV_STAT_TOTAL(&machine, translations) - t;
        }
    }
    printf("%d zones: the cache made room %llu times (%llu zones emptied, %llu full flushes), the loop was translated again %llu times\n",
           zones, (unsigned long long)(HV_STAT_TOTAL(&machine, flush_full) - full), (unsigned long long)(HV_STAT_TOTAL(&machine, zone_evictions) - emptied),
           (unsigned long long)(HV_STAT_TOTAL(&machine, block_flushes) - flushes), (unsigned long long)again);
    return again;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (hv_machine_init(&machine, 8 << 20) != 0) return 1;
    ram = aligned_alloc(PAGE, (size_t)NPAGES * PAGE);
    if (!ram || hv_machine_add_ram(&machine, BASE, (uint64_t)NPAGES * PAGE, ram, 0) != 0) return 1;
    for (size_t i = 0; i < (size_t)NPAGES * INSNS; i++) ram[i] = 0xd503201f; /* nop */
    uint32_t *h = at(HOT);
    h[0] = 0x91000400; /* add x0, x0, #1 */
    h[1] = 0xf1000421; /* subs x1, x1, #1 */
    h[2] = 0x54000001 | ((uint32_t)(-2 & 0x7ffff) << 5); /* b.ne HOT */
    h[3] = branch(HOT + 12, STOP);
    at(S1)[0] = 0xb5000001u | (uint32_t)(((T1 - S1) / 4) & 0x7ffff) << 5; /* cbnz x1, T1 */
    at(S1)[1] = branch(S1 + 4, STOP);
    at(T1)[0] = 0x91000c00; /* add x0, x0, #3 */
    at(T1)[1] = branch(T1 + 4, STOP);
    at(S2)[0] = branch(S2, T2);
    at(T2)[0] = 0x91001400; /* add x0, x0, #5 */
    at(T2)[1] = 0xf1000421; /* subs x1, x1, #1 */
    at(T2)[2] = 0x54000001 | (uint32_t)((((int64_t)S2 - (int64_t)(T2 + 8)) / 4) & 0x7ffff) << 5; /* b.ne S2 */
    at(T2)[3] = branch(T2 + 12, STOP);
    for (int c = 0; c < NCHUNKS; c++) {
        uint64_t va = COLD + (uint64_t)c * PAGE;
        for (uint32_t i = 0; i + 1 < INSNS; i++) at(va)[i] = branch(va + 4 * i, va + 4 * i + 4);
        at(va)[INSNS - 1] = branch(va + 4 * (INSNS - 1), STOP);
    }
    machine.stats_interval = 0;

    /* 1. Hot code survives the cache filling up. */
    uint64_t again = hot_and_cold(4);
    check(machine.nzones == 4 && HV_STAT_TOTAL(&machine, zone_evictions) >= 4, "the cache filled and emptied zones");
    check(again == 0, "with zones, the loop in use was never translated again");
    uint64_t again_one = hot_and_cold(1);
    check(again_one > 0, "with one zone (the whole cache flushed when full, as before), it was");

    /* 2. Links into an emptied zone are cut, and the guest runs the code made again. */
    hv_machine_set_zones(&machine, 4);
    machine.gen_checks = 1;
    check(run(T1, 0) == 3 && run(T2, 1) == 5, "targets translated in zone 0");
    HvBlock *t1 = block(T1), *t2 = block(T2);
    check(t1 && t2 && t1->zone == 0 && t2->zone == 0, "both are in zone 0");
    hv_jit_make_room(&machine, -1);
    check(machine.zone != 0, "the cache moved on to another zone");
    check(check_lists(&machine, "after a zone switch") == 0, "the hash and the page lists hold together (after a zone switch)");
    check(run(S1, 1) == 3 && run(S2, 2) == 10, "sources run into them");
    int cur = machine.zone;
    uint32_t links[16], nlinks = 0, conds = 0, cross = 0;
    for (uint32_t s = (uint32_t)cur * machine.zone_slots; s < machine.slot_limit && s < (uint32_t)machine.nslots && nlinks < 16; s++)
        if (machine.slots[s] && in_zone(machine.slots[s], 0)) {
            links[nlinks++] = s;
            conds += machine.slot_cond[s] != NULL;
            cross += machine.slot_xpg[s] != 0;
        }
    check(nlinks >= 2 && conds >= 1 && cross >= 1, "exits in the new zone are chained into zone 0 (a conditional one, one from another page)");
    HvRam *r = &machine.ram[0];
    uint64_t t2pg = (T2 - BASE) >> HV_RAM_PAGE_BITS;
    check(r->chain_heads[t2pg] != 0, "T2's page lists the exit from S2");
    uint32_t xchained = machine.nxchained; /* S2's exit to T2's page, and T2's back to S2's */
    uint32_t *site[16], *stub[16], *cond[16];
    for (uint32_t k = 0; k < nlinks; k++) site[k] = machine.slot_site[links[k]], stub[k] = machine.slot_stub[links[k]], cond[k] = machine.slot_cond[links[k]];

    hv_jit_make_room(&machine, 0);
    check(machine.zones[0].nblocks == 0 && machine.zones[0].used == 0, "zone 0 is empty");
    check(!block(T1) && !block(T2), "its blocks are out of the hash");
    int cut = 1;
    for (uint32_t k = 0; k < nlinks; k++) {
        uint32_t s = links[k];
        cut &= machine.slots[s] == 0 && machine.slot_xpg[s] == 0;
        cut &= *site[k] == branch((uint64_t)(uintptr_t)site[k], (uint64_t)(uintptr_t)stub[k]); /* back to its stub */
        if (cond[k]) { /* the conditional branch back to that B */
            uint32_t w = *cond[k];
            int64_t off = (int64_t)(int32_t)(w << 8) >> 13;
            cut &= cond[k] + off == site[k];
        }
    }
    check(cut, "every exit chained into it went back to its stub, its conditional branch too");
    uint32_t listed = 0, zone0_listed = 0;
    for (uint32_t pg = 0; pg < NPAGES; pg++)
        for (uint32_t l = r->chain_heads[pg]; l; l = machine.slot_xnext[l - 1]) listed++, zone0_listed += l - 1 < machine.zone_slots;
    check(r->chain_heads[t2pg] == 0 && listed == machine.nxchained && machine.nxchained == xchained - 2,
          "and off the page's list");
    check(zone0_listed == 0, "so are zone 0's own exits to other pages (T2's back to S2): their slots are free again");
    check(check_lists(&machine, "after emptying zone 0") == 0, "the hash and the page lists hold together (after emptying zone 0)");
    check(run(T1, 0) == 3 && run(T2, 1) == 5, "the targets run again (made again, not the emptied code)");
    check(run(S1, 1) == 3 && run(S2, 2) == 10, "the sources run into the new code");
    HvBlock *n1 = block(T1), *n2 = block(T2);
    check(n1 && n2 && n1->zone == machine.zone && machine.zone == 0, "the new targets are in the zone being filled: the one just emptied");
    int rechained = n1 && n2;
    for (uint32_t k = 0; k < nlinks && rechained; k++) {
        uint64_t to = machine.slots[links[k]];
        rechained &= to == (uint64_t)(uintptr_t)n1->code || to == (uint64_t)(uintptr_t)n2->code;
    }
    check(rechained, "and the sources chain to them");

    /* 3. A CPU that ran the emptied code and goes on without a reset doesn't find it in what it remembers. */
    hv_jit_make_room(&machine, -1);
    hv_jit_make_room(&machine, -1);
    hv_jit_make_room(&machine, -1); /* every zone has been filled: zone 0, with the new targets, is the oldest */
    check(run(T1, 0) == 3 && run_on(T2, 1) == 5 && block(T1)->zone == 0, "the CPU ran the targets in zone 0");
    hv_jit_make_room(&machine, 0);
    check(!block(T1), "zone 0 emptied under it");
    check(check_lists(&machine, "after emptying zone 0 again") == 0, "the hash and the page lists hold together (after emptying zone 0 again)");
    check(run_on(T1, 0) == 3 && run_on(T2, 1) == 5 && run_on(S2, 2) == 10, "it runs them again, made again");

    /* 4. A full flush still drops it all. */
    hv_flush_blocks(&machine);
    check(machine.zone == 0 && !block(T1) && !machine.nxchained && hv_blocks_held(&machine) == 0, "a full flush empties every zone");
    check(run(S2, 2) == 10, "and the guest runs on");
    check(check_lists(&machine, "after a full flush and a run") == 0, "the hash and the page lists hold together (after a full flush and a run)");

    /* 5. Every zone but the one being filled retired, and none due back: make_room has no zone to empty. It starts
       over with a full flush instead of emptying zone -1 (pick_victim's "none"). */
    hv_machine_set_zones(&machine, 4);
    check(machine.nzones == 4 && run(S2, 2) == 10, "four zones, the guest running in zone 0");
    machine.zones_retired = 0xe;
    machine.trim_quiet_ns = ~0ull;
    uint32_t gen = machine.jit_gen;
    uint64_t flushes = HV_STAT_TOTAL(&machine, block_flushes);
    hv_jit_make_room(&machine, -1);
    check(machine.zone == 0 && machine.jit_gen != gen && HV_STAT_TOTAL(&machine, block_flushes) > flushes && !block(S2),
          "no zone to empty: the cache is flushed instead");
    check(check_lists(&machine, "after the flush instead") == 0, "the hash and the page lists hold together (after the flush instead)");
    check(run(S2, 2) == 10, "and the guest runs on");
    hv_machine_free(&machine);
    free(ram);
    if (failures) printf("test_code_evict: %d failures\n", failures);
    return failures != 0;
}
