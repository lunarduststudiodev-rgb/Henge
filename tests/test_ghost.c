// How well the table of blocks thrown out (core/machine.h HV_GHOST_*) tells what a translation makes again
// (HvStats.retrans_evicted): a zone of about 650K blocks, the size an 8 GB Mac's zones run to, is emptied and its code
// run again, and nearly all of it must count as made again because its zone was emptied. Then the table alone: how
// much of each of the last few zones' keys it still holds, and how often a key never thrown out passes for one.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dispatch.h"
#include "machine.h"
#include "zalloc.h"

#define PAGE (1u << HV_RAM_PAGE_BITS)
#define BASE 0x80000000ull
#define ZONES 4
#define ZONE_KEYS 650000u /* blocks in a zone at 8 GB */

static HvMachine m;
static HvCpu *cpu;
static uint32_t *ram;
static int failures;

static void check(int ok, const char *what) {
    printf("%s: %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

static uint32_t branch(uint64_t from, uint64_t to) { return 0x14000000u | ((uint32_t)((int64_t)(to - from) / 4) & 0x03ffffff); }

static int run(uint64_t pc, uint64_t stop) {
    hv_cpu_reset_index(cpu, &m, 0);
    cpu->cpacr_el1 = 3u << 20;
    cpu->el = 0, cpu->spsel = 0;
    cpu->pc = pc;
    cpu->x[0] = 0;
    int r = hv_run(cpu);
    return r == HV_EXIT_HALT && cpu->pc == stop;
}

static HvBlock *block(uint64_t va) {
    HvRam *r = &m.ram[0];
    return hv_block_find(&m, hv_block_hash(va, va, 0), va, va, 0, r->page_gen[(va - r->pa) >> HV_RAM_PAGE_BITS]);
}

#define T(f) HV_STAT_TOTAL(&m, f)

/* The table alone: keys of zones of ZONE_KEYS blocks noted as their zones are emptied, oldest zone first. */
static void table_only(void) {
    HvBlock b;
    memset(&b, 0, sizeof b);
    memset(m.ghost, 0, HV_GHOST_ENTRIES * sizeof *m.ghost);
    /* Keys never thrown out, against a full table: each match is a translation of new code counted as made again. */
    for (uint32_t i = 0; i < 4 * ZONE_KEYS; i++) b.va = b.pa = 0x100000000ull + 4ull * i, hv_ghost_note(&m, &b, HV_GHOST_EVICTED);
    uint32_t false_hits = 0;
    for (uint32_t i = 0; i < ZONE_KEYS; i++) {
        uint64_t va = 0x900000000ull + 4ull * i;
        false_hits += hv_ghost_take(&m, va, va, 0, 0) != 0;
    }
    printf("of %u keys never thrown out, %u (%.3f%%) matched an entry of a full table\n", ZONE_KEYS, false_hits,
           100.0 * false_hits / ZONE_KEYS);
    check(false_hits < ZONE_KEYS / 200, "fewer than 1 in 200 keys never thrown out pass for one");

    memset(m.ghost, 0, HV_GHOST_ENTRIES * sizeof *m.ghost);
    for (uint32_t z = 0; z < 4; z++)
        for (uint32_t i = 0; i < ZONE_KEYS; i++) {
            b.va = b.pa = 0x100000000ull * (z + 1) + 4ull * i;
            hv_ghost_note(&m, &b, HV_GHOST_EVICTED);
        }
    double kept[4];
    for (int z = 3; z >= 0; z--) {
        uint32_t found = 0;
        for (uint32_t i = 0; i < ZONE_KEYS; i++) {
            uint64_t va = 0x100000000ull * (uint32_t)(z + 1) + 4ull * i;
            found += hv_ghost_take(&m, va, va, 0, 0) == HV_GHOST_EVICTED;
        }
        kept[z] = 100.0 * found / ZONE_KEYS;
    }
    printf("four zones of %u blocks emptied in turn: the table still holds %.1f%% of the last's keys, %.1f%% of the one "
           "before, %.1f%%, %.1f%%\n", ZONE_KEYS, kept[3], kept[2], kept[1], kept[0]);
    check(kept[3] >= 99.0, "it holds 99% of the last zone's keys");
    check(kept[2] >= 80.0, "and 80% of the one emptied before");
    memset(m.ghost, 0, HV_GHOST_ENTRIES * sizeof *m.ghost);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    /* 4 zones of about 650K block records (one per 160 bytes of cache) */
    if (hv_machine_init(&m, (size_t)ZONES * ZONE_KEYS * 160) != 0) return 1;
    m.chain = 1;
    m.stats_interval = 0; /* always counted */
    hv_machine_set_zones(&m, ZONES);
    printf("%d zones of %u blocks\n", ZONES, m.zone_blocks);

    /* A chain of add x0, x0, #1; b next, 2048 blocks a page, longer than a zone holds, then b STOP. */
    uint64_t nchain = m.zone_blocks + 4096ull;
    uint64_t npages = (nchain * 8 + PAGE - 1) / PAGE + 1, stop = BASE + (npages - 1) * PAGE;
    ram = aligned_alloc(PAGE, npages * PAGE);
    if (!ram || hv_machine_add_ram(&m, BASE, npages * PAGE, ram, 0) != 0) return 1;
    for (size_t i = 0; i < npages * PAGE / 4; i++) ram[i] = 0xd503201f; /* nop */
    for (uint64_t i = 0; i < nchain; i++) {
        uint64_t va = BASE + i * 8;
        ram[2 * i] = 0x91000400;
        ram[2 * i + 1] = branch(va + 4, i + 1 < nchain ? va + 8 : stop);
    }
    m.stop_pc = stop;
    cpu = zalloc(_Alignof(HvCpu), sizeof(HvCpu));

    check(run(BASE, stop) && cpu->x[0] == nchain, "the chain runs");
    int z0 = block(BASE)->zone;
    check(z0 != m.zone, "its first zone is full and the next one being filled");
    uint32_t n = m.zones[z0].nblocks;
    printf("zone %d holds %u of its blocks\n", z0, n);
    uint64_t tr = T(translations), ev = T(retrans_evicted), wr = T(retrans_written);
    hv_jit_make_room(&m, z0);
    check(!block(BASE), "emptying that zone throws its blocks out");
    check(run(BASE, stop) && cpu->x[0] == nchain, "the chain runs again");
    uint64_t again = T(translations) - tr, evicted = T(retrans_evicted) - ev;
    printf("%llu blocks made again, %llu (%.2f%%) counted as made again because their zone was emptied, %llu as written\n",
           (unsigned long long)again, (unsigned long long)evicted, 100.0 * (double)evicted / (double)(again ? again : 1),
           (unsigned long long)(T(retrans_written) - wr));
    check(again == n, "just the emptied zone's blocks are made again");
    check(evicted * 100 >= again * 95, "at least 95% of them count as retrans_evicted");
    check(T(retrans_written) == wr, "none as retrans_written");

    table_only();

    free(cpu);
    hv_machine_free(&m);
    free(ram);
    if (failures) printf("test_ghost: %d failures\n", failures);
    return failures != 0;
}
