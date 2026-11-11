/* Native vs JIT timing for typical code shapes (see bench_programs.S). */
#include "harness.h"

#include <mach/mach_time.h>

#include "translate.h"
#include <stdlib.h>

DECLARE_PROGRAM(bench_calls);
DECLARE_PROGRAM(bench_mem);
DECLARE_PROGRAM(bench_dispatch);
DECLARE_PROGRAM(bench_loop);
extern const char bench_programs_begin[], bench_programs_finish[];

static const TestProgram programs[] = {
    TEST_PROGRAM(bench_calls), TEST_PROGRAM(bench_mem), TEST_PROGRAM(bench_dispatch), TEST_PROGRAM(bench_loop),
};

/* EL0, so the indirect-branch cache takes these low (user) addresses. */
static void at_el0(HvCpu *cpu) {
    cpu->el = 0;
    cpu->spsel = 0;
}

static double now_ms(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    static HvMachine m;
    if (hv_machine_init(&m, 4 << 20) != 0) return 1;
    m.chain = 1;
    hv_machine_add_ram(&m, (uint64_t)(uintptr_t)bench_programs_begin, bench_programs_finish - bench_programs_begin,
                       (void *)bench_programs_begin, 1);
    uint8_t *stack = aligned_alloc(16384, 65536);
    hv_machine_add_ram(&m, (uint64_t)(uintptr_t)stack, 65536, stack, 0);
    uint64_t sp = (uint64_t)(uintptr_t)stack + 32768;
    int failed = 0;
    for (size_t i = 0; i < sizeof programs / sizeof programs[0]; i++) {
        const TestProgram *p = &programs[i];
        static HvCpu init, native, jit;
        memset(&init, 0, sizeof init);
        seed_cpu(&init, i + 1, sp);
        double tn = 1e9, tj = 1e9;
        uint64_t entries = 0, slow = 0;
        for (int k = 0; k < 5; k++) {
            double t0 = now_ms();
            native = init;
            hv_native_run(&native, p->native);
            double t1 = now_ms();
            uint64_t b0 = HV_STAT_TOTAL(&m, blocks), s0 = HV_STAT_TOTAL(&m, helper[HV_OP_MEM]);
            if (run_jit(&m, p, &init, &jit, at_el0)) return 1;
            entries = HV_STAT_TOTAL(&m, blocks) - b0;
            slow = HV_STAT_TOTAL(&m, helper[HV_OP_MEM]) - s0;
            double t2 = now_ms();
            if (t1 - t0 < tn) tn = t1 - t0;
            if (k && t2 - t1 < tj) tj = t2 - t1; /* the first JIT run includes translation */
        }
        int bad = i != 1 && (native.x[0] != jit.x[0] || native.x[5] != jit.x[5]); /* bench_mem's results depend on the buffer's earlier contents */
        printf("%-15s native %7.3f ms  jit %7.3f ms  %5.2fx  (%llu dispatcher entries, %llu slow loads/stores)%s\n", p->name, tn, tj, tj / tn,
               (unsigned long long)entries, (unsigned long long)slow, bad ? "  WRONG RESULT" : "");
        failed += bad;
    }
    /* HV_DUMP=file: every translated block (guest VA, guest words, host words) for disassembly. */
    if (getenv("HV_DUMP")) {
        FILE *f = fopen(getenv("HV_DUMP"), "wb");
        for (int i = 0; f && i < m.nblocks; i++) {
            HvBlock *b = &m.blocks[i];
            uint32_t *end = i + 1 < m.nblocks ? m.blocks[i + 1].code : m.jit.base + m.jit.used;
            uint64_t hdr[3] = {b->va, b->ninsns, (uint64_t)(end - b->code)};
            fwrite(hdr, sizeof hdr, 1, f);
            fwrite((const void *)(uintptr_t)b->va, 4, b->ninsns, f);
            fwrite(b->code, 4, (size_t)(end - b->code), f);
        }
        if (f) fclose(f);
    }
    return failed != 0;
}
