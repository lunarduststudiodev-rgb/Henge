// A TLBI VMALLE1 right after a process switch (a TTBR0 write) must drop every
// translation, as on a real CPU, with the knobs as they are by default.
//
// HV_SWITCH_FLUSH=2 (the default from e540907) left most of these out, checking
// only 1 in 32 first. On the iPad (2026-11-09) its own check found translations
// macOS had removed still held when macOS asked for the flush: a kernel page and
// pages of switched-out processes, all with no mapping any more. Left in place,
// the Mac goes on reading and writing pages it has freed. Each case here removes
// a mapping, relies on the whole-TLB flush to drop it (which the architecture
// allows: changing a page table and then flushing all), and checks that the page
// faults afterwards. Several rounds, so the unchecked skips are covered too.
// The process cases run twice: for a native process (16 KB pages) and for an
// Intel one under Rosetta, which macOS maps with 4 KB pages (TCR_EL1.TG0 4 KB,
// switched with the process): its scattered pages are cached one 4 KB page per
// entry (the 4 KB table in a 16 KB build, every table in a 4 KB one).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "machine.h"
#include "mmu.h"
#include "sysreg.h"

#define PT_PA 0x80000000ull
#define PT_SIZE (1u << 22)
#define DATA_PA (PT_PA + (2u << 20)) /* pages the mappings point at: ordinary RAM, so they are cached */

static uint8_t *pt;
static uint64_t pt_next;
static int failures, checks, fault_checks, fault_failures;

static uint64_t *host(uint64_t pa) { return (uint64_t *)(pt + (pa - PT_PA)); }

static uint64_t alloc_table(void) {
    uint64_t pa = PT_PA + pt_next;
    pt_next += 16384;
    memset(host(pa), 0, 16384);
    return pa;
}

/* The level 3 entry for va (granule 1 << g: 12 or 14), creating the tables above it. */
static uint64_t *pte_g(uint64_t root, int inputsize, uint64_t va, int g) {
    const int stride = g - 3;
    int levels = (inputsize - g + stride - 1) / stride;
    uint64_t table = root;
    for (int level = 4 - levels;; level++) {
        int shift = g + stride * (3 - level);
        int idx_bits = level == 4 - levels ? inputsize - shift : stride;
        uint64_t *e = &host(table)[(va >> shift) & ((1ull << idx_bits) - 1)];
        if (level == 3) return e;
        if (!(*e & 1)) *e = alloc_table() | 3;
        table = *e & 0x0000ffffffffffffull & ~((1ull << g) - 1);
    }
}
static uint64_t *pte(uint64_t root, int inputsize, uint64_t va) { return pte_g(root, inputsize, va, 14); }

#define LEAF(pa) ((pa) | 3ull | 1ull << 10 | 1ull << 6)   /* page, AF, EL0+EL1 read/write */
#define ULEAF(pa) (LEAF(pa) | 1ull << 11)                  /* and not global, as a process's pages are */

static void msr_ttbr0(HvCpu *cpu, uint64_t v) {
    cpu->x[1] = v;
    hv_sysreg_msr(cpu, 0xd5182001u, 0); /* msr ttbr0_el1, x1 */
}

static void msr_tcr(HvCpu *cpu, uint64_t v) {
    cpu->x[1] = v;
    hv_sysreg_msr(cpu, 0xd5182041u, 0); /* msr tcr_el1, x1 */
}

static void tlbi_vmalle1(HvCpu *cpu) { hv_sysreg_sys(cpu, 0xd508871fu, 0); /* tlbi vmalle1 */ }

static void expect_fault(const char *what, int round, HvCpu *cpu, uint64_t va, int el) {
    uint64_t pa = 0;
    uint32_t fsc = hv_mmu_access(cpu, va, HV_ACC_READ, el, &pa);
    checks++, fault_checks++;
    if (fsc != HV_FSC_TRANSLATION(3)) {
        printf("  FAIL %s, round %d: va %llx still reaches pa %llx after TLBI VMALLE1 (fsc %02x)\n", what, round,
               (unsigned long long)va, (unsigned long long)pa, fsc);
        failures++, fault_failures++;
    }
}

static void expect_hit(const char *what, HvCpu *cpu, uint64_t va, int el, uint64_t want) {
    uint64_t pa = 0;
    uint32_t fsc = hv_mmu_access(cpu, va, HV_ACC_READ, el, &pa);
    checks++;
    if (fsc || pa != want) {
        printf("  FAIL %s: va %llx -> pa %llx fsc %02x, expected pa %llx\n", what, (unsigned long long)va,
               (unsigned long long)pa, fsc, (unsigned long long)want);
        failures++;
    }
}

int main(void) {
    static HvMachine m;
    static HvCpu cpu;
    if (hv_machine_init(&m, 1 << 20)) return 1;
    pt = aligned_alloc(16384, PT_SIZE);
    memset(pt, 0, PT_SIZE);
    hv_machine_add_ram(&m, PT_PA, PT_SIZE, pt, 0);
    hv_cpu_reset(&cpu, &m);
    printf("HV_SWITCH_FLUSH %d\n", hv_knob[HV_K_SWITCHFLUSH]);

    uint64_t r1 = alloc_table(), ra = alloc_table(), rb = alloc_table(), rc = alloc_table();
    const uint64_t ttbr_a = 0x61bull << 48 | ra, ttbr_b = 0x144ull << 48 | rb, ttbr_c = 0x2a7ull << 48 | rc;
    cpu.ttbr0_el1 = ttbr_a;
    cpu.ttbr1_el1 = r1;
    cpu.mair_el1 = 0xff;
    const uint64_t tcr16 = 16 | 2u << 14 | 25u << 16 | 1ull << 30 | 5ull << 32 | 1ull << 36; /* T0SZ 16, TG0 16K, T1SZ 25, TG1 16K, 16-bit ASIDs */
    const uint64_t tcr4 = tcr16 & ~(3ull << 14);                                             /* TG0 4K: an Intel process */
    cpu.tcr_el1 = tcr16;
    cpu.sctlr_el1 |= 1;
    cpu.el = 1;

    const uint64_t kva = 0xffffffc012340000ull; /* a kernel page (the iPad's was fffffe810a384000) */
    const uint64_t uva = 0x118984000ull;        /* a process's page (one of the iPad's) */
    const uint64_t uva2 = 0x118988000ull;
    const uint64_t xva = 0x10000d000ull, xva2 = 0x100016000ull; /* an Intel process's pages, their neighbours unmapped */

    for (int round = 0; round < 64; round++) {
        uint64_t pa = DATA_PA + (uint64_t)(round & 7) * 16384;

        /* The kernel removes one of its pages (no TLBI of its own), switches process and flushes this CPU's TLB. */
        *pte(r1, 39, kva) = LEAF(pa);
        expect_hit("kernel page mapped", &cpu, kva, 1, pa);
        *pte(r1, 39, kva) = 0;
        msr_ttbr0(&cpu, cpu.ttbr0_el1 == ttbr_a ? ttbr_b : ttbr_a);
        tlbi_vmalle1(&cpu);
        expect_fault("kernel page removed before a switch and a flush", round, &cpu, kva, 1);

        /* A switched-out process loses a page; the switch back to it is followed by the flush. */
        msr_ttbr0(&cpu, ttbr_a);
        *pte(ra, 48, uva) = ULEAF(pa);
        expect_hit("process page mapped", &cpu, uva, 0, pa);
        msr_ttbr0(&cpu, ttbr_b); /* A's entries are put aside */
        *pte(ra, 48, uva) = 0;
        msr_ttbr0(&cpu, ttbr_a); /* and brought back */
        tlbi_vmalle1(&cpu);
        expect_fault("process page removed while it was switched out", round, &cpu, uva, 0);

        /* The same, with the flush done while the other process runs: it must drop the entries put aside too.
           (A third flush per round also moves the 1-in-32 checked one of HV_SWITCH_FLUSH=2 across all three cases.) */
        *pte(ra, 48, uva2) = ULEAF(pa);
        expect_hit("process page mapped", &cpu, uva2, 0, pa);
        msr_ttbr0(&cpu, ttbr_b);
        *pte(ra, 48, uva2) = 0;
        tlbi_vmalle1(&cpu);
        msr_ttbr0(&cpu, ttbr_a);
        expect_fault("process page removed and flushed while it was switched out", round, &cpu, uva2, 0);

        /* The same for an Intel process (4 KB pages: the switch to it and away also changes TCR_EL1.TG0). */
        uint64_t xpa = pa + (uint64_t)(round & 3) * 4096;
        msr_tcr(&cpu, tcr4), msr_ttbr0(&cpu, ttbr_c);
        *pte_g(rc, 48, xva, 12) = ULEAF(xpa);
        expect_hit("4 KB process page mapped", &cpu, xva, 0, xpa);
        msr_tcr(&cpu, tcr16), msr_ttbr0(&cpu, ttbr_a); /* C's entries are put aside */
        *pte_g(rc, 48, xva, 12) = 0;
        msr_tcr(&cpu, tcr4), msr_ttbr0(&cpu, ttbr_c); /* and brought back */
        tlbi_vmalle1(&cpu);
        expect_fault("4 KB process page removed while it was switched out", round, &cpu, xva, 0);

        *pte_g(rc, 48, xva2, 12) = ULEAF(xpa);
        expect_hit("4 KB process page mapped", &cpu, xva2, 0, xpa);
        msr_tcr(&cpu, tcr16), msr_ttbr0(&cpu, ttbr_a);
        *pte_g(rc, 48, xva2, 12) = 0;
        tlbi_vmalle1(&cpu);
        msr_tcr(&cpu, tcr4), msr_ttbr0(&cpu, ttbr_c);
        expect_fault("4 KB process page removed and flushed while it was switched out", round, &cpu, xva2, 0);

        /* The kernel removes a page while the Intel process runs, switches to a native one and flushes. */
        *pte(r1, 39, kva) = LEAF(pa);
        expect_hit("kernel page mapped", &cpu, kva, 1, pa);
        *pte(r1, 39, kva) = 0;
        msr_tcr(&cpu, tcr16), msr_ttbr0(&cpu, ttbr_a);
        tlbi_vmalle1(&cpu);
        expect_fault("kernel page removed before a switch from a 4 KB process and a flush", round, &cpu, kva, 1);
    }

    printf("%d checks, %d failures (%d of %d pages still reachable after the flush)\n", checks, failures, fault_failures,
           fault_checks);
    return failures != 0;
}
