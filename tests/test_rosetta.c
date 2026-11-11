#include "harness.h"
#include "rosetta.h"
#include "exception.h"
#include "translate.h"
#include <stdlib.h>
#include <time.h>

DECLARE_PROGRAM(ap_add);
DECLARE_PROGRAM(ap_sub);
DECLARE_PROGRAM(ap_and);
DECLARE_PROGRAM(ap_carry);
DECLARE_PROGRAM(ap_save);
DECLARE_PROGRAM(ap_wide);
DECLARE_PROGRAM(ap_fpcr);
DECLARE_PROGRAM(ap_fpcr_read);
extern const char rosetta_programs_begin[], rosetta_programs_finish[];
static HvMachine m;
static HvCpu c;
static int failures;
#define ADDR(p) ((uint64_t)(uintptr_t)(p))

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void check(const char *label, uint64_t got, uint64_t want) {
    if (got != want && failures++ < 8)
        printf("FAIL %s: %llx expected %llx\n", label, (unsigned long long)got, (unsigned long long)want);
}

static void reset(void *stack) {
    hv_cpu_reset(&c, &m);
    c.el = 0;
    c.spsel = 0;
    c.sp = ADDR(stack) + 16384;
    c.actlr_el1 = 16;
}

int main(int argc, char **argv) {
    (void)argv;
    setenv("HV_ROSETTA", argc > 1 ? "2" : "1", 1);
    if (!hv_rosetta_host_supported()) { puts("SKIP: host FEAT_AFP unavailable"); return 77; }
    if (hv_machine_init(&m, 16 << 20)) return 1;
    uint32_t *out = aligned_alloc(16384, 256 * 256 * 4);
    void *stack = aligned_alloc(16384, 16384);
    if (!out || !stack) return 1;
    if (hv_machine_add_ram(&m, ADDR(rosetta_programs_begin), rosetta_programs_finish - rosetta_programs_begin,
                          (void *)rosetta_programs_begin, 1) ||
        hv_machine_add_ram(&m, ADDR(out), 256 * 256 * 4, out, 0) ||
        hv_machine_add_ram(&m, ADDR(stack), 16384, stack, 0)) return 1;
    const char *start[] = {ap_add, ap_sub, ap_and}, *end[] = {ap_add_end, ap_sub_end, ap_and_end};
    const char *names[] = {"ADDS", "SUBS", "ANDS"};
    double execution = 0;
    for (unsigned kind = 0; kind < 3; kind++) {
        reset(stack);
        c.x[3] = ADDR(out);
        c.pc = ADDR(start[kind]);
        m.stop_pc = ADDR(end[kind]);
        double begin = now();
        uint32_t reason = hv_run(&c);
        execution += now() - begin;
        check("guest exit", reason, HV_EXIT_HALT);
        for (unsigned a = 0; a < 256; a++) for (unsigned b = 0; b < 256; b++) {
            uint32_t result = kind == 0 ? a + b : kind == 1 ? a - b : a & b;
            unsigned count = 0;
            for (unsigned bit = 0; bit < 8; bit++) count += (result >> bit) & 1;
            unsigned af = kind == 0 ? (a & 15) + (b & 15) > 15 : kind == 1 ? (a & 15) < (b & 15) : 0;
            uint32_t want = (result & 0x80000000) | (result == 0) << 30 |
                            (kind == 1 && a >= b) << 29 | af << 27 | !(count & 1) << 26;
            check(names[kind], out[a * 256 + b], want);
        }
    }
    reset(stack);
    c.x[0] = 0xfc000000;
    c.pc = ADDR(ap_save); m.stop_pc = ADDR(ap_save_end);
    check("NZCV save exit", hv_run(&c), HV_EXIT_HALT);
    check("NZCV AP round trip", c.x[1], 0xfc000000);
    reset(stack);
    c.x[0] = UINT32_MAX; c.x[1] = 0; c.x[2] = 1ull << 29;
    c.pc = ADDR(ap_carry); m.stop_pc = ADDR(ap_carry_end);
    check("carry exit", hv_run(&c), HV_EXIT_HALT);
    check("ADCS result", c.x[18], 0);
    check("ADCS flags", c.x[3], 0x6c000000);
    check("CFINV preserves P/A", c.x[4], 0x4c000000);
    check("SBCS result", c.x[28], UINT32_MAX - 1);
    check("SBCS flags", c.x[5], 0xa0000000);
    reset(stack);
    c.x[0] = 0x7ffffffffffffff0; c.x[1] = 1; c.x[2] = 255;
    c.pc = ADDR(ap_wide); m.stop_pc = ADDR(ap_wide_end);
    check("wide exit", hv_run(&c), HV_EXIT_HALT);
    check("shifted ADDS result", c.x[18], 0x8000000000000000ull);
    check("shifted ADDS flags", c.x[3], 0x94000000);
    check("immediate SUBS result", c.x[28], 0x7fffffffffffffffull);
    check("immediate SUBS flags", c.x[4], 0x3c000000);
    check("logical immediate result", c.x[6], 255);
    check("logical immediate flags", c.x[5], 0x04000000);
    check("extended SP result", c.x[7], c.sp - 4);
    check("extended SP flags", c.x[8], 0x24000000);
    // FPCR in Rosetta mode: the guest's value is read inline, without the helper, also into a register the
    // translator keeps in memory (x28). AFP on (ACTLR bit 5) makes the host's FPCR differ (it adds AH), so a
    // read of the host's register would be caught. The C reference (mode 2) reads through the helper.
    reset(stack);
    c.cpacr_el1 = 3u << 20; /* FP on at EL0 */
    c.actlr_el1 = 0x30;     /* AP flags and AFP */
    c.x[0] = 0x1c00001;     /* FZ, RMode 3 and FIZ */
    c.x[1] = 0x55;
    c.pc = ADDR(ap_fpcr); m.stop_pc = ADDR(ap_fpcr_end);
    m.stats_interval = 1;
    uint64_t mrs = HV_STAT_TOTAL(&m, helper[HV_OP_MRS]), msr = HV_STAT_TOTAL(&m, helper[HV_OP_MSR]);
    check("FPCR exit", hv_run(&c), HV_EXIT_HALT);
    check("host FPCR differs (AH)", c.fpcr_native, 0x1c00003);
    check("FPCR read back", c.x[1], 0x1c00001);
    check("FPCR read back (x28)", c.x[28], 0x1c00001);
    check("FPCR written through the helper", HV_STAT_TOTAL(&m, helper[HV_OP_MSR]) - msr, 1);
    check("FPCR reads inline (helper in the reference)", HV_STAT_TOTAL(&m, helper[HV_OP_MRS]) - mrs, argc > 1 ? 3 : 0);
    m.stats_interval = 0;
    // With FP off at EL0 the read traps (EC 7) through the helper, at the MRS, and leaves x1 alone.
    static uint32_t vectors[2048] __attribute__((aligned(2048)));
    if (hv_machine_add_ram(&m, ADDR(vectors), sizeof vectors, vectors, 1)) return 1;
    reset(stack);
    c.cpacr_el1 = 0;
    c.vbar_el1 = ADDR(vectors);
    c.x[1] = 0x55;
    c.pc = ADDR(ap_fpcr_read); m.stop_pc = ADDR(vectors) + 0x400; /* synchronous, from EL0 */
    check("FPCR trap exit", hv_run(&c), HV_EXIT_HALT);
    check("FPCR trap class", c.esr_el1 >> 26, 7);
    check("FPCR trap address", c.elr_el1, ADDR(ap_fpcr_read));
    check("FPCR trap to EL1", c.el, 1);
    check("FPCR trap leaves x1", c.x[1], 0x55);
    reset(stack);
    c.actlr_el1 = 0;
    c.apflags = HV_APFLAGS_MASK;
    c.x[0] = 0xfc000000;
    c.pc = ADDR(ap_save); m.stop_pc = ADDR(ap_save_end);
    check("cached code with AP off", hv_run(&c), HV_EXIT_HALT);
    check("ARM thread NZCV", c.x[1], 0xf0000000);
    c.aspsr_el1 = 6; c.afpcr_el0 = 3; c.actlr_el1 = 0x70; c.fpcr = 0;
    hv_rosetta_fp_apply(&c);
    check("AFP DAZ/FTZ/NEP mapping", c.fpcr_native, 0x1000007);
    c.apflags = HV_APFLAGS_MASK;
    hv_take_exception(&c, HV_EXC_SYNC, 0, 0, 0, ADDR(ap_save));
    check("exception saves AP", c.aspsr_el1, 6);
    check("exception clears AP", c.apflags, 0);
    check("kernel FP mode", c.fpcr_native, 0);
    hv_eret(&c);
    check("ERET restores AP", c.apflags, HV_APFLAGS_MASK);
    check("ERET restores AFP", c.fpcr_native, 0x1000007);
    c.afpcr_el0 = 0; c.fpcr = 0x1000001;
    hv_rosetta_fp_apply(&c);
    check("architectural DAZ/FTZ retained", c.fpcr_native, 0x1000007);
    printf("Rosetta CPU: 196608 arithmetic cases, NZCV and AFP controls: %d failures\n", failures);
    printf("Arithmetic execution (%s): %.3f ms\n", argc > 1 ? "reference" : "inline", execution * 1000);
    hv_machine_free(&m); free(out); free(stack);
    return failures != 0;
}
