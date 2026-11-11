// Giving memory back on a low-memory warning (hv_machine_request_trim): the translation tables a full
// flush leaves written but unused go back to the system as zero pages, the process's footprint drops by
// that much, nothing happens without a request, a request waits for the next flush unless asked for now,
// and the guest runs the same code afterwards. With zones, the dispatcher's stop only notes what a trim gives back,
// and the CPU that stopped the others gives it back once they run again, for a CPU still parked too.
#include <dlfcn.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "dispatch.h"
#include "machine.h"

#define PAGE (1u << HV_RAM_PAGE_BITS) /* a RAM page: its code has its own generation */
#define BASE 0x80000000ull

static HvMachine machine;
static HvCpu cpu;
static int failures;

/* The engine's mmap is this one (linked into the same image). A give-back swaps pages with MAP_FIXED: each one is
   counted by whether a stop was still on (stw_active) when it ran. */
static unsigned fixed_in_stop, fixed_after;
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    static void *(*sys)(void *, size_t, int, int, int, off_t);
    if (flags & MAP_FIXED) {
        if (__atomic_load_n(&machine.stw_active, __ATOMIC_SEQ_CST)) __atomic_add_fetch(&fixed_in_stop, 1, __ATOMIC_RELAXED);
        else __atomic_add_fetch(&fixed_after, 1, __ATOMIC_RELAXED);
    }
    if (!sys) sys = (void *(*)(void *, size_t, int, int, int, off_t))dlsym(RTLD_NEXT, "mmap");
    return sys(addr, len, prot, flags, fd, off);
}

static void check(int ok, const char *what) {
    printf("%s: %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

static uint64_t footprint(void) {
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) != KERN_SUCCESS) return 0;
    return info.phys_footprint;
}

static int all_zero(const void *p, size_t bytes) {
    const uint8_t *b = p;
    for (size_t i = 0; i < bytes; i++)
        if (b[i]) return 0;
    return 1;
}

/* Entries [from, to) of table p, in the host pages wholly inside them (what a zone's give-back zeroes), read zero. */
static int zero_inside(const void *p, size_t from, size_t to, size_t size) {
    size_t page = (size_t)getpagesize();
    uintptr_t lo = ((uintptr_t)p + from * size + page - 1) / page * page, hi = ((uintptr_t)p + to * size) / page * page;
    return hi > lo && all_zero((const void *)lo, hi - lo);
}

static uint32_t branch(uint64_t from, uint64_t to) {
    return 0x14000000u | ((uint32_t)((int64_t)(to - from) / 4) & 0x03ffffff);
}

/* x0 counts up to x1 (10) round a loop that crosses a page: 20 with add #2, 10 with add #1. */
static uint64_t run(void) {
    hv_cpu_reset(&cpu, &machine);
    cpu.el = 0;
    cpu.spsel = 0;
    cpu.pc = BASE;
    cpu.x[1] = 10;
    cpu.cpacr_el1 = 3u << 20;
    machine.stop_pc = BASE + 2 * PAGE;
    int reason = hv_run(&cpu);
    return reason == HV_EXIT_HALT ? cpu.x[0] : ~0ull;
}

/* As a long run leaves them: most of each table written, then emptied by a flush. */
static void fill_tables(uint32_t blocks, uint32_t slots, uint32_t undef) {
    for (uint32_t i = 0; i < blocks; i++) {
        machine.blocks[i].va = 0x1000 + i;
        machine.blocks[i].jit_gen = 7;
    }
    for (uint32_t i = 0; i < slots; i++) {
        machine.slots[i] = 0x1234;
        machine.slot_site[i] = (uint32_t *)(uintptr_t)8;
        machine.slot_stub[i] = (uint32_t *)(uintptr_t)8;
        machine.slot_cond[i] = (uint32_t *)(uintptr_t)8;
        machine.slot_xa[i] = 1;
        machine.slot_xpg[i] = 1;
        machine.slot_xprev[i] = 1;
        machine.slot_xnext[i] = 1;
    }
    for (uint32_t i = 0; i < undef; i++) machine.undef_map[i] = 0xabcd;
    machine.nblocks = (int)blocks;
    machine.nslots = slots;
    machine.nundef = undef;
    /* One indirect-branch table of CPU 0 written all through */
    HvPerCpu *p = &machine.percpu[0];
    memset(p->ijc, 0x5a, HV_IJC_TABLE_BYTES);
    p->ijc_tables_used |= 1;
#ifndef OLD
    p->ijc_tables_held |= 1; /* as a fill marks it */
#endif
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (hv_machine_init(&machine, 64 << 20) != 0) return 1;
    uint32_t *ram = aligned_alloc(PAGE, 3 * PAGE);
    if (!ram || hv_machine_add_ram(&machine, BASE, 3 * PAGE, ram, 0) != 0) return 1;
    memset(ram, 0, 3 * PAGE);
    ram[0] = branch(BASE, BASE + PAGE);
    uint32_t *target = ram + PAGE / 4;
    target[0] = 0x91000800; /* add x0, x0, #2 */
    target[1] = 0xf1000421; /* subs x1, x1, #1 */
    target[2] = 0x54000001 | (((uint32_t)(-(int32_t)(PAGE + 8) / 4) & 0x7ffff) << 5); /* b.ne BASE */
    target[3] = branch(BASE + PAGE + 12, BASE + 2 * PAGE);
    check(run() == 20, "guest code runs before any trim");

    uint32_t blocks = machine.max_blocks * 3 / 4, slots = machine.max_slots * 3 / 4, undef = machine.max_undef / 2;
    fill_tables(blocks, slots, undef);
    hv_flush_blocks(&machine);
    check(machine.trims == 0, "a flush nobody asked to trim gives nothing back");
    check(machine.blocks[blocks - 1].va == 0x1000 + blocks - 1, "an ordinary flush leaves the tables as they were");

    uint64_t expect = (uint64_t)blocks * sizeof(HvBlock) + (uint64_t)undef * 4 +
                      (uint64_t)slots * (5 * sizeof(uint64_t) + 2 * sizeof(uint32_t *) - 4) + HV_IJC_TABLE_BYTES;
    hv_machine_request_trim(&machine, 0);
    check(machine.trim_pending == 1 && machine.flush_pending == 0, "a trim not asked for now waits for the next flush");
    fill_tables(blocks, slots, undef); /* the guest goes on, until the code cache fills and flushes */
    uint64_t before = footprint();
    hv_flush_blocks(&machine);
    uint64_t after = footprint();
    printf("tables written %.1f MB, given back %.1f MB, footprint %.1f -> %.1f MB\n", expect / 1048576.0,
           machine.trim_released / 1048576.0, before / 1048576.0, after / 1048576.0);
    check(machine.trims == 1 && !machine.trim_pending, "the flush did the trim asked for");
    check(machine.trim_released >= expect && machine.trim_released <= expect + 64 * 16384, "it gives back what was written");
    check(before > after && before - after >= machine.trim_released * 9 / 10, "the process's footprint falls by that much");
    check(all_zero(machine.blocks, (size_t)blocks * sizeof(HvBlock)) && all_zero(machine.slots, (size_t)slots * 8) &&
          all_zero(machine.slot_xnext, (size_t)slots * 4) && all_zero(machine.slot_xpg, (size_t)slots * 4) &&
          all_zero(machine.slot_xprev, (size_t)slots * 4) && all_zero(machine.undef_map, (size_t)undef * 4) &&
          all_zero(machine.percpu[0].ijc, HV_IJC_TABLE_BYTES), "the tables read as they started: zero");
    check(machine.hw_blocks == 0 && machine.hw_slots == 0 && machine.hw_undef == 0, "nothing is left to give back");

    hv_flush_blocks(&machine);
    check(machine.trims == 1, "the next flush doesn't trim again");

    /* The guest after a trim: the request reaches the dispatcher like any flush, and the code is translated again. */
    check(run() == 20, "guest code still runs after the trim");
    uint32_t add_one = 0x91000400; /* add x0, x0, #1 */
    check(hv_phys_write(&machine, BASE + PAGE, &add_one, 4) == 0, "write new guest code");
    hv_machine_request_trim(&machine, 1);
    check(machine.flush_pending == 1, "a trim asked for now asks for a full flush");
    check(run() == 10, "a trim left pending for the dispatcher: the guest runs the new code");
    check(machine.trims == 2 && machine.trim_released > 0, "the running dispatcher did the trim");

#ifndef OLD
    /* With zones, as on the device: the full flush in the dispatcher's stop retires a zone and notes the others'
       tables (all empty, but the one filled next); the CPU gives them back after letting the others go. */
    hv_machine_set_zones(&machine, 4);
    size_t z2 = (size_t)2 * machine.zone_blocks;
    for (uint32_t i = 0; i < 4096; i++) machine.blocks[z2 + i].va = 0x1000 + i;
    /* A CPU that stays parked (here: never runs) may not give its branch caches back itself for a long while: the
       stopper gives them back for it, and the trim counts as done, with them, only then. */
    size_t ijc1 = (size_t)1 << 20;
    memset(machine.percpu[1].ijc, 0x5a, ijc1);
    machine.percpu[1].ijc_tables_used |= 1;
#ifndef OLD
    machine.percpu[1].ijc_tables_held |= 1;
#endif
    unsigned trims0 = machine.trims;
    uint64_t gb0 = HV_STAT_TOTAL(&machine, giveback_ticks);
    hv_machine_request_trim(&machine, 1);
    check(run() == 10, "with 4 zones, a trim now: the guest runs on");
    check(machine.trims == trims0 + 1 && machine.trim_released >= 4096 * sizeof(HvBlock) * 9 / 10 + ijc1 * 9 / 10 &&
              HV_STAT_TOTAL(&machine, giveback_ticks) > gb0, "the dispatcher gave the trim's tables back after its stop, a parked CPU's caches too");
    check(all_zero(machine.blocks + z2 + 256, (4096 - 256) * sizeof(HvBlock)) /* past a page shared with zone 1 */ && !machine.giveback_pending && !machine.giveback_zones &&
              !machine.percpu[0].ijc_giveback, "  an empty zone's tables read zero, nothing left noted");
    check(all_zero(machine.percpu[1].ijc, ijc1) && !machine.percpu[1].ijc_giveback && !machine.giveback_left,
          "  the parked CPU's caches read zero, every part of the trim done");
    check(machine.zones_retired == 1u << 3 && machine.zone == 0, "one zone retired, the first one filled");

    /* The give-back is not part of the stop: it runs once the stopper has let the others go (stw_active 0). Every
       page swap it makes is seen (mmap above): none may run with the stop still on. The times are printed only. */
    size_t zb = machine.zone_blocks;
    for (size_t z = 1; z < 3; z++) memset(machine.blocks + z * zb, 0x5a, zb * sizeof(HvBlock)); /* zone 3 is retired */
    size_t zs = machine.zone_slots, ns = zs * 2;
#define FILL(a) memset(machine.a + zs, 0x5a, ns * sizeof *machine.a)
    FILL(slots), FILL(slot_site), FILL(slot_stub), FILL(slot_cond), FILL(slot_xa), FILL(slot_xpg), FILL(slot_xprev), FILL(slot_xnext);
#undef FILL
    uint64_t stops0 = machine.stats.stops, stop0 = machine.stats.stop_ticks;
    gb0 = HV_STAT_TOTAL(&machine, giveback_ticks);
    fixed_in_stop = fixed_after = 0;
    hv_machine_request_trim(&machine, 1);
    check(run() == 10, "a trim with tens of MB to give back: the guest runs on");
    uint64_t stop = machine.stats.stop_ticks - stop0, gave = HV_STAT_TOTAL(&machine, giveback_ticks) - gb0;
    printf("  %llu stop(s) took %.2f ms in all; the give-back after, %.2f ms (%.1f MB): %u page swaps after the stop, %u in it\n",
           (unsigned long long)(machine.stats.stops - stops0), stop / 24e3, gave / 24e3, machine.trim_released / 1048576.0,
           fixed_after, fixed_in_stop);
    check(machine.stats.stops > stops0 && machine.trim_released >= 2 * zb * sizeof(HvBlock) && fixed_after > 0 && fixed_in_stop == 0,
          "  the give-back ran after the stop, not in it");
    /* Every slot table of the zones given back, the pages' list links too (the host pages wholly in each zone's
       part: one shared with the next zone is kept, and translating sets a slot's links before it is used). */
    int zeroed = 1;
    for (size_t z = 1; z < 3; z++) {
#define ZERO(a) zero_inside(machine.a, z * zs, (z + 1) * zs, sizeof *machine.a)
        zeroed &= ZERO(slots) && ZERO(slot_xa) && ZERO(slot_xpg) && ZERO(slot_xprev) && ZERO(slot_xnext);
#undef ZERO
    }
    check(zeroed, "  the zones' exits read zero, their list links (slot_xpg, slot_xprev, slot_xnext) too");
#endif

    hv_machine_free(&machine);
    free(ram);
    return failures != 0;
}
