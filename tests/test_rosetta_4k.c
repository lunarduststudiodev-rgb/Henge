// Real page tables and translated instructions: noncontiguous 4 KB pages,
// permissions, page boundaries, code changes and local/remote invalidation.
// --bench N G runs the same memory/indirect-branch workload with guest granule
// 2^G, with either software TLB build. This is not an Intel app benchmark.
#include <mach/mach_time.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "codestore.h"
#include "dispatch.h"
#include "memop.h"
#include "mmu.h"
#include "rosetta.h"
#include "sysreg.h"

#define RAM_PA 0x40000000ull
#define RAM_SIZE (4u << 20)
#define PT_PA 0x80000000ull
#define PT_SIZE (2u << 20)
#define CODE_VA 0x100000000ull
#define DATA_VA 0x200100000ull
#define STOP_VA 0x700000000ull
#define VECTOR 0xfffffe0000000000ull
#define AF (1ull << 10)
#define AP(x) ((uint64_t)(x) << 6)
#define UXN (1ull << 54)
#define DESC (3 | AF | AP(1))

#define PROGRAM(n) extern const char n[], n##_end[]
PROGRAM(r4_a); PROGRAM(r4_b); PROGRAM(r4_c); PROGRAM(r4_d);
PROGRAM(r4_pair); PROGRAM(r4_atomic); PROGRAM(r4_jump);
PROGRAM(r4_value); PROGRAM(r4_wait);

static HvMachine m;
static HvCpu cpu, peer;
static uint8_t *ram, *pt;
static uint64_t next_pt, roots[2];
static unsigned granule;
static int checks, failures;

static void check(const char *what, uint64_t got, uint64_t want) {
    checks++;
    if (got == want) return;
    printf("FAIL %s: got 0x%llx, expected 0x%llx\n", what,
           (unsigned long long)got, (unsigned long long)want);
    failures++;
}

static uint64_t table(void) {
    uint64_t pa = PT_PA + next_pt;
    next_pt += 65536; // aligned for all three architectural granules
    if (next_pt > PT_SIZE) abort();
    return pa;
}

static uint64_t *map(uint64_t va, uint64_t pa, uint64_t attrs) {
    unsigned stride = granule - 3;
    unsigned levels = (48 - granule + stride - 1) / stride;
    uint64_t root = roots[(va >> 55) & 1];
    for (unsigned level = 4 - levels;; level++) {
        unsigned shift = granule + stride * (3 - level);
        unsigned bits = level == 4 - levels ? 48 - shift : stride;
        uint64_t *e = (uint64_t *)(pt + root - PT_PA) + ((va >> shift) & ((1ull << bits) - 1));
        if (level == 3) {
            __atomic_store_n(e, pa | attrs, __ATOMIC_RELEASE);
            return e;
        }
        if (!(*e & 1)) *e = table() | 3;
        root = *e & 0x0000ffffffff0000ull;
    }
}

static void configure_on(HvCpu *c, HvMachine *mm, unsigned index) {
    hv_cpu_reset_index(c, mm, index);
    c->ttbr0_el1 = roots[0] | 7ull << 48;
    c->ttbr1_el1 = roots[1];
    unsigned tg0 = granule == 12 ? 0 : granule == 14 ? 2 : 1;
    unsigned tg1 = granule == 12 ? 2 : granule == 14 ? 1 : 3;
    c->tcr_el1 = 16 | (uint64_t)tg0 << 14 | 16u << 16 | (uint64_t)tg1 << 30 | 5ull << 32;
    c->mair_el1 = 0xff;
    c->sctlr_el1 |= 1;
    c->cpacr_el1 = 3u << 20;
    c->actlr_el1 = 2; // software TSO, as used by Rosetta
    c->el = 0;
    c->spsel = 0;
    c->vbar_el1 = VECTOR;
    c->x[15] = STOP_VA;
    hv_tlb_flush(c);
    hv_ijc_switch(c);
}

static void configure(HvCpu *c, unsigned index) { configure_on(c, &m, index); }

static void setup(unsigned g) {
    granule = g;
    hv_flush_blocks(&m);
    m.ncpus = 1;
    m.cpus[1] = NULL;
    memset(pt, 0, PT_SIZE);
    next_pt = 0;
    roots[0] = table(); roots[1] = table();
    configure(&cpu, 0);
    m.stop_pc = STOP_VA;
    m.stop_pc2 = VECTOR + 0x400; // EL0 synchronous abort, no handler needed
}

static uint64_t data_pa(unsigned i) {
    static const unsigned offsets[] = {0x30000, 0x10000, 0x70000, 0x50000};
    return RAM_PA + offsets[i];
}

static void data_pages(void) {
    for (unsigned i = 0; i < 4; i++) {
        uint64_t va = DATA_VA + (i << granule);
        map(va, data_pa(i), DESC | UXN);
        uint64_t value = i + 1;
        memcpy(ram + data_pa(i) - RAM_PA, &value, 8);
        cpu.x[i] = va;
    }
}

static void program(unsigned page, const char *begin, const char *end) {
    uint64_t pa = RAM_PA + 0x100000 + page * 0x20000;
    map(CODE_VA + ((uint64_t)page << granule), pa, 3 | AF | AP(3));
    check("install guest code", hv_phys_write(&m, pa, begin, (unsigned)(end - begin)), 0);
}

static void run(uint64_t pc) {
    cpu.pc = pc;
    check("guest exit", hv_run(&cpu), HV_EXIT_HALT);
}

static void invalidate(HvCpu *c, uint64_t va) {
    hv_tlbi_apply(c, 0, 8, 7, 1, (7ull << 48) | ((va >> 12) & 0xfffffffffffull));
}

static uint64_t translate(HvCpu *c, uint64_t va) {
    uint64_t pa = 0;
    check("translate read", hv_mmu_access(c, va, HV_ACC_READ, (va >> 55) & 1, &pa), 0);
    return pa;
}

static void boundaries(void) {
    setup(12); data_pages();
    program(0, r4_pair, r4_pair_end);
    uint64_t a = 0x1122334455667788ull, b = 0x8877665544332211ull;
    memcpy(ram + data_pa(0) - RAM_PA + 4088, &a, 8);
    memcpy(ram + data_pa(1) - RAM_PA, &b, 8);
    cpu.x[0] = DATA_VA + 4088;
    cpu.x[1] = DATA_VA + 8192 + 4088;
    // Warm all four separate translations before an access crosses the boundary.
    for (int i = 0; i < 4; i++)
        check("noncontiguous physical mapping", translate(&cpu, DATA_VA + i * 4096), data_pa(i));
    run(CODE_VA);
    check("pair first word", cpu.x[5], a);
    check("pair second word", cpu.x[6], b);
    check("SIMD first word", (uint64_t)cpu.v[0], a);
    check("SIMD second word", (uint64_t)(cpu.v[0] >> 64), b);
    uint64_t got;
    memcpy(&got, ram + data_pa(2) - RAM_PA + 4088, 8);
    check("cross-page store first", got, a);
    memcpy(&got, ram + data_pa(3) - RAM_PA, 8);
    check("cross-page store second", got, b);

    map(DATA_VA + 12288, data_pa(3), 3 | AF | AP(3) | UXN);
    invalidate(&cpu, DATA_VA + 12288);
    uint64_t sentinel = 0xdeadbeef;
    memcpy(ram + data_pa(3) - RAM_PA, &sentinel, 8);
    run(CODE_VA);
    check("cross-page write permission fault", cpu.esr_el1 & 63, HV_FSC_PERMISSION(3));
    check("fault address is protected neighbour", cpu.far_el1, DATA_VA + 12288);
    memcpy(&got, ram + data_pa(3) - RAM_PA, 8);
    check("protected neighbour unchanged", got, sentinel);

    configure(&cpu, 0);
    map(DATA_VA + 4096, 0, 0);
    invalidate(&cpu, DATA_VA + 4096);
    cpu.x[0] = DATA_VA + 4088;
    cpu.x[1] = DATA_VA + 8192;
    run(CODE_VA);
    check("unmapped neighbour faults", cpu.esr_el1 & 63, HV_FSC_TRANSLATION(3));
    check("unmapped neighbour FAR", cpu.far_el1, DATA_VA + 4096);
}

static void atomics(void) {
    setup(12); data_pages();
    program(0, r4_atomic, r4_atomic_end);
    for (unsigned i = 0; i < 4; i++) {
        cpu.x[0] = DATA_VA + i * 4096;
        cpu.x[1] = DATA_VA + ((i + 1) % 4) * 4096;
        cpu.x[2] = DATA_VA + ((i + 2) % 4) * 4096 + 32;
        uint64_t before = 0, got = 0;
        memcpy(&before, ram + data_pa(i) - RAM_PA, 8);
        run(CODE_VA);
        memcpy(&got, ram + data_pa(i) - RAM_PA, 8);
        check("exclusive increment on each 4K page", got, before + 1);
        memcpy(&got, ram + data_pa((i + 2) % 4) - RAM_PA + 32, 8);
        check("ordered store on another page", got, before + 1);
    }
    cpu.x[0] = DATA_VA + 4093;
    run(CODE_VA);
    check("unaligned atomic faults", cpu.esr_el1 & 63, HV_FSC_ALIGNMENT);
}

static void code_changes(void) {
    setup(12); data_pages();
    program(0, r4_jump, r4_jump_end);
    program(1, r4_value, r4_value_end);
    cpu.x[9] = CODE_VA + 4096;
    run(CODE_VA); run(CODE_VA); // target is translated and its indirect entry warm
    check("initial indirect target", cpu.x[5], 77);
    uint64_t target_pa = RAM_PA + 0x120000;
    uint32_t changed = 0xd2800b05u; // mov x5, #88
    check("DMA code write", hv_phys_write(&m, target_pa, &changed, 4), 0);
    run(CODE_VA);
    check("changed code replaces cached target", cpu.x[5], 88);

    // A writable alias must invalidate translated code on the same physical page.
    map(DATA_VA, target_pa, DESC | UXN);
    invalidate(&cpu, DATA_VA);
    changed = 0xd2800c65u; // mov x5, #99
    check("write through code alias", hv_vmem_xfer(&cpu, DATA_VA, &changed, 4, 1, 0, CODE_VA), 0);
    run(CODE_VA);
    check("alias write invalidates code", cpu.x[5], 99);

    map(CODE_VA + 4096, target_pa, 3 | AF | AP(3) | UXN);
    invalidate(&cpu, CODE_VA + 4096);
    cpu.x[5] = 0;
    run(CODE_VA);
    check("cached target cannot bypass XN", cpu.esr_el1 & 63, HV_FSC_PERMISSION(3));
    check("XN target did not execute", cpu.x[5], 0);
}

static void invalidations(void) {
    for (unsigned g = 12; g <= (HENGE_ROSETTA_4K_CACHE ? 16u : 14u); g += 2) {
        setup(g);
        for (int high = 0; high < 2; high++) {
            uint64_t va = high ? 0xfffffe0010000000ull : DATA_VA;
            map(va, RAM_PA, DESC | UXN);
            for (unsigned off = 0; off < (1u << g); off += 4096)
                check("warm fragment", translate(&cpu, va + off), RAM_PA + off);
            map(va, RAM_PA + 0x80000, DESC | UXN);
            // Address the last 4 KB fragment, not necessarily the granule's base.
            invalidate(&cpu, va + (1u << g) - 4096);
            for (unsigned off = 0; off < (1u << g); off += 4096)
                check("whole guest granule invalidated", translate(&cpu, va + off), RAM_PA + 0x80000 + off);
        }
    }
    setup(12);
    for (int i = 0; i < 6; i++) {
        map(DATA_VA + i * 4096, RAM_PA + i * 4096, DESC | UXN);
        translate(&cpu, DATA_VA + i * 4096);
        if (i > 0 && i < 5) map(DATA_VA + i * 4096, RAM_PA + 0x80000 + i * 4096, DESC | UXN);
    }
    uint64_t xt = 7ull << 48 | 1ull << 46 | 1ull << 39 | ((DATA_VA + 4096) >> 12);
    hv_tlbi_apply(&cpu, 0, 8, 6, 1, xt); // four 4 KB pages
    for (int i = 0; i < 6; i++)
        check("4K range invalidation", translate(&cpu, DATA_VA + i * 4096),
              RAM_PA + i * 4096 + (i > 0 && i < 5 ? 0x80000 : 0));
}

static void write_register(uint32_t instruction, uint64_t value) {
    cpu.el = 1;
    cpu.x[0] = value;
    check("system register write", hv_sysreg_msr(&cpu, instruction, CODE_VA), 0);
    cpu.el = 0;
}

static void address_spaces(void) {
    setup(12);
    map(DATA_VA, RAM_PA, DESC | UXN);
    uint64_t first = cpu.ttbr0_el1;
    roots[0] = table();
    map(DATA_VA, RAM_PA + 0x80000, DESC | UXN);
    uint64_t second = roots[0] | 9ull << 48;
    check("first address space", translate(&cpu, DATA_VA), RAM_PA);
    write_register(0xd5182000u, second); // MSR TTBR0_EL1, X0
    check("second address space", translate(&cpu, DATA_VA), RAM_PA + 0x80000);
    write_register(0xd5182000u, first);
    check("restored address space", translate(&cpu, DATA_VA), RAM_PA);
    write_register(0xd5182000u, second);
    roots[0] = first & 0x0000ffffffffffffull;
    map(DATA_VA, RAM_PA + 0x90000, DESC | UXN);
    invalidate(&cpu, DATA_VA); // invalidate ASID 7 while ASID 9 is running
    write_register(0xd5182000u, first);
    check("inactive address space cannot restore stale entry", translate(&cpu, DATA_VA), RAM_PA + 0x90000);
    write_register(0xd5182000u, second);
    check("other address space remains correct", translate(&cpu, DATA_VA), RAM_PA + 0x80000);

    // A TCR change must discard a saved address space as well as current entries.
    roots[0] = first & 0x0000ffffffffffffull;
    map(DATA_VA, RAM_PA + 0xa0000, DESC | UXN);
    write_register(0xd5182040u, cpu.tcr_el1 ^ (1ull << 37)); // MSR TCR_EL1, X0 (TBI0)
    write_register(0xd5182000u, first);
    check("TCR change drops saved translations", translate(&cpu, DATA_VA), RAM_PA + 0xa0000);
}

// 4 KB pages that are one aligned run of physical memory with the same attributes (how macOS maps
// an Intel app's memory from its 16 KB pages): the 16 KB software TLB holds them as one page, so the
// warm workload takes no slow path. A change to one of them, invalidated by its own address, is seen.
#define RUN_PA (RAM_PA + 0x200000)
#define RUN_DATA_PA (RAM_PA + 0x240000)

static void run_pages(void) {
    setup(12);
    const char *begin[] = {r4_a, r4_b, r4_c, r4_d}, *end[] = {r4_a_end, r4_b_end, r4_c_end, r4_d_end};
    for (unsigned i = 0; i < 4; i++) {
        uint64_t pa = RUN_PA + i * 4096, va = CODE_VA + i * 4096;
        map(va, pa, 3 | AF | AP(3));
        check("install guest code", hv_phys_write(&m, pa, begin[i], (unsigned)(end[i] - begin[i])), 0);
        cpu.x[9 + i] = va;
        map(DATA_VA + i * 4096, RUN_DATA_PA + i * 4096, DESC | UXN);
        uint64_t value = i + 1;
        memcpy(ram + RUN_DATA_PA - RAM_PA + i * 4096, &value, 8);
        cpu.x[i] = DATA_VA + i * 4096;
    }
}

static uint64_t sum_pages(uint64_t iterations) {
    cpu.x[4] = iterations; cpu.x[5] = 0;
    run(CODE_VA);
    return cpu.x[5];
}

static void joined_pages(void) {
    run_pages();
    check("one run: first pass", sum_pages(4), 4 * 10);
    uint64_t slow = HV_STAT_TOTAL(&m, slow_mem), walks = HV_STAT_TOTAL(&m, fetch_walks);
    check("one run: warm pass", sum_pages(1000), 1000 * 10);
    check("one run of 4K pages: no slow memory accesses", HV_STAT_TOTAL(&m, slow_mem) - slow, 0);
    check("one run of 4K pages: no instruction walks", HV_STAT_TOTAL(&m, fetch_walks) - walks, 0);
    check("one run: each page where it is", translate(&cpu, DATA_VA + 12288 + 8), RUN_DATA_PA + 12288 + 8);

    // One page of the run moves elsewhere; its own TLBI is enough.
    uint64_t moved = RAM_PA + 0x280000, value = 100;
    memcpy(ram + moved - RAM_PA, &value, 8);
    map(DATA_VA + 8192, moved, DESC | UXN);
    invalidate(&cpu, DATA_VA + 8192);
    check("a moved page is seen", sum_pages(1), 1 + 2 + 100 + 4);
    check("the run's other pages stay", translate(&cpu, DATA_VA + 4096), RUN_DATA_PA + 4096);
    if (HV_TLB_PAGE_BITS > 12) { // no longer one run: the 4 KB table still holds each page
        sum_pages(1);
        slow = HV_STAT_TOTAL(&m, slow_mem);
        sum_pages(10);
        check("no longer one run: still no slow memory accesses", HV_STAT_TOTAL(&m, slow_mem) - slow, 0);
        check("no longer one run: each page where it is", sum_pages(1), 1 + 2 + 100 + 4);
    }

    // One page of the run becomes read-only: writes to it fault, even with the rest warm.
    run_pages();
    sum_pages(4);
    map(DATA_VA + 12288, RUN_DATA_PA + 12288, 3 | AF | AP(3) | UXN);
    invalidate(&cpu, DATA_VA + 12288);
    uint64_t before;
    memcpy(&before, ram + RUN_DATA_PA - RAM_PA + 12288 + 16, 8);
    sum_pages(1);
    check("read-only page in the run: write faults", cpu.esr_el1 & 63, HV_FSC_PERMISSION(3));
    check("read-only page in the run: fault address", cpu.far_el1, DATA_VA + 12288 + 16);
    uint64_t after;
    memcpy(&after, ram + RUN_DATA_PA - RAM_PA + 12288 + 16, 8);
    check("read-only page in the run: unchanged", after, before);

    // A page of the run changed without a TLBI of its own (one of everything is coming): the check that may
    // leave such a flush out sees the joined entry is out of date.
    run_pages();
    sum_pages(4);
    check("joined entries match the tables", (uint64_t)hv_tlb_check_all(&cpu), 0);
    map(DATA_VA + 4096, RAM_PA + 0x2c0000, DESC | UXN);
    check("a changed page of a joined entry is found", hv_tlb_check_all(&cpu) > 0, 1);

    // Each 4 KB page of a run fills its own 4 KB entry, but the run's one software-page entry only once: the
    // places a switch keeps a process's entries by (user_fills) aren't spent four times over.
    run_pages();
    hv_tlb_flush(&cpu);
    uint32_t fills = cpu.nuser_fills;
    for (unsigned i = 0; i < 4; i++) translate(&cpu, DATA_VA + i * 4096);
    check("one run: one software-page entry, four 4 KB ones", cpu.nuser_fills - fills, HV_TLB_SMALL ? 5 : 4);

    // The joined counter counts the walks that fill the TLB, one each, not the walks of a check.
    if (HV_TLB_PAGE_BITS > 12) {
        run_pages();
        sum_pages(4);
        m.stats_interval = 1;
        uint64_t before[4], sum = 0;
        for (int i = 0; i < 4; i++) before[i] = HV_STAT_TOTAL(&m, small_join[i]);
        check("joined counter: entries to check", hv_tlb_check_all(&cpu), 0); // its walks only look
        check("joined counter: the check walked them", HV_STAT_TOTAL(&m, flush_check_entries) > 0, 1);
        for (int i = 0; i < 4; i++) sum += HV_STAT_TOTAL(&m, small_join[i]) - before[i];
        check("joined counter: a check's walks are not counted", sum, 0);
        invalidate(&cpu, DATA_VA);
        translate(&cpu, DATA_VA + 8); // one fill
        check("joined counter: a fill is counted once", HV_STAT_TOTAL(&m, small_join[0]) - before[0], 1);
        translate(&cpu, DATA_VA + 16); // in the TLB now: no walk
        check("joined counter: a hit is not counted", HV_STAT_TOTAL(&m, small_join[0]) - before[0], 1);
        m.stats_interval = 0;
    }
}

// Code in a run of 4 KB pages (one software page in the TLB): a block must not run on into the next 4 KB page,
// nor branch to it unchecked, since that page may be remapped alone. Its own TLBI must be enough to run its new
// code, whether the old code was reached by falling through the page's end or by a branch already chained.
#define CODE_RUN_PA (RAM_PA + 0x300000)
#define CODE_MOVED_PA (RAM_PA + 0x340000)

static void put_word(uint64_t pa, uint32_t insn) {
    check("install guest code", hv_phys_write(&m, pa, &insn, 4), 0);
}

static void code_in_joined_pages(void) {
    const uint32_t movz_x5_1 = 0xd2800025u, add_10 = 0x910028a5u, add_20 = 0x910050a5u, br_x15 = 0xd61f01e0u;
    setup(12);
    for (unsigned i = 0; i < 4; i++) map(CODE_VA + i * 4096, CODE_RUN_PA + i * 4096, 3 | AF | AP(3));
    put_word(CODE_RUN_PA + 4092, movz_x5_1); // runs on into the next 4 KB page
    put_word(CODE_RUN_PA, movz_x5_1);        // branches to it
    put_word(CODE_RUN_PA + 4, 0x14000000u | (4096 + 0x100 - 4) / 4);
    for (unsigned off = 0; off <= 0x100; off += 0x100) {
        put_word(CODE_RUN_PA + 4096 + off, add_10); put_word(CODE_RUN_PA + 4096 + off + 4, br_x15);
        put_word(CODE_MOVED_PA + off, add_20); put_word(CODE_MOVED_PA + off + 4, br_x15);
    }
    for (int i = 0; i < 3; i++) { // warm: translated, chained, joined in the TLB
        run(CODE_VA + 4092);
        check("falls into the next 4K page", cpu.x[5], 11);
        run(CODE_VA);
        check("branches to the next 4K page", cpu.x[5], 11);
    }
    map(CODE_VA + 4096, CODE_MOVED_PA, 3 | AF | AP(3));
    invalidate(&cpu, CODE_VA + 4096);
    run(CODE_VA + 4092);
    check("falling through: the remapped 4K page's new code runs", cpu.x[5], 21);
    run(CODE_VA);
    check("branching: the remapped 4K page's new code runs", cpu.x[5], 21);
}

// A TCR write that changes only TTBR0's controls (a switch between a 4 KB and a 16 KB process) drops user
// entries but keeps the kernel's; one that changes anything else drops both.
static void user_granule_switch(void) {
    setup(14);
    uint64_t kva = 0xfffffe0010000000ull;
    map(kva, RAM_PA, DESC | UXN);
    map(DATA_VA, RAM_PA + 0x80000, DESC | UXN);
    translate(&cpu, kva);
    translate(&cpu, DATA_VA);
    const HvTlbEntry *k = hv_tlb_ent(&cpu, (uint32_t)(kva >> HV_TLB_PAGE_BITS) & (HV_TLB_SIZE - 1), 1);
    const HvTlbEntry *u = hv_tlb_ent(&cpu, (uint32_t)(DATA_VA >> HV_TLB_PAGE_BITS) & (HV_TLB_SIZE - 1), 0);
    check("kernel entry filled", k->read_tag != HV_TLB_INVALID, 1);
    check("user entry filled", u->read_tag != HV_TLB_INVALID, 1);
    write_register(0xd5182040u, cpu.tcr_el1 & ~(3ull << 14)); // MSR TCR_EL1, X0: TG0 16 KB to 4 KB
    check("TG0 change keeps kernel entries", k->read_tag != HV_TLB_INVALID, 1);
    check("TG0 change drops user entries", u->read_tag != HV_TLB_INVALID, 0);
    write_register(0xd5182040u, cpu.tcr_el1 | 2ull << 14); // and back
    check("TG0 back brings the user entries back", u->read_tag != HV_TLB_INVALID, 1);
    check("kernel still translates", translate(&cpu, kva), RAM_PA);
    write_register(0xd5182040u, cpu.tcr_el1 ^ (1ull << 38)); // TBI1: the kernel's half
    check("TTBR1 control change drops kernel entries", k->read_tag != HV_TLB_INVALID, 0);
}

static unsigned live_user_entries(void) {
    unsigned n = 0;
    for (uint32_t k = 0; k < HV_TLB_TABLES * HV_TLB_SIZE; k++) {
        const HvTlbEntry *e = hv_tlb_ent(&cpu, k % HV_TLB_SIZE, (int)(k / HV_TLB_SIZE));
        if (e->read_tag != HV_TLB_INVALID && !(((e->read_tag + HV_TLB_ADJ) >> 55) & 1)) n++;
    }
    return n;
}

static void scattered_program(void) {
    setup(12); data_pages();
    program(0, r4_a, r4_a_end); program(1, r4_b, r4_b_end);
    program(2, r4_c, r4_c_end); program(3, r4_d, r4_d_end);
    for (unsigned i = 0; i < 4; i++) cpu.x[9 + i] = CODE_VA + ((uint64_t)i << 12);
}

static uint64_t scattered_sum(uint64_t iterations) {
    cpu.x[0] = DATA_VA; // (write_register's)
    cpu.x[4] = iterations; cpu.x[5] = 0;
    run(CODE_VA);
    return cpu.x[5];
}

// macOS writes TCR_EL1.TG0 on every switch between an Intel app (4 KB pages) and a native one, before or after the
// TTBR0 write. The Intel app's entries are kept aside under its TTBR0 and TCR, as a TLB tagged by ASID keeps them,
// so it runs warm as soon as it is back; a TLBI of one of its pages while it was away is still seen; and entries
// kept under one TCR never come back under another.
static void granule_switches(void) {
    scattered_program();
    scattered_sum(8);
    uint64_t intel = cpu.ttbr0_el1, tcr4 = cpu.tcr_el1, tcr16 = (tcr4 & ~(3ull << 14)) | 2ull << 14;
    uint64_t native = table() | 9ull << 48; // nothing runs in it
    const uint32_t ttbr0 = 0xd5182000u, tcr = 0xd5182040u;
    for (int order = 0; order < 2; order++) {
        if (order) write_register(ttbr0, native), write_register(tcr, tcr16);
        else write_register(tcr, tcr16), write_register(ttbr0, native);
        check("away: the Intel app's entries are not live", live_user_entries(), 0);
        if (order) write_register(tcr, tcr4), write_register(ttbr0, intel);
        else write_register(ttbr0, intel), write_register(tcr, tcr4);
        uint64_t slow = HV_STAT_TOTAL(&m, slow_mem), walks = HV_STAT_TOTAL(&m, fetch_walks);
        check("back: sum", scattered_sum(100), 100 * 10);
        check("back: no slow memory accesses", HV_STAT_TOTAL(&m, slow_mem) - slow, 0);
        check("back: no instruction walks", HV_STAT_TOTAL(&m, fetch_walks) - walks, 0);
        check("back: entries match the tables", (uint64_t)hv_tlb_check_all(&cpu), 0);
    }

    // A page moved while the app was away, with its TLBI then: seen when it is back.
    write_register(tcr, tcr16), write_register(ttbr0, native);
    uint64_t moved = RAM_PA + 0x90000, value = 100;
    memcpy(ram + moved - RAM_PA, &value, 8);
    map(DATA_VA + 8192, moved, DESC | UXN);
    invalidate(&cpu, DATA_VA + 8192); // ASID 7, the Intel app's
    write_register(ttbr0, intel), write_register(tcr, tcr4);
    check("a page moved while away is seen", scattered_sum(1), 1 + 2 + 100 + 4);

    // Back under another TCR (its TTBR0 with the native granule): nothing comes back until its own TCR is.
    scattered_sum(4);
    write_register(tcr, tcr16), write_register(ttbr0, native);
    write_register(ttbr0, intel);
    check("its TTBR0 under another TCR: nothing kept comes back", live_user_entries(), 0);
    write_register(tcr, tcr4);
    check("its own TCR again: its entries come back", live_user_entries() > 0, 1);
    check("and match the tables", (uint64_t)hv_tlb_check_all(&cpu), 0);
    check("and run", scattered_sum(1), 1 + 2 + 100 + 4);
}

// A 4 KB build holds a 16 KB process's pages as four 4 KB entries each. Kept aside while a 4 KB process runs, a TLBI of
// any address in such a page, though the TCR then current says 4 KB pages, drops all four.
static void kept_granules(void) {
    setup(14);
    map(DATA_VA, RAM_PA + 0x80000, DESC | UXN);
    for (unsigned off = 0; off < 16384; off += 4096) translate(&cpu, DATA_VA + off);
    uint64_t native = cpu.ttbr0_el1, tcr16 = cpu.tcr_el1, tcr4 = tcr16 & ~(3ull << 14);
    uint64_t intel = table() | 9ull << 48;
    const uint32_t ttbr0 = 0xd5182000u, tcr = 0xd5182040u;
    write_register(tcr, tcr4), write_register(ttbr0, intel);
    map(DATA_VA, RAM_PA + 0xc0000, DESC | UXN); // the 16 KB process's page moves
    invalidate(&cpu, DATA_VA + 4096);          // its TLBI, by one address in it
    write_register(ttbr0, native), write_register(tcr, tcr16);
    for (unsigned off = 0; off < 16384; off += 4096)
        check("a kept 16 KB page is dropped whole", translate(&cpu, DATA_VA + off), RAM_PA + 0xc0000 + off);
    check("and the rest match the tables", (uint64_t)hv_tlb_check_all(&cpu), 0);
}

// The RAM-file writer (auto-save) takes back every writable entry to the pages it copies. Four writable 4 KB entries
// to one RAM page, scattered so they can't be joined (in a 16 KB build they sit in the 4 KB table), are more than the
// hints name, so the writer scans: all four are dropped, or a store could change the page while it is being copied.
static unsigned writable_to(uint64_t pa) {
    const uint8_t *page = ram + ((pa - RAM_PA) & ~((1ull << HV_RAM_PAGE_BITS) - 1));
    unsigned n = 0;
    for (uint32_t k = 0; k < HV_TLB_TABLES * HV_TLB_SIZE; k++) {
        const HvTlbEntry *e = hv_tlb_ent(&cpu, k % HV_TLB_SIZE, (int)(k / HV_TLB_SIZE));
        if (e->write_tag == HV_TLB_INVALID) continue;
        const uint8_t *host = (const uint8_t *)(uintptr_t)(e->write_tag + HV_TLB_ADJ + e->addend);
        if (host >= page && host < page + (1u << HV_RAM_PAGE_BITS)) n++;
    }
    return n;
}

static void revoke_writes(void) {
    setup(12);
    uint64_t base = RAM_PA + 0xa0000; // 16 KB aligned: one RAM page of a 16 KB build
    for (unsigned i = 0; i < 4; i++) map(DATA_VA + ((uint64_t)i << 12), base + ((uint64_t)(3 - i) << 12), DESC | UXN);
    for (unsigned i = 0; i < 4; i++) {
        uint64_t pa = 0;
        check("write translates", hv_mmu_access(&cpu, DATA_VA + ((uint64_t)i << 12), HV_ACC_WRITE, 0, &pa), 0);
    }
    check("four writable entries to the page", writable_to(base), 4);
    uint32_t page = (uint32_t)((base - RAM_PA) >> HV_RAM_PAGE_BITS); /* the RAM file's page: 16 KB in either build */
    hv_ram_revoke_writes(&m, &m.ram[0], &page, 1);
    check("the writer takes back every writable entry", writable_to(base), 0);
}

// A suspended Mac's translated code (core/codestore.c), saved while an Intel app ran. Its blocks' exits to another
// 4 KB page check a key made from that 4 KB page; the store takes them as they are into the resumed Mac, whose RAM is
// at another host address, remaking each key: nothing is translated again and the exits chain.
static uint32_t branch_to(uint64_t from, uint64_t to, uint32_t op) {
    int64_t words = (int64_t)(to - from) / 4;
    return op == 0x14000000u ? op | ((uint32_t)words & 0x03ffffff) : op | ((uint32_t)words & 0x7ffff) << 5;
}

static void code_store_small(void) {
    setup(12);
    uint64_t pa0 = RAM_PA + 0x1e0000, pa1 = RAM_PA + 0x160000, p1 = CODE_VA + 4096; // scattered: never joined
    map(CODE_VA, pa0, 3 | AF | AP(3));
    map(p1, pa1, 3 | AF | AP(3));
    uint32_t page0[] = {0x910004a5 /* add x5, x5, #1 */, branch_to(CODE_VA + 4, p1, 0x14000000u)};
    uint32_t page1[] = {0x910008a5 /* add x5, x5, #2 */, 0xf1000484 /* subs x4, x4, #1 */,
                        branch_to(p1 + 8, CODE_VA, 0x54000001u) /* b.ne */, 0xd61f01e0 /* br x15 */};
    check("install guest code", hv_phys_write(&m, pa0, page0, sizeof page0), 0);
    check("install guest code", hv_phys_write(&m, pa1, page1, sizeof page1), 0);
    cpu.x[4] = 10; cpu.x[5] = 0;
    run(CODE_VA);
    check("code store: the loop runs", cpu.x[5], 30);

    char path[512];
    const char *tmp = getenv("TMPDIR");
    snprintf(path, sizeof path, "%s/hv_rosetta_4k_store_%d.hvsnap", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    HvCodeKey key;
    hv_code_key_new(&key);
    check("code store: saved", (uint64_t)hv_code_store_save(&m, path, &key, NULL, 0), 0);

    static HvMachine m2;
    static HvCpu cpu2;
    uint8_t *ram2 = aligned_alloc(65536, RAM_SIZE), *pt2 = aligned_alloc(65536, PT_SIZE);
    if (!ram2 || !pt2 || hv_machine_init(&m2, 16u << 20)) abort();
    memcpy(ram2, ram, RAM_SIZE);
    memcpy(pt2, pt, PT_SIZE);
    if (hv_machine_add_ram(&m2, RAM_PA, RAM_SIZE, ram2, 0) || hv_machine_add_ram(&m2, PT_PA, PT_SIZE, pt2, 0)) abort();
    m2.stop_pc = m.stop_pc;
    m2.stop_pc2 = m.stop_pc2;
    long n = hv_code_store_load(&m2, path, &key);
    check("code store: blocks loaded", n > 0, 1);
    configure_on(&cpu2, &m2, 0);
    uint64_t t = HV_STAT_TOTAL(&m2, translations);
    cpu2.pc = CODE_VA;
    cpu2.x[4] = 10; cpu2.x[5] = 0;
    check("code store: loaded loop exit", hv_run(&cpu2), HV_EXIT_HALT);
    check("code store: the loaded loop runs", cpu2.x[5], 30);
    check("code store: nothing translated again", HV_STAT_TOTAL(&m2, translations) - t, 0);
    check("code store: its exits to another 4 KB page chain", m2.nxchained > 0, 1);
    hv_machine_free(&m2);
    free(pt2); free(ram2);
    hv_code_store_remove(path);
}

// An Intel app writes code through a writable alias (as Rosetta's JIT does), then runs it. Once the page is code,
// the alias's 4 KB entry may no longer write it directly: the next write must reach the translated code. Also
// through the 4 KB table's last slot, the one whose write hint can't name it.
static void code_through_small_alias(uint64_t alias) {
    setup(12);
    uint64_t code_va = CODE_VA + 8192, code_pa = RAM_PA + 0x1a0000, src = DATA_VA, src_pa = data_pa(2);
    program(0, r4_pair, r4_pair_end);
    map(code_va, code_pa, 3 | AF | AP(3));
    map(alias, code_pa, DESC | UXN);
    map(src, src_pa, DESC | UXN);
    uint32_t first[4] = {0xd28009a5 /* mov x5, #77 */, 0xd61f01e0 /* br x15 */, 0, 0};
    memcpy(ram + src_pa - RAM_PA, first, sizeof first);
    cpu.x[0] = src; cpu.x[1] = alias;
    run(CODE_VA); // writes the code: a writable 4 KB entry for the alias
    unsigned bits;
    check("alias: a writable entry in its table", hv_tlb_at(&cpu, alias, hv_tlb_where(&cpu, alias, 0, &bits), bits)->write_tag != HV_TLB_INVALID, 1);
    cpu.x[5] = 0;
    run(code_va);
    check("alias: the written code runs", cpu.x[5], 77);
    uint32_t second[4] = {0xd2800b05 /* mov x5, #88 */, 0xd61f01e0, 0, 0};
    memcpy(ram + src_pa - RAM_PA, second, sizeof second);
    cpu.x[0] = src; cpu.x[1] = alias;
    run(CODE_VA); // writes it again, now that it is code
    cpu.x[5] = 0;
    run(code_va);
    check("alias: code written again runs its new instructions", cpu.x[5], 88);
}

// A TLBI of a whole address space (ASIDE1) while the Intel app's entries are kept aside: none comes back.
static void stashed_asid_drop(void) {
    scattered_program();
    scattered_sum(8);
    uint64_t intel = cpu.ttbr0_el1, tcr4 = cpu.tcr_el1, tcr16 = (tcr4 & ~(3ull << 14)) | 2ull << 14;
    uint64_t native = table() | 9ull << 48;
    const uint32_t ttbr0 = 0xd5182000u, tcr = 0xd5182040u;
    write_register(tcr, tcr16), write_register(ttbr0, native);
    uint64_t moved = RAM_PA + 0x90000, value = 100;
    memcpy(ram + moved - RAM_PA, &value, 8);
    map(DATA_VA + 8192, moved, DESC | UXN);
    hv_tlbi_apply(&cpu, 0, 8, 7, 2, 7ull << 48); // TLBI ASIDE1, ASID 7: the Intel app's
    write_register(ttbr0, intel), write_register(tcr, tcr4);
    check("ASID dropped while away: nothing comes back", live_user_entries(), 0);
    check("ASID dropped while away: the moved page is seen", scattered_sum(1), 1 + 2 + 100 + 4);
}

static void *run_peer(void *arg) { return (void *)(intptr_t)hv_run(arg); }

static void remote_invalidation(void) {
    setup(14); data_pages();
    program(0, r4_wait, r4_wait_end);
    configure(&peer, 1);
    peer.pc = CODE_VA;
    peer.x[0] = DATA_VA;
    translate(&peer, DATA_VA); // a live 16 KB mapping before the replacement
    // Initiator uses 4 KB user pages; receiver uses 16 KB. Its own granule wins.
    cpu.el = 1;
    cpu.tcr_el1 &= ~(3ull << 14);
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    if (pthread_create(&thread, &attr, run_peer, &peer)) abort();
    pthread_attr_destroy(&attr);
    while (!__atomic_load_n(&peer.pc_state->tlb_live, __ATOMIC_ACQUIRE)) sched_yield();
    map(DATA_VA, data_pa(1), DESC | UXN); // new page starts with 2
    cpu.x[1] = 7ull << 48 | ((DATA_VA + 12288) >> 12);
    check("broadcast VAE1IS", hv_sysreg_sys(&cpu, 0xd5088321u, 0), 0);
    void *result;
    pthread_join(thread, &result);
    check("remote CPU exit", (intptr_t)result, HV_EXIT_HALT);
    check("remote CPU saw new mapping", peer.x[5], 2);
    m.ncpus = 1; m.cpus[1] = NULL;
}

// The 4 KB table (16 KB builds): an Intel app's scattered 4 KB pages, each cached alone. Warm, nothing is slow and
// nothing is walked, branch-cache lookups included; a page moved, made read-only or unmapped by its own TLBI is seen
// at once while its 16 KB neighbours stay fast; the entries match the page tables; and another CPU's TLBI of one of
// them reaches a CPU running in that process.
static void small_table(void) {
    setup(12); data_pages();
    program(0, r4_a, r4_a_end); program(1, r4_b, r4_b_end);
    program(2, r4_c, r4_c_end); program(3, r4_d, r4_d_end);
    for (unsigned i = 0; i < 4; i++) cpu.x[9 + i] = CODE_VA + ((uint64_t)i << 12);
    cpu.x[4] = 8; cpu.x[5] = 0;
    run(CODE_VA);
    uint64_t slow = HV_STAT_TOTAL(&m, slow_mem), walks = HV_STAT_TOTAL(&m, fetch_walks), nomap = HV_STAT_TOTAL(&m, light_fail[2]);
    cpu.x[4] = 1000; cpu.x[5] = 0;
    run(CODE_VA);
    check("scattered 4K pages: sum", cpu.x[5], 1000 * 10);
    check("scattered 4K pages: no slow memory accesses", HV_STAT_TOTAL(&m, slow_mem) - slow, 0);
    check("scattered 4K pages: no instruction walks", HV_STAT_TOTAL(&m, fetch_walks) - walks, 0);
    check("scattered 4K pages: branch lookups find their pages", HV_STAT_TOTAL(&m, light_fail[2]) - nomap, 0);
    check("scattered 4K pages: entries match the tables", (uint64_t)hv_tlb_check_all(&cpu), 0);

    // One page moves, by its own TLBI; the other three are untouched.
    uint64_t moved = RAM_PA + 0x90000, value = 100;
    memcpy(ram + moved - RAM_PA, &value, 8);
    map(DATA_VA + 8192, moved, DESC | UXN);
    invalidate(&cpu, DATA_VA + 8192);
    cpu.x[4] = 1; cpu.x[5] = 0;
    run(CODE_VA);
    check("scattered 4K pages: a moved page is seen", cpu.x[5], 1 + 2 + 100 + 4);
    slow = HV_STAT_TOTAL(&m, slow_mem);
    cpu.x[4] = 100; cpu.x[5] = 0;
    run(CODE_VA);
    check("scattered 4K pages: still fast after the move", HV_STAT_TOTAL(&m, slow_mem) - slow, 0);

    // A page beside them becomes read-only (same 16 KB VA range): its write faults, the others' do not.
    map(DATA_VA + 4096, data_pa(1), 3 | AF | AP(3) | UXN);
    invalidate(&cpu, DATA_VA + 4096);
    uint64_t before;
    memcpy(&before, ram + data_pa(1) - RAM_PA + 16, 8);
    cpu.x[4] = 1; cpu.x[5] = 0;
    run(CODE_VA);
    check("scattered 4K pages: read-only page's write faults", cpu.esr_el1 & 63, HV_FSC_PERMISSION(3));
    check("scattered 4K pages: at its address", cpu.far_el1, DATA_VA + 4096 + 16);
    uint64_t after;
    memcpy(&after, ram + data_pa(1) - RAM_PA + 16, 8);
    check("scattered 4K pages: read-only page unchanged", after, before);

    // Unmapped: a translation fault, though the entry was there.
    configure(&cpu, 0); // (registers too)
    for (unsigned i = 0; i < 4; i++) cpu.x[9 + i] = CODE_VA + ((uint64_t)i << 12), cpu.x[i] = DATA_VA + ((uint64_t)i << 12);
    map(DATA_VA + 4096, data_pa(1), DESC | UXN);
    invalidate(&cpu, DATA_VA + 4096);
    cpu.x[4] = 4; cpu.x[5] = 0;
    run(CODE_VA);
    map(DATA_VA + 12288, 0, 0);
    invalidate(&cpu, DATA_VA + 12288);
    cpu.x[4] = 1; cpu.x[5] = 0;
    run(CODE_VA);
    check("scattered 4K pages: unmapped page faults", cpu.esr_el1 & 63, HV_FSC_TRANSLATION(3));
    check("scattered 4K pages: unmapped page FAR", cpu.far_el1, DATA_VA + 12288);

    // Code: one page's code moves by its own TLBI, reached through a branch that was cached.
    map(DATA_VA + 12288, data_pa(3), DESC | UXN);
    invalidate(&cpu, DATA_VA + 12288);
    program(1, r4_jump, r4_jump_end); // CODE_VA + 4096 now jumps straight on to x9
    configure(&cpu, 0); // (the same tables; its TLB and branch caches start empty)
    for (unsigned i = 0; i < 4; i++) cpu.x[9 + i] = CODE_VA + ((uint64_t)i << 12);
    cpu.x[0] = DATA_VA; cpu.x[1] = DATA_VA + 4096; cpu.x[2] = DATA_VA + 8192; cpu.x[3] = DATA_VA + 12288;
    program(1, r4_b, r4_b_end);
    cpu.x[4] = 4; cpu.x[5] = 0;
    run(CODE_VA); // warm: page 1 is r4_b
    uint64_t pa1 = RAM_PA + 0x1c0000;
    check("install guest code", hv_phys_write(&m, pa1, r4_value, (unsigned)(r4_value_end - r4_value)), 0);
    map(CODE_VA + 4096, pa1, 3 | AF | AP(3));
    invalidate(&cpu, CODE_VA + 4096);
    cpu.x[4] = 4; cpu.x[5] = 0;
    run(CODE_VA);
    check("scattered 4K pages: moved code page runs its new code", cpu.x[5], 77);
}

// Another CPU's TLBI of one 4 KB page reaches a CPU running a 4 KB-page process (its 4 KB table).
static void remote_small(void) {
    setup(12); data_pages();
    program(0, r4_wait, r4_wait_end);
    configure(&peer, 1);
    peer.pc = CODE_VA;
    peer.x[0] = DATA_VA;
    translate(&peer, DATA_VA); // a live 4 KB entry before the replacement
    cpu.el = 1;
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    if (pthread_create(&thread, &attr, run_peer, &peer)) abort();
    pthread_attr_destroy(&attr);
    while (!__atomic_load_n(&peer.pc_state->tlb_live, __ATOMIC_ACQUIRE)) sched_yield();
    map(DATA_VA, data_pa(1), DESC | UXN); // the new page starts with 2
    cpu.x[1] = 7ull << 48 | (DATA_VA >> 12);
    check("4K process: broadcast VAE1IS", hv_sysreg_sys(&cpu, 0xd5088321u, 0), 0);
    void *result;
    pthread_join(thread, &result);
    check("4K process: remote CPU exit", (intptr_t)result, HV_EXIT_HALT);
    check("4K process: remote CPU saw the new page", peer.x[5], 2);
    m.ncpus = 1; m.cpus[1] = NULL;
}

static void benchmark(uint64_t iterations, unsigned g) {
    setup(g); data_pages();
    program(0, r4_a, r4_a_end); program(1, r4_b, r4_b_end);
    program(2, r4_c, r4_c_end); program(3, r4_d, r4_d_end);
    for (unsigned i = 0; i < 4; i++) cpu.x[9 + i] = CODE_VA + ((uint64_t)i << g);
    cpu.x[4] = 32; cpu.x[5] = 0;
    run(CODE_VA);
    cpu.x[4] = iterations; cpu.x[5] = 0;
    uint64_t slow = HV_STAT_TOTAL(&m, slow_mem), walks = HV_STAT_TOTAL(&m, fetch_walks);
    uint64_t start = mach_absolute_time();
    run(CODE_VA);
    uint64_t elapsed = mach_absolute_time() - start;
    check("workload checksum", cpu.x[5], iterations * 10);
    slow = HV_STAT_TOTAL(&m, slow_mem) - slow; walks = HV_STAT_TOTAL(&m, fetch_walks) - walks;
    // Every granule, scattered 4 KB pages too (the 4 KB table of a 16 KB build): nothing slow once warm.
    check("warm workload has no slow memory accesses", slow, 0);
    check("warm workload has no instruction walks", walks, 0);
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    printf("BENCH software_page=%u guest_page=%u iterations=%llu ms=%.3f slow=%llu fetch_walks=%llu checksum=%llu\n",
           1u << HV_TLB_PAGE_BITS, 1u << g, (unsigned long long)iterations,
           elapsed * (double)timebase.numer / timebase.denom / 1e6,
           (unsigned long long)slow, (unsigned long long)walks, (unsigned long long)cpu.x[5]);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!hv_rosetta_host_supported()) return 77;
    setenv("HV_ROSETTA", "1", 1);
    alarm(45);
    if (hv_machine_init(&m, 16u << 20)) return 1;
    ram = aligned_alloc(65536, RAM_SIZE);
    pt = aligned_alloc(65536, PT_SIZE);
    if (!ram || !pt) return 1;
    memset(ram, 0, RAM_SIZE);
    if (hv_machine_add_ram(&m, RAM_PA, RAM_SIZE, ram, 0) ||
        hv_machine_add_ram(&m, PT_PA, PT_SIZE, pt, 0)) return 1;
    if (argc > 1 && !strcmp(argv[1], "--bench")) {
        uint64_t n = argc > 2 ? strtoull(argv[2], NULL, 10) : 100000;
        unsigned g = argc > 3 ? (unsigned)atoi(argv[3]) : 12;
        if (!n || (g != 12 && g != 14 && g != 16)) return 2;
        benchmark(n, g);
    } else {
        boundaries(); atomics(); code_changes(); invalidations(); address_spaces(); joined_pages(); code_in_joined_pages(); user_granule_switch();
        remote_invalidation(); small_table(); remote_small(); granule_switches(); kept_granules(); revoke_writes(); code_store_small();
        code_through_small_alias(DATA_VA + 0x10000); code_through_small_alias(0x203fff000ull); stashed_asid_drop();
        benchmark(10000, 12); benchmark(10000, 14); benchmark(10000, 16);
    }
    printf("4K cache: %d checks, %d failures (software page %u)\n", checks, failures, 1u << HV_TLB_PAGE_BITS);
    hv_machine_free(&m);
    free(pt); free(ram);
    alarm(0);
    return failures != 0;
}
