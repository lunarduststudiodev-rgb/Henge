#include "harness.h"

#include <stdlib.h>

#include "mmu.h"
#include "translate.h"

DECLARE_PROGRAM(single);
DECLARE_PROGRAM(stolen_mem);
DECLARE_PROGRAM(pairs);
DECLARE_PROGRAM(simd_mem);
DECLARE_PROGRAM(exclusive);
DECLARE_PROGRAM(atomics);
DECLARE_PROGRAM(literal);
DECLARE_PROGRAM(loops);
DECLARE_PROGRAM(epilogue);
DECLARE_PROGRAM(dczva);
DECLARE_PROGRAM(pageend);
DECLARE_PROGRAM(pageend_light);
DECLARE_PROGRAM(groups);
DECLARE_PROGRAM(mmio);
extern const char mem_programs_begin[], mem_programs_finish[];

static const TestProgram programs[] = {
    TEST_PROGRAM(single), TEST_PROGRAM(stolen_mem), TEST_PROGRAM(pairs),   TEST_PROGRAM(simd_mem),
    TEST_PROGRAM(exclusive), TEST_PROGRAM(atomics),  TEST_PROGRAM(literal), TEST_PROGRAM(loops), TEST_PROGRAM(epilogue),
    TEST_PROGRAM(dczva), TEST_PROGRAM(pageend), TEST_PROGRAM(pageend_light), TEST_PROGRAM(groups),
};

#define DATA_SIZE 131072
#define STACK_SIZE 65536
#define PT_SIZE (256 * 1024)
#define DATA_PA 0x40000000ull /* where the data buffer sits in guest PA when the MMU is on */
#define PT_PA 0x80000000ull

static uint8_t *data, *stack, *pt;
static uint64_t data_seed;
static uint64_t ttbr0, pt_next;

static void reset_data(void) {
    uint64_t s = data_seed * 0x2545f4914f6cdd1dull + 7;
    for (size_t i = 0; i < DATA_SIZE; i += 8) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        memcpy(data + i, &s, 8);
    }
}

/* 16 KB granule, 48-bit VA: four levels, level 0 indexed by bit 47. */
static uint64_t *pt_host(uint64_t pa) {
    return (uint64_t *)(pt + (pa - PT_PA));
}

static uint64_t pt_alloc(void) {
    uint64_t pa = PT_PA + pt_next;
    pt_next += 16384;
    memset(pt_host(pa), 0, 16384);
    return pa;
}

static void map_page(uint64_t va, uint64_t pa) {
    uint64_t table = ttbr0;
    for (int level = 0; level < 3; level++) {
        int shift = 14 + 11 * (3 - level);
        uint64_t *e = &pt_host(table)[(va >> shift) & 0x7ff];
        if (!(*e & 1)) *e = pt_alloc() | 3;
        table = *e & 0x0000ffffffffc000ull;
    }
    /* page, AttrIndx 0 (normal), AP=00 (EL1 RW), inner shareable, AF */
    pt_host(table)[(va >> 14) & 0x7ff] = pa | 3 | 3u << 8 | 1u << 10;
}

static void map_range(uint64_t va, uint64_t pa, uint64_t size) {
    for (uint64_t off = 0; off < size; off += 16384) map_page(va + off, pa + off);
}

static void build_page_tables(void) {
    pt_next = 0;
    ttbr0 = pt_alloc();
    uint64_t code = (uint64_t)(uintptr_t)mem_programs_begin;
    map_range(code, code, mem_programs_finish - mem_programs_begin);
    map_range((uint64_t)(uintptr_t)stack, (uint64_t)(uintptr_t)stack, STACK_SIZE);
    map_range((uint64_t)(uintptr_t)data, DATA_PA, DATA_SIZE);
}

static void mmu_on(HvCpu *cpu) {
    cpu->mair_el1 = 0xff;                                       /* attr 0: normal write-back */
    cpu->tcr_el1 = 16 | 2u << 14 | 1u << 23 | 5ull << 32;      /* T0SZ 16, TG0 16K, EPD1, IPS 48 bits */
    cpu->ttbr0_el1 = ttbr0;
    cpu->sctlr_el1 |= 1 | 1u << 2 | 1u << 12;                   /* M, C, I */
    hv_tlb_flush(cpu);
}

static void tlb_empty(HvCpu *cpu) {
    hv_tlb_flush(cpu);
}

/* A device that logs every access and answers reads with offset * 0x01010101 + size. */
typedef struct { uint64_t off, val; unsigned size, write; } DevAccess;
static DevAccess dev_log[32];
static int dev_n;

static uint64_t dev_read(void *opaque, uint64_t off, unsigned size) {
    (void)opaque;
    uint64_t v = off * 0x0101010101010101ull + size;
    if (size < 8) v &= (1ull << (size * 8)) - 1;
    dev_log[dev_n++] = (DevAccess){off, v, size, 0};
    return v;
}

static void dev_write(void *opaque, uint64_t off, uint64_t val, unsigned size) {
    (void)opaque;
    dev_log[dev_n++] = (DevAccess){off, val, size, 1};
}

static int test_mmio(HvMachine *m) {
    static const TestProgram p = TEST_PROGRAM(mmio);
    static HvCpu init, jit;
    const uint64_t base = 0x20000000;
    memset(&init, 0, sizeof init);
    seed_cpu(&init, 77, (uint64_t)(uintptr_t)stack + STACK_SIZE / 2);
    init.x[0] = base;
    dev_n = 0;
    if (run_jit(m, &p, &init, &jit, NULL)) return 1;
    const struct { uint64_t off; unsigned size, write; } want[] = {
        {0, 4, 1}, {4, 4, 0}, {3, 1, 1}, {6, 2, 0}, {16, 8, 1}, {24, 8, 1},
        {8, 8, 0}, {32, 8, 0}, {64, 8, 1}, {72, 8, 0},
    };
    int bad = dev_n != (int)(sizeof want / sizeof want[0]);
    for (int i = 0; i < dev_n && !bad; i++)
        if (dev_log[i].off != want[i].off || dev_log[i].size != want[i].size || dev_log[i].write != want[i].write) bad = 1;
    if (!bad) {
        bad |= dev_log[0].val != (uint32_t)init.x[1] || dev_log[2].val != (uint8_t)init.x[3];
        bad |= dev_log[4].val != init.x[5] || dev_log[5].val != init.x[6] || dev_log[8].val != base + 64;
        bad |= jit.x[2] != 4 * 0x01010101ull + 4 || jit.x[4] != 0x0606 + 2 || jit.x[7] != 8 * 0x0101010101010101ull + 8;
        /* LDP of two words from MMIO is one 8-byte read here */
        uint64_t pairv = 32 * 0x0101010101010101ull + 8;
        bad |= jit.x[8] != (uint32_t)pairv || jit.x[9] != pairv >> 32;
        bad |= jit.x[28] != 72 * 0x0101010101010101ull + 8;
    }
    if (bad)
        for (int i = 0; i < dev_n; i++)
            printf("  mmio: %s off %llu size %u val %llx\n", dev_log[i].write ? "write" : "read",
                   (unsigned long long)dev_log[i].off, dev_log[i].size, (unsigned long long)dev_log[i].val);
    printf("%-11s %-18s: %s\n", "mmio", "fast path", bad ? "FAIL" : "ok");
    return bad;
}

int main(void) {
    setenv("HV_GROUPS", "1", 0); /* grouped accesses are off by default: test them here */
    setvbuf(stdout, NULL, _IONBF, 0);
    static HvMachine m;
    if (hv_machine_init(&m, 8 << 20) != 0) return 1;
    data = aligned_alloc(16384, DATA_SIZE);
    stack = aligned_alloc(16384, STACK_SIZE);
    pt = aligned_alloc(16384, PT_SIZE);
    uint64_t code = (uint64_t)(uintptr_t)mem_programs_begin;
    hv_machine_add_ram(&m, code, mem_programs_finish - mem_programs_begin, (void *)mem_programs_begin, 1);
    hv_machine_add_ram(&m, (uint64_t)(uintptr_t)stack, STACK_SIZE, stack, 0);
    hv_machine_add_ram(&m, (uint64_t)(uintptr_t)data, DATA_SIZE, data, 0);
    hv_machine_add_ram(&m, DATA_PA, DATA_SIZE, data, 0); /* the same memory, at another PA */
    hv_machine_add_ram(&m, PT_PA, PT_SIZE, pt, 0);
    build_page_tables();
    m.trace = getenv("HV_TRACE") != NULL;

    static const char *mode_names[] = {"C slow path", "fast path", "fast path, MMU on"};
    size_t count = sizeof programs / sizeof programs[0];
    int failed = 0;
    static uint8_t native_data[DATA_SIZE];
    for (int mode = 0; mode < 3; mode++) {
        m.slow_mem = mode == 0;
        hv_flush_blocks(&m);
        for (size_t i = 0; i < count; i++) {
            const TestProgram *p = &programs[i];
            static HvCpu init, native, jit;
            memset(&init, 0, sizeof init);
            seed_cpu(&init, i + 11, (uint64_t)(uintptr_t)stack + STACK_SIZE / 2);
            init.x[0] = (uint64_t)(uintptr_t)data;
            data_seed = i + 3;
            run_native(p, &init, &native, reset_data);
            memcpy(native_data, data, DATA_SIZE);
            reset_data();
            uint64_t slow0 = HV_STAT_TOTAL(&m, slow_mem);
            /* pageend_light starts with an empty TLB and takes no helper call (counted with statistics on, never printed) */
            int light = !strcmp(p->name, "pageend_light");
            HvStats st0;
            hv_stats_sum(&m, &st0);
            m.stats_interval = light ? 1 << 30 : 0;
            int bad = run_jit(&m, p, &init, &jit, mode == 2 ? mmu_on : light ? tlb_empty : NULL);
            m.stats_interval = 0;
            if (!bad && light && mode) {
                uint64_t calls = HV_STAT_TOTAL(&m, helper[HV_OP_MEM]) - st0.helper[HV_OP_MEM] + HV_STAT_TOTAL(&m, helper[HV_OP_SYS]) - st0.helper[HV_OP_SYS];
                uint64_t light_done = HV_STAT_TOTAL(&m, page_end_light) - st0.page_end_light;
                if (calls || !light_done) {
                    printf("  %s: %llu helper calls (expected none), %llu page-end accesses done light\n",
                           p->name, (unsigned long long)calls, (unsigned long long)light_done);
                    bad = 1;
                }
            }
            if (!bad) bad = compare_cpus(p->name, &native, &jit);
            for (size_t off = 0; off < DATA_SIZE && !bad; off += 8)
                if (memcmp(native_data + off, data + off, 8) != 0) {
                    uint64_t a, b;
                    memcpy(&a, native_data + off, 8);
                    memcpy(&b, data + off, 8);
                    printf("  %s: data+%zu native %016llx jit %016llx\n", p->name, off, (unsigned long long)a,
                           (unsigned long long)b);
                    bad = 1;
                }
            printf("%-11s %-18s: %-4s (%llu slow-path accesses)\n", p->name, mode_names[mode], bad ? "FAIL" : "ok",
                   (unsigned long long)(HV_STAT_TOTAL(&m, slow_mem) - slow0));
            failed += bad != 0;
        }
    }
    hv_machine_add_mmio(&m, 0x20000000, 0x1000, NULL, dev_read, dev_write, "test device");
    m.slow_mem = 0;
    failed += test_mmio(&m);
    printf("%zu programs x 3 modes + mmio, %d failures, %llu chained exits\n", count, failed,
           (unsigned long long)HV_STAT_TOTAL(&m, chained));
    return failed != 0;
}
