#include "harness.h"

#include <mach/mach_time.h>
#include <stdlib.h>
#include <unistd.h>

DECLARE_PROGRAM(p_svc);
DECLARE_PROGRAM(p_brk_undef);
DECLARE_PROGRAM(p_dabt);
DECLARE_PROGRAM(p_align);
DECLARE_PROGRAM(p_el0);
DECLARE_PROGRAM(p_dze);
DECLARE_PROGRAM(p_sysregs);
DECLARE_PROGRAM(p_irq);
DECLARE_PROGRAM(p_fptrap);
DECLARE_PROGRAM(p_psci);
DECLARE_PROGRAM(p_smc);
DECLARE_PROGRAM(p_smc_nomaint);
extern const char exc_programs_begin[], exc_programs_finish[];

#define ACK_PA 0x20000000ull

static HvMachine m;
static uint8_t *stack;
static int failures, checks;

static void ack_write(void *opaque, uint64_t off, uint64_t val, unsigned size) {
    (void)off, (void)val, (void)size;
    ((HvMachine *)opaque)->percpu[0].irq_line = 0;
}

static void check(const char *prog, const char *what, uint64_t got, uint64_t want) {
    checks++;
    if (got != want) {
        printf("  %s: %s = %llx, expected %llx\n", prog, what, (unsigned long long)got, (unsigned long long)want);
        failures++;
    }
}

static int run(const TestProgram *p, HvCpu *cpu, uint64_t x9) {
    static HvCpu init;
    memset(&init, 0, sizeof init);
    seed_cpu(&init, 5, (uint64_t)(uintptr_t)stack + 32768);
    init.x[9] = x9;
    init.x[19] = ACK_PA;
    int before = failures;
    if (run_jit(&m, p, &init, cpu, NULL)) failures++;
    return failures != before;
}

#define ADDR(sym) ((uint64_t)(uintptr_t)(sym))

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    hv_machine_init(&m, 4 << 20);
    stack = aligned_alloc(16384, 65536);
    hv_machine_add_ram(&m, ADDR(exc_programs_begin), exc_programs_finish - exc_programs_begin,
                       (void *)exc_programs_begin, 1);
    hv_machine_add_ram(&m, ADDR(stack), 65536, stack, 0);
    hv_machine_add_mmio(&m, ACK_PA, 0x1000, &m, NULL, ack_write, "irq ack");
    m.trace = getenv("HV_TRACE") != NULL;
    static HvCpu c;

    static const TestProgram svc = TEST_PROGRAM(p_svc);
    if (!run(&svc, &c, 0)) {
        check("svc", "ESR", c.x[1], 0x56000042);
        check("svc", "ELR", c.x[2], c.x[6]);
        check("svc", "SPSR", c.x[3], 0x800003c5);
        check("svc", "NZCV after ERET", c.x[4], 0x80000000);
        check("svc", "vector", c.x[7], 0x200);
        check("svc", "count", c.x[24], 1);
    }

    static const TestProgram bu = TEST_PROGRAM(p_brk_undef);
    if (!run(&bu, &c, 0)) {
        check("brk", "ESR", c.x[1], 0xf2000007);
        check("brk", "ELR", c.x[2], c.x[3]);
        check("udf", "ESR", c.x[4], 0x02000000);
        check("udf", "ELR", c.x[5], c.x[6]);
    }

    static const TestProgram da = TEST_PROGRAM(p_dabt);
    if (!run(&da, &c, 0)) {
        check("dabt", "load ESR", c.x[1], 0x96000010);
        check("dabt", "load FAR", c.x[2], 0x10000);
        check("dabt", "ELR", c.x[3], c.x[4]);
        check("dabt", "store ESR (WnR)", c.x[5], 0x96000050);
        check("dabt", "store FAR", c.x[6], 0x10010);
        check("dabt", "no writeback on a fault", c.x[10], 0x10000);
    }

    static const TestProgram al = TEST_PROGRAM(p_align);
    alarm(60); /* a misaligned access the light TLB refill retried for ever hung here */
    if (!run(&al, &c, ADDR(stack) + 4096)) {
        check("align", "LDAR ESR", c.x[1], 0x96000021);
        check("align", "LDAR FAR", c.x[2], ADDR(stack) + 4097);
        check("align", "LDAR ELR", c.x[4], c.x[3]);
        check("align", "LDADD ESR (WnR)", c.x[5], 0x96000061);
        check("align", "LDADD FAR", c.x[6], ADDR(stack) + 4099);
        check("align", "LDADD ELR", c.x[13], c.x[12]);
        check("align", "faults", c.x[14], 2);
    }
    alarm(0);

    static const TestProgram dze = TEST_PROGRAM(p_dze);
    if (!run(&dze, &c, ADDR(stack) + 49152 + 256)) {
        check("dze", "DC ZVA at EL0 with DZE clear: ESR", c.x[2], 0x02000000);
        check("dze", "one exception", c.x[3], 1);
        check("dze", "nothing zeroed", c.x[4], 0x55);
    }

    static const TestProgram el0 = TEST_PROGRAM(p_el0);
    uint64_t sp0 = ADDR(stack) + 8192;
    uint64_t sp1 = ADDR(stack) + 32768;
    if (!run(&el0, &c, sp0)) {
        check("el0", "SP at EL0", c.x[10], sp0);
        check("el0", "SVC vector (lower EL)", c.x[13], 0x400);
        check("el0", "MRS SCTLR_EL1 at EL0: ESR", c.x[14], 0x02000000);
        check("el0", "MRS SCTLR_EL1 at EL0: vector", c.x[15], 0x400);
        check("el0", "TPIDR_EL0 write/read", c.x[18], 0x55);
        check("el0", "still at EL0", c.el, 0);
        check("el0", "SP_EL1 banked", c.sp_el1, sp1);
        check("el0", "exceptions", c.x[24], 2);
    }

    static const TestProgram sr = TEST_PROGRAM(p_sysregs);
    static HvCpu seeded;
    seed_cpu(&seeded, 5, 0);
    if (!run(&sr, &c, 0)) {
        check("sysregs", "TPIDR_EL1", c.x[2], seeded.x[1]);
        check("sysregs", "CurrentEL", c.x[3], 4);
        check("sysregs", "DAIF after set", c.x[4], 0x3c0);
        check("sysregs", "DAIF after clr", c.x[5], 0x300);
        check("sysregs", "NZCV", c.x[7], 0x60000000);
        check("sysregs", "CNTVCT advances", c.x[9] > c.x[8], 1);
        check("sysregs", "CNTFRQ", c.x[10], 24000000);
        check("sysregs", "16K granule advertised", (c.x[11] >> 20) & 0xf, 1);
        check("sysregs", "SP_EL0 via SPSel", c.x[12], 0);
        check("sysregs", "SP_EL1 via SPSel", c.x[13], sp1);
        check("sysregs", "MIDR implementer", c.x[14] >> 24, 0x61);
        check("sysregs", "FPCR", c.x[17], 1u << 24);
        check("sysregs", "VBAR", c.x[18], c.vbar_el1);
        check("sysregs", "CNTV_CVAL", c.x[21], 0x123);
        check("sysregs", "CNTV_CTL ISTATUS", c.x[23], 5);
    }
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    check("host", "mach_absolute_time is 24 MHz (timebase 125/3)", (uint64_t)tb.numer * 1000 + tb.denom, 125003);

    static const TestProgram irq = TEST_PROGRAM(p_irq);
    m.percpu[0].irq_line = 1;
    if (!run(&irq, &c, 0)) {
        check("irq", "masked IRQ not taken across WFI", c.x[1], 0);
        check("irq", "taken after DAIFClr", c.x[2], 1);
        check("irq", "vector", c.x[3], 0x280);
        check("irq", "ELR = next instruction", c.x[4], c.x[5]);
        check("irq", "line acknowledged", m.percpu[0].irq_line, 0);
    }
    m.percpu[0].irq_line = 0;

    static const TestProgram fp = TEST_PROGRAM(p_fptrap);
    if (!run(&fp, &c, 0)) {
        check("fptrap", "ESR", c.x[1], 0x1fe00000);
        check("fptrap", "one trap", c.x[2], 1);
        check("fptrap", "MSR FPCR with FP off traps", c.x[5], 2);
        check("fptrap", "MRS FPSR with FP off traps", c.x[7], 3);
        check("fptrap", "its ESR", c.x[8], 0x1fe00000);
        check("fptrap", "MRS FPCR with FP off traps", c.x[10], 4);
        check("fptrap", "FPCR written after the trap", c.x[9] & (1u << 24), 1u << 24);
        double d1, d2, d0;
        memcpy(&d1, &seeded.v[1], 8);
        memcpy(&d2, &seeded.v[2], 8);
        d0 = d1 + d2;
        uint64_t want;
        memcpy(&want, &d0, 8);
        check("fptrap", "FADD retried", c.x[3], want);
    }

    static const TestProgram ps = TEST_PROGRAM(p_psci);
    if (!run(&ps, &c, 0)) {
        check("psci", "PSCI_VERSION", c.x[3], 0x10000);
        check("psci", "PSCI_FEATURES(CPU_ON)", c.x[2], 0);
    }

    /* Self-modifying code runs from a writable copy. */
    const TestProgram *smc_progs[2];
    static const TestProgram smc1 = TEST_PROGRAM(p_smc), smc2 = TEST_PROGRAM(p_smc_nomaint);
    smc_progs[0] = &smc1;
    smc_progs[1] = &smc2;
    uint8_t *code = aligned_alloc(16384, 16384);
    hv_machine_add_ram(&m, ADDR(code), 16384, code, 0);
    for (int i = 0; i < 2; i++) {
        const TestProgram *p = smc_progs[i];
        size_t len = (const char *)p->end - (const char *)p->start + 4;
        memcpy(code, p->start, len);
        TestProgram copy = {p->name, NULL, code, code + ((const char *)p->end - (const char *)p->start)};
        if (!run(&copy, &c, 0)) check(p->name, "1 then 2 after patching", c.x[1], 3);
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
