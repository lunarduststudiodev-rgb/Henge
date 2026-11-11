#include "harness.h"
#include "rosetta.h"
#include "translate.h"

#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

DECLARE_PROGRAM(tso_caps);
DECLARE_PROGRAM(tso_writer);
DECLARE_PROGRAM(tso_reader);
DECLARE_PROGRAM(tso_enter);
DECLARE_PROGRAM(tso_plain);
DECLARE_PROGRAM(tso_calls);
DECLARE_PROGRAM(tso_calls2);
extern const char tso_far[], tso_far_ret[], tso_far2[], tso_calls_back[], tso_calls2_back[], tso_vectors[];
extern const char tso_programs_begin[], tso_programs_finish[];

static HvMachine machines[2], rm;
static HvCpu caps, cpus[2];
static int failures;

#define ADDR(p) ((uint64_t)(uintptr_t)(p))

static void check(const char *what, uint64_t got, uint64_t want) {
    if (got == want) return;
    printf("FAIL %s: got %llx, expected %llx\n", what,
           (unsigned long long)got, (unsigned long long)want);
    failures++;
}

static void *run_cpu(void *arg) {
    return (void *)(intptr_t)hv_run(arg);
}

/* Writer and reader on two CPUs. el0: each starts at EL1 in tso_enter, which sets
   ACTLR_EL1.EnTSO and returns to the program at EL0, as Rosetta's kernel does. */
static void message_passing(HvMachine *m, uint8_t *data, uint8_t *stack, int el0, const char *how) {
    memset(data, 0, 16384);
    const uint64_t iterations = 100000;
    m->stop_pc = ADDR(tso_writer_end);
    m->stop_pc2 = ADDR(tso_reader_end);
    for (int i = 0; i < 2; i++) {
        hv_cpu_reset_index(&cpus[i], m, i);
        cpus[i].sp = ADDR(stack) + 16384 + i * 32768;
        uint64_t prog = i ? ADDR(tso_reader) : ADDR(tso_writer);
        cpus[i].pc = el0 ? ADDR(tso_enter) : prog;
        cpus[i].x[0] = ADDR(data);
        cpus[i].x[1] = iterations;
        cpus[i].x[9] = 2;
        cpus[i].x[10] = prog;
        cpus[i].actlr_el1 = el0 ? 0 : 2;
    }
    check("CPUs are not serialized", m->big_lock_on, 0);
    pthread_t threads[2];
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20); /* same CPU-thread stack as vm.c */
    for (int i = 0; i < 2; i++)
        if (pthread_create(&threads[i], &attr, run_cpu, &cpus[i])) exit(1);
    pthread_attr_destroy(&attr);
    for (int i = 0; i < 2; i++) {
        void *result;
        pthread_join(threads[i], &result);
        check("message-passing CPU exit", (intptr_t)result, HV_EXIT_HALT);
        check("message-passing CPU level", cpus[i].el, el0 ? 0 : cpus[i].el);
    }
    check("message passing completed", cpus[1].x[2], iterations + 1);
    check("flag observed before payload", cpus[1].x[5], 0);
    printf("%s: %llu two-CPU message-passing rounds, %d failures\n", how,
           (unsigned long long)iterations, failures);
}

/* Fences in a fresh translation of tso_plain with these block flags. */
static int fences(HvMachine *m, uint32_t flags) {
    HvBlock b = {.va = ADDR(tso_plain), .pa = ADDR(tso_plain), .flags = flags};
    uint32_t avail = (uint32_t)((16384 - (ADDR(tso_plain) & 16383)) / 4);
    if (hv_translate_block(m, &b, (const uint32_t *)(const void *)tso_plain, avail) != 0) return -1;
    int n = 0;
    for (uint32_t k = 0; k < hv_translate_last_words(); k++)
        n += b.code[k] == 0xd50339bfu || b.code[k] == 0xd5033abfu || b.code[k] == 0xd5033bbfu; /* dmb ishld, ishst, ish */
    return n;
}

/* A translation of the code at va, with (tso) or without HV_BLOCK_TSO. */
static HvBlock *block_at(HvMachine *m, uint64_t va, int tso) {
    for (uint32_t h = 0; h < HV_BLOCK_HASH; h++)
        for (uint32_t i = m->hash[h]; i; i = m->blocks[i - 1].hash_next)
            if (m->blocks[i - 1].va == va && !(m->blocks[i - 1].flags & HV_BLOCK_TSO) == !tso) return &m->blocks[i - 1];
    return NULL;
}

/* At EL1 from tso_enter with EnTSO = actlr, to the program at EL0. ACTLR already holds it, as when a thread
   returns from the kernel: no change, so no exit request, and the ERET resumes through the branch cache. The CPU
   is reset only for its first run (fresh): a reset starts its branch cache afresh (another salt), and these runs
   are to share one, as one thread's trips through the kernel do. */
static int run_el0(HvMachine *m, HvCpu *c, uint64_t prog, uint64_t end, uint64_t actlr, uint8_t *data, int fresh) {
    if (fresh) hv_cpu_reset(c, m);
    c->el = 1;
    c->spsel = 1;
    c->actlr_el1 = actlr;
    c->pc = ADDR(tso_enter);
    c->x[0] = ADDR(data);
    c->x[9] = actlr;
    c->x[10] = prog;
    m->stop_pc = end;
    int r = hv_run(c);
    check("EL0 run level", c->el, 0);
    return r;
}

static int tso_thread(HvMachine *m, int el, uint64_t actlr) {
    HvCpu c;
    memset(&c, 0, sizeof c); /* zero before its first reset (hv_cpu_reset_index): not an earlier call's CPU */
    hv_cpu_reset(&c, m);
    c.el = el;
    c.actlr_el1 = actlr;
    return hv_tso_thread(&c);
}

static int new_machine(HvMachine *m, const char *rosetta, const char *tso, uint8_t *data, uint8_t *stack) {
    setenv("HV_ROSETTA", rosetta, 1);
    if (tso) setenv("HV_SOFTWARE_TSO", tso, 1);
    else unsetenv("HV_SOFTWARE_TSO");
    if (hv_machine_init(m, 8 << 20)) exit(1);
    if (hv_machine_add_ram(m, ADDR(tso_programs_begin),
                          tso_programs_finish - tso_programs_begin, (void *)tso_programs_begin, 1) ||
        hv_machine_add_ram(m, ADDR(data), 16384, data, 0) ||
        hv_machine_add_ram(m, ADDR(stack), 65536, stack, 0)) exit(1);
    return 0;
}

/* Rosetta mode: software TSO follows ACTLR_EL1.EnTSO of the EL0 thread, unless
   HV_SOFTWARE_TSO says otherwise (0 off, 1 every block). */
static void per_thread(uint8_t *data, uint8_t *stack) {
    new_machine(&rm, "0", NULL, data, stack);
    check("no Rosetta, nothing set: software TSO off", rm.software_tso, 0);
    hv_machine_free(&rm);
    if (!hv_rosetta_host_supported()) {
        puts("per-thread TSO: SKIP, host FEAT_AFP unavailable");
        return;
    }

    new_machine(&rm, "1", "0", data, stack);
    check("Rosetta, HV_SOFTWARE_TSO=0: software TSO off", rm.software_tso, 0);
    check("HV_SOFTWARE_TSO=0: no TSO thread", tso_thread(&rm, 0, 2), 0);
    check("HV_SOFTWARE_TSO=0: no fences in a TSO block", fences(&rm, HV_BLOCK_TSO), 0);
    hv_cpu_reset(&caps, &rm);
    caps.pc = ADDR(tso_caps);
    rm.stop_pc = ADDR(tso_caps_end);
    check("HV_SOFTWARE_TSO=0 capability run", hv_run(&caps), HV_EXIT_HALT);
    check("HV_SOFTWARE_TSO=0: Rosetta's AIDR.TSO still shown", caps.x[0] >> 9 & 1, 1);
    hv_machine_free(&rm);

    new_machine(&rm, "1", "1", data, stack);
    check("Rosetta, HV_SOFTWARE_TSO=1: every block", rm.tso_every, 1);
    check("HV_SOFTWARE_TSO=1: no per-thread flag", tso_thread(&rm, 0, 2), 0);
    check("HV_SOFTWARE_TSO=1: fences without the flag", fences(&rm, 0) > 0, 1);
    hv_machine_free(&rm);

    new_machine(&rm, "1", NULL, data, stack);
    HvMachine *m = &rm;
    check("Rosetta, nothing set: software TSO on", m->software_tso, 1);
    check("Rosetta, nothing set: per thread", m->tso_every, 0);
    check("TSO thread: EL0 with EnTSO", tso_thread(m, 0, 2), 1);
    check("TSO thread: EL0 without EnTSO", tso_thread(m, 0, 0), 0);
    check("TSO thread: EL1 with EnTSO", tso_thread(m, 1, 2), 0);
    check("plain EL0 block: no fences", fences(m, 0), 0);
    check("TSO block: fences", fences(m, HV_BLOCK_TSO) > 0, 1);
    /* Branch caches: a TSO block's table is no other block's table. */
    for (uint32_t f = 0; f < 32; f++) {
        if (f & (HV_BLOCK_EL1 | HV_BLOCK_SPX | HV_BLOCK_PAN)) continue; /* TSO blocks are EL0 */
        for (uint32_t g = 0; g < 32; g++) {
            if (!(g & HV_BLOCK_EL1) && (g & (HV_BLOCK_SPX | HV_BLOCK_PAN))) continue; /* no such EL0 block */
            if (hv_ijc_table(f | HV_BLOCK_TSO) == hv_ijc_table(g)) {
                printf("FAIL branch cache: TSO flags %x share a table with flags %x\n", f, g);
                failures++;
            }
        }
    }

    /* The same code with EnTSO off, on, off and on: one translation each way, kept. The ERET into it resumes
       through the branch cache once the block is there (runs 2 and 3), and helper_ret is the code it resumed at:
       the translation for this run's EnTSO, not the other one, which that cache holds too. */
    uint64_t made[4];
    HvCpu c;
    memset(&c, 0, sizeof c); /* zero before its first reset (hv_cpu_reset_index) */
    for (int run = 0; run < 4; run++) {
        check("EL0 run exit", run_el0(m, &c, ADDR(tso_plain), ADDR(tso_plain_end), run & 1 ? 2 : 0, data, !run), HV_EXIT_HALT);
        made[run] = HV_STAT_TOTAL(m, translations);
        HvBlock *b = block_at(m, ADDR(tso_plain), run & 1);
        if (run >= 2) check(run & 1 ? "EnTSO on: resumed in the TSO translation" : "EnTSO off: resumed in the plain translation",
                            c.helper_ret, b ? ADDR(b->code) : 1);
    }
    int plain = 0, tso = 0;
    for (uint32_t h = 0; h < HV_BLOCK_HASH; h++)
        for (uint32_t i = m->hash[h]; i; i = m->blocks[i - 1].hash_next)
            if (m->blocks[i - 1].va == ADDR(tso_plain)) m->blocks[i - 1].flags & HV_BLOCK_TSO ? tso++ : plain++;
    check("tso_plain translated without TSO", plain, 1);
    check("tso_plain translated with TSO", tso, 1);

    /* A call to another page, a branch through a register and the return, plain first, then with EnTSO: each
       exit finds the TSO translations, not the plain ones in the branch caches and the return table. */
    static const struct { const char *name; uint64_t va; } calls[] = {
        {"the caller", ADDR(tso_calls)}, {"the call's target, on another page", ADDR(tso_far)},
        {"the register branch's target", ADDR(tso_far_ret)}, {"the return", ADDR(tso_calls_back)}};
    for (int run = 0; run < 2; run++) {
        check("calls run exit", run_el0(m, &c, ADDR(tso_calls), ADDR(tso_calls_end), run ? 2 : 0, data, 0), HV_EXIT_HALT);
        for (int k = 0; k < 4; k++)
            if (!block_at(m, calls[k].va, run)) {
                printf("FAIL %s: no %s translation ran\n", calls[k].name, run ? "TSO" : "plain");
                failures++;
            }
    }

    /* A call made without EnTSO, the return with it: the callee's system call, and the kernel turns EnTSO on.
       The return finds no TSO translation in its table, not the call's way back into the plain caller, whose exit
       the first, plain, run chained to the plain translation of the code after the call. */
    c.vbar_el1 = ADDR(tso_vectors);
    for (int run = 0; run < 2; run++) {
        c.x[11] = run ? 2 : 0;
        check("call, system call, return", run_el0(m, &c, ADDR(tso_calls2), ADDR(tso_calls2_end), 0, data, 0), HV_EXIT_HALT);
        check("EnTSO after the system call", c.actlr_el1, run ? 2 : 0);
    }
    check("the return ran its TSO translation", block_at(m, ADDR(tso_calls2_back), 1) != NULL, 1);

    message_passing(m, data, stack, 1, "per-thread TSO at EL0");
    check("writer translated with TSO only", !block_at(m, ADDR(tso_writer), 0) && block_at(m, ADDR(tso_writer), 1), 1);
    check("reader translated with TSO only", !block_at(m, ADDR(tso_reader), 0) && block_at(m, ADDR(tso_reader), 1), 1);
    hv_machine_free(m);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setenv("HV_ROSETTA", "0", 1);
    alarm(30); /* A broken guest handshake must fail instead of hanging CI. */
    uint8_t *data = aligned_alloc(16384, 16384);
    uint8_t *stack = aligned_alloc(16384, 65536);
    if (!data || !stack) return 1;

    for (int mode = 0; mode < 2; mode++) {
        setenv("HV_SOFTWARE_TSO", mode ? "1" : "0", 1);
        HvMachine *m = &machines[mode];
        if (hv_machine_init(m, 8 << 20)) return 1;
        if (hv_machine_add_ram(m, ADDR(tso_programs_begin),
                              tso_programs_finish - tso_programs_begin, (void *)tso_programs_begin, 1) ||
            hv_machine_add_ram(m, ADDR(data), 16384, data, 0) ||
            hv_machine_add_ram(m, ADDR(stack), 65536, stack, 0)) return 1;

        /* Environment changes after initialization must not change a CPU's
           advertised model or the ordering of already compiled blocks. */
        setenv("HV_SOFTWARE_TSO", mode ? "0" : "1", 1);
        hv_cpu_reset(&caps, m);
        caps.sp = ADDR(stack) + 32768;
        caps.pc = ADDR(tso_caps);
        m->stop_pc = ADDR(tso_caps_end);
        check("capability program exit", hv_run(&caps), HV_EXIT_HALT);
        check("AIDR.TSO", caps.x[0], mode ? 1u << 9 : 0);
        check("ACTLR reset", caps.x[1], 0);
        check("ACTLR.EnTSO enabled", caps.x[3], 2);
        check("ACTLR.EnTSO disabled", caps.x[4], 0);
        printf("software TSO %s: capability and ACTLR checks complete\n", mode ? "on" : "off");
    }

    HvMachine *m = &machines[1];
    check("enable grouped memory accesses", hv_knob_set(m, "HV_GROUPS", 1), 1);
    message_passing(m, data, stack, 0, "every block");
    per_thread(data, stack);
    alarm(0);
    for (int i = 0; i < 2; i++) hv_machine_free(&machines[i]);
    free(stack);
    free(data);
    return failures != 0;
}
