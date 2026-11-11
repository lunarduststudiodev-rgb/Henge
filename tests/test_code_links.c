// Chains between pages (HvRam.chain_heads): changing a page unlinks only the
// exits chained into it, and the guest then runs the new code.
#include <stdio.h>
#include <stdlib.h>

#include "dispatch.h"
#include "machine.h"
#include "evict_lists.h"

#define PAGE (1u << HV_RAM_PAGE_BITS) /* a RAM page: its code has its own generation */
#define BASE 0x80000000ull

static HvMachine machine;
static HvCpu cpu;
static int failures;

static void check(int ok, const char *what) {
    printf("%s: %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

static uint32_t branch(uint64_t from, uint64_t to) {
    return 0x14000000u | ((uint32_t)((int64_t)(to - from) / 4) & 0x03ffffff);
}

static void run(unsigned result) {
    hv_cpu_reset(&cpu, &machine);
    cpu.el = 0;
    cpu.spsel = 0;
    cpu.pc = BASE;
    cpu.x[1] = 10;
    cpu.cpacr_el1 = 3u << 20;
    machine.stop_pc = BASE + 2 * PAGE;
    int reason = hv_run(&cpu);
    check(reason == HV_EXIT_HALT && cpu.x[0] == result, "cross-page loop executes current code");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (hv_machine_init(&machine, 4 << 20) != 0) return 1;
    uint32_t *ram = aligned_alloc(PAGE, 3 * PAGE);
    if (!ram || hv_machine_add_ram(&machine, BASE, 3 * PAGE, ram, 0) != 0) return 1;
    machine.stats_interval = 1;
    ram[0] = branch(BASE, BASE + PAGE);
    uint32_t *target = ram + PAGE / 4;
    target[0] = 0x91000400; /* add x0, x0, #1 */
    target[1] = 0xf1000421; /* subs x1, x1, #1 */
    target[2] = 0x54000001 | (((uint32_t)(-(int32_t)(PAGE + 8) / 4) & 0x7ffff) << 5); /* b.ne BASE */
    target[3] = branch(BASE + PAGE + 12, BASE + 2 * PAGE);
    run(10);
    HvRam *region = &machine.ram[0];
    check(region->chain_heads[0] && region->chain_heads[1], "real JIT branches indexed by target page");

    /* A large unrelated set must neither be examined nor unlinked when page 1 changes. */
    uint32_t unrelated_first = machine.nslots;
    for (unsigned i = 0; i < 1000; i++) {
        uint32_t slot = machine.nslots++;
        if (slot >= machine.max_slots) return 1;
        hv_xlist_insert(&machine, region, 2, slot);
        machine.slots[slot] = 0x1234;
    }
    uint32_t affected = 0;
    for (uint32_t link = region->chain_heads[1]; link; link = machine.slot_xnext[link - 1]) affected++;
    uint32_t live = machine.nxchained, other_head = region->chain_heads[0];
    uint64_t examined = HV_STAT_TOTAL(&machine, smc_link_checks);
    uint32_t add_two = 0x91000800;
    check(hv_phys_write(&machine, BASE + PAGE, &add_two, 4) == 0, "write modified guest code");
    check(!region->chain_heads[1] && machine.nxchained == live - affected, "only changed page's links removed");
    check(HV_STAT_TOTAL(&machine, smc_link_checks) - examined == affected, "invalidation work independent of unrelated links");
    check(region->chain_heads[0] == other_head && region->chain_heads[2] != 0 &&
          machine.slots[unrelated_first] == 0x1234, "other pages' incoming links remain intact");
          check(check_lists(&machine, "after a write to code") == 0, "the hash and the page lists hold together (after a write to code)");
    run(20);
    check(region->chain_heads[1] != 0, "invalidated slots can chain to replacement code");
    check(check_lists(&machine, "after chaining again") == 0, "the hash and the page lists hold together (after chaining again)");
    hv_flush_blocks(&machine);
    check(!region->chain_heads[0] && !region->chain_heads[1] && !region->chain_heads[2] && !machine.nxchained,
          "full flush clears all per-page lists before slot reuse");
          check(check_lists(&machine, "after a full flush") == 0, "the hash and the page lists hold together (after a full flush)");
    run(20);
    check(region->chain_heads[0] && region->chain_heads[1], "chains rebuild after full flush");
    check(check_lists(&machine, "after rebuilding") == 0, "the hash and the page lists hold together (after rebuilding)");
    hv_machine_free(&machine);
    free(ram);
    return failures != 0;
}
