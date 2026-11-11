// Differential fuzzer: random A64 sequences run natively and through the
// translator (guest PA = host address, EL1, MMU off), comparing every
// register, NZCV, FPCR/FPSR and the stack memory they touch.
//
//   build/test_fuzz [iterations] [seed] [mode]
//   mode: all (default), single (one random word per body, the whole
//   data-processing/SIMD space), seq (templated sequences only)
//   build/test_fuzz crash <hex insn>   run one word through the JIT in a child
//   build/test_fuzz one <hex insn>...   one body, native and translated
//
// With no arguments (make test) it first runs fixed regression cases, then a
// short fuzz run. It fails on any difference, on a word the host rejects that
// does not reach the guest as UNDEF, and on any host signal.
#include "harness.h"
#include "classify.h"
#include "dispatch.h"
#include "operands.h"

#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

DECLARE_PROGRAM(fuzz_tmpl);
extern const char fuzz_epi[], fuzz_epi_end[];
extern char hv_native_cpu[];
extern void hv_native_return(void);

#define BUF_BYTES (64 * 1024)
#define TRAP_OFF 0x8000
#define STACK_BYTES 65536

static HvMachine m;
static uint32_t *buf;
static uint32_t pro_words, jit_start_w, jit_end_w;
static uint8_t *stack;
static uint64_t sp0, rng;
static uint64_t mem_seed;

static uint32_t rnd(void) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)(rng >> 11);
}

static void write_code(const uint32_t *body, int n) {
    pthread_jit_write_protect_np(0);
    memcpy(buf, fuzz_tmpl_native, (size_t)(fuzz_tmpl - fuzz_tmpl_native));
    pro_words = (uint32_t)((fuzz_tmpl - fuzz_tmpl_native) / 4);
    jit_start_w = pro_words;
    memcpy(buf + pro_words, body, 4u * (uint32_t)n);
    uint32_t e = pro_words + (uint32_t)n;
    if (e & 1) buf[e++] = 0xd503201f; /* nop: the epilogue's literals want 8-byte alignment */
    jit_end_w = e;
    size_t len = (size_t)(fuzz_epi_end - fuzz_epi);
    memcpy(buf + e, fuzz_epi, len);
    uint64_t lits[2] = {(uint64_t)(uintptr_t)hv_native_cpu, (uint64_t)(uintptr_t)hv_native_return};
    memcpy((char *)(buf + e) + len - 16, lits, 16);
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(buf, BUF_BYTES);
    hv_dma_written(&m, (uint64_t)(uintptr_t)buf, TRAP_OFF); /* drop translations of the last body */
}

static void reset_mem(void) {
    uint64_t s = mem_seed;
    for (size_t i = 0; i < STACK_BYTES; i += 8) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        memcpy(stack + i, &s, 8);
    }
}

static sigjmp_buf jb;
static volatile sig_atomic_t in_guard;
#include <sys/ucontext.h>
static volatile uint64_t sig_pc, sig_fpcr;
static volatile uint32_t sig_insn;
static volatile sig_atomic_t rewrite_on_sigill; /* the stale-page case in fixed_cases */
static uint32_t rewrite_at;

static void on_sig(int sig, siginfo_t *si, void *ctx) {
    (void)si;
    if (sig == SIGILL && rewrite_on_sigill) {
        /* As if another CPU rewrote the page after this block was translated: the
           guest's bytes no longer say which instruction the block was running. */
        rewrite_on_sigill = 0;
        pthread_jit_write_protect_np(0);
        buf[rewrite_at] = 0xd503201f;
        pthread_jit_write_protect_np(1);
        hv_invalidate_code(&m, (uint64_t)(uintptr_t)buf);
    }
    /* As host/vm.c: a word the host rejects inside translated code becomes a guest UNDEF. */
    if (sig == SIGILL && hv_jit_sigill(&m, ctx)) return;
    ucontext_t *uc = ctx;
    sig_pc = uc->uc_mcontext->__ss.__pc;
    sig_insn = *(volatile uint32_t *)(uintptr_t)sig_pc;
    sig_fpcr = uc->uc_mcontext->__ns.__fpcr;
    if (in_guard) siglongjmp(jb, sig);
    signal(sig, SIG_DFL);
    raise(sig);
}

/* Native, with SIGILL/SIGBUS/SIGSEGV turned into a return value. */
static int native_once(const HvCpu *init, HvCpu *out) {
    *out = *init;
    reset_mem();
    int sig = sigsetjmp(jb, 1);
    if (sig) {
        in_guard = 0;
        __asm__ volatile("msr fpcr, xzr");
        return sig;
    }
    in_guard = 1;
    hv_native_run(out, buf);
    in_guard = 0;
    __asm__ volatile("msr fpcr, xzr");
    return 0;
}

static uint8_t native_mem[STACK_BYTES], jit_mem[STACK_BYTES];
static int native(const HvCpu *init, HvCpu *out) {
    static HvCpu b; /* an HvCpu is 5 MB: never on the stack, where two inlined into main overflow its 8 MB */
    int s = 0;
    for (int tries = 0; tries < 10; tries++) {
        s = native_once(init, out);
        if (s) return s;
        memcpy(native_mem, stack, STACK_BYTES);
        if (native_once(init, &b)) return -1;
        if (memcmp(out->x, b.x, sizeof b.x) == 0 && out->v[0] == b.v[0]) break;
    }
    return 0;
}

/* Through the translator. Returns 0, or the guest exception class + 1000, or -1. */
static int jit(const HvCpu *init, HvCpu *cpu) {
    reset_mem();
    hv_cpu_reset(cpu, &m);
    memcpy(cpu->x, init->x, sizeof cpu->x);
    memcpy(cpu->v, init->v, sizeof cpu->v);
    cpu->sp = init->sp;
    cpu->nzcv = init->nzcv;
    cpu->fpcr = init->fpcr;
    cpu->fpsr = init->fpsr;
    cpu->cpacr_el1 = 3u << 20;
    uint64_t trap = (uint64_t)(uintptr_t)buf + TRAP_OFF;
    cpu->vbar_el1 = trap;
    cpu->pc = (uint64_t)(uintptr_t)(buf + jit_start_w);
    m.stop_pc = (uint64_t)(uintptr_t)(buf + jit_end_w);
    m.stop_pc2 = trap + (cpu->spsel ? 0x200 : 0);
    int sig = sigsetjmp(jb, 1);
    if (sig) {
        in_guard = 0;
        printf("  host signal %d while running translated code: host pc %llx insn %08x fpcr %llx (jit rx %p..)\n", sig, (unsigned long long)sig_pc, sig_insn, (unsigned long long)sig_fpcr, (void *)m.jit.base);
        exit(3);
    }
    in_guard = 1;
    int r = hv_run(cpu);
    in_guard = 0;
    memcpy(jit_mem, stack, STACK_BYTES);
    if (r != HV_EXIT_HALT) return -1;
    if (cpu->pc == m.stop_pc2) return 1000 + (int)(cpu->esr_el1 >> 26);
    return 0;
}

/* ---- generators ---- */

static uint32_t fix_sp_dest(uint32_t w) {
    /* Keep SP pointing into the stack: rewrite destinations that would write it. */
    int rd = w & 31;
    if (rd != 31) return w;
    uint32_t newrd = rnd() % 31;
    if ((w & 0x1f200000) == 0x0b200000 && !(w & (1u << 29))) return (w & ~31u) | newrd; /* ADD/SUB (extended) */
    if ((w & 0x1f800000) == 0x11000000 && !(w & (1u << 29))) return (w & ~31u) | newrd; /* ADD/SUB (immediate) */
    if ((w & 0x1f800000) == 0x12000000 && ((w >> 29) & 3) != 3) return (w & ~31u) | newrd; /* AND/ORR/EOR (immediate) */
    if ((w & 0x1f800000) == 0x11800000) return (w & ~31u) | newrd; /* ADDG/SUBG, MIN/MAX (immediate) */
    return w;
}

static int reg_hot(void) {
    static const int hot[] = {6, 7, 15, 18, 28, 0, 1, 29, 30, 31};
    return rnd() % 3 ? hot[rnd() % 10] : (int)(rnd() % 32);
}

static uint32_t rand_regs(uint32_t w) {
    /* Bias the three usual register fields toward the stolen registers. */
    if (rnd() % 2) w = (w & ~31u) | (uint32_t)reg_hot();
    if (rnd() % 2) w = (w & ~(31u << 5)) | (uint32_t)reg_hot() << 5;
    if (rnd() % 2) w = (w & ~(31u << 16)) | (uint32_t)reg_hot() << 16;
    return w;
}

static int native_ok_alone(uint32_t w);
static void seed_state(HvCpu *init);

static uint32_t gen_dp_reg(void) { return fix_sp_dest(rand_regs((rnd() & ~0x0e000000u) | 0x0a000000u)); }
static uint32_t gen_simd(void) { return rand_regs((rnd() & ~0x0e000000u) | 0x0e000000u); }
static uint32_t gen_dp_imm(void) { return fix_sp_dest(rand_regs((rnd() & ~0x1c000000u) | 0x10000000u)); }

static uint32_t gen_flags(void) {
    int rt = reg_hot();
    switch (rnd() % 9) {
    case 0: return 0xd500401f; /* CFINV */
    case 1: return 0xba000400u | (rnd() % 64) << 15 | (uint32_t)rt << 5 | (rnd() % 16); /* RMIF */
    case 2: return 0x3a00080du | (rnd() & 1) << 14 | (uint32_t)rt << 5;               /* SETF8/16 */
    case 3: return 0xd53b4200u | (uint32_t)rt;                                        /* MRS NZCV */
    case 4: return 0xd51b4200u | (uint32_t)rt;                                        /* MSR NZCV */
    case 5: return 0xd53b4420u | (uint32_t)rt;                                        /* MRS FPSR */
    case 6: return 0xd51b4420u | (uint32_t)rt;                                        /* MSR FPSR */
    case 7: return 0xd53b4400u | (uint32_t)rt;                                        /* MRS FPCR */
    default: /* MSR FPCR with only the bits the guest keeps (HV_FPCR_MASK); the trap
                enables are checked by the fixed cases, since natively they stick on M4/M5 */
        return 0; /* replaced by the caller with a masked pair */
    }
}

static uint32_t gen_hint(void) {
    for (;;) {
        uint32_t imm = rnd() % 128;
        uint32_t w = 0xd503201fu | imm << 5;
        if (hv_classify(w) == HV_INSN_COPY) return w;
    }
}

/* Loads and stores with SP as the base, offsets kept inside the stack and aligned. */
static uint32_t gen_mem(void) {
    int rt = reg_hot(), rs = reg_hot();
    if (rt == 31 && rnd() % 2) rt = (int)(rnd() % 31);
    uint32_t sz = rnd() % 4;
    switch (rnd() % 14) {
    case 0: { /* LDR/STR/LDRS (unsigned offset) */
        uint32_t opc = rnd() % (sz == 3 ? 2 : sz == 2 ? 3 : 4);
        return sz << 30 | 0x39000000u | opc << 22 | (rnd() % 64) << 10 | 31u << 5 | (uint32_t)rt;
    }
    case 1: { /* LDUR/STUR, aligned, either side of SP */
        uint32_t opc = rnd() % (sz == 3 ? 2 : sz == 2 ? 3 : 4);
        int32_t off = ((int32_t)(rnd() % 64) - 32) << sz;
        return sz << 30 | 0x38000000u | opc << 22 | ((uint32_t)off & 0x1ff) << 12 | 31u << 5 | (uint32_t)rt;
    }
    case 2: { /* pre/post-index on SP, multiples of 16 */
        uint32_t opc = rnd() % (sz == 3 ? 2 : sz == 2 ? 3 : 4);
        int32_t off = ((int32_t)(rnd() % 9) - 4) * 16;
        return sz << 30 | 0x38000000u | opc << 22 | ((uint32_t)off & 0x1ff) << 12 | (rnd() % 2 ? 3u : 1u) << 10 | 31u << 5 | (uint32_t)rt;
    }
    case 3: { /* LDP/STP/LDPSW, offset/pre/post */
        static const uint32_t base[] = {0x29000000u, 0xa9000000u, 0x69400000u, 0x2d000000u, 0x6d000000u, 0xad000000u};
        int k = (int)(rnd() % 6);
        int scale = k == 1 || k == 4 ? 3 : k == 5 ? 4 : 2;
        int mode = (int)(rnd() % 3); /* 0 offset, 1 post, 2 pre */
        uint32_t w = base[k];
        if (k != 2 && rnd() % 2) w |= 1u << 22; /* load */
        w = (w & ~(3u << 23)) | (mode == 0 ? 2u : mode == 1 ? 1u : 3u) << 23;
        int32_t imm = mode ? ((int32_t)(rnd() % 9) - 4) * (16 >> scale) : (int32_t)(rnd() % 32) - 16;
        if (mode && !imm && scale == 4) imm = 1;
        int rt2 = reg_hot();
        if ((w >> 22) & 1)
            while (rt2 == rt) rt2 = reg_hot();
        return w | ((uint32_t)imm & 0x7f) << 15 | (uint32_t)rt2 << 10 | 31u << 5 | (uint32_t)rt;
    }
    case 4: /* register offset, XZR index */
        return sz << 30 | 0x38200000u | (rnd() % 2) << 22 | 31u << 16 | 3u << 13 | (rnd() % 2) << 12 | 2u << 10 | 31u << 5 | (uint32_t)rt;
    case 5: case 6: { /* LSE: LDADD..LDUMIN, SWP, LDAPR */
        uint32_t o3 = rnd() % 4 == 0, opc = o3 ? (rnd() % 2 ? 0 : 4) : rnd() % 8;
        uint32_t a = rnd() % 2, rl = rnd() % 2;
        if (o3 && opc == 4) rs = 31, a = 1, rl = 0; /* LDAPR */
        return sz << 30 | 0x38200000u | a << 23 | rl << 22 | (uint32_t)rs << 16 | o3 << 15 | opc << 12 | 31u << 5 | (uint32_t)rt;
    }
    case 7: /* CAS family */
        return sz << 30 | 0x08a07c00u | (rnd() % 2) << 22 | (uint32_t)rs << 16 | (rnd() % 2) << 15 | 31u << 5 | (uint32_t)rt;
    case 8: { /* CASP */
        int a = (int)(rnd() % 15) * 2, b = (int)(rnd() % 15) * 2;
        return (rnd() % 2) << 30 | 0x08207c00u | (rnd() % 2) << 22 | (uint32_t)a << 16 | (rnd() % 2) << 15 | 31u << 5 | (uint32_t)b;
    }
    case 9: /* LDAR / STLR */
        return sz << 30 | (rnd() % 2 ? 0x08dffc00u : 0x089ffc00u) | 31u << 5 | (uint32_t)rt;
    case 10: { /* LDAPUR/STLUR */
        uint32_t opc = rnd() % (sz == 3 ? 2 : sz == 2 ? 3 : 4);
        int32_t off = ((int32_t)(rnd() % 64) - 32) << sz;
        return sz << 30 | 0x19000000u | opc << 22 | ((uint32_t)off & 0x1ff) << 12 | 31u << 5 | (uint32_t)rt;
    }
    case 11: /* LD1/ST1 one 16-byte register, no offset or post-index #16 */
        return (rnd() % 2 ? 0x4c007000u : 0x4c807000u | 31u << 16) | (rnd() % 2) << 22 | 31u << 5 | (uint32_t)(rnd() % 32);
    case 12: /* LD1R / LD2 etc. (two registers) */
        return (rnd() % 2 ? 0x4d40c000u : 0x4c408000u) | sz << 10 | 31u << 5 | (uint32_t)(rnd() % 31);
    default: /* SIMD LDR/STR unsigned offset: B,H,S,D,Q */
        if (rnd() % 5 == 0) return 0x3d800000u | (rnd() % 2) << 22 | (rnd() % 32) << 10 | 31u << 5 | (uint32_t)(rnd() % 32);
        return sz << 30 | 0x3d000000u | (rnd() % 2) << 22 | (rnd() % 64) << 10 | 31u << 5 | (uint32_t)(rnd() % 32);
    }
}

static uint32_t gen_branch(int i, int n) {
    int room = n - i; /* target i + k, at most n (the end) */
    int k = 1 + (int)(rnd() % (uint32_t)(room < 4 ? room : 4));
    int rt = reg_hot();
    switch (rnd() % 5) {
    case 0: return 0x54000000u | (uint32_t)k << 5 | (rnd() % 16);
    case 1: return (rnd() % 2) << 31 | 0x34000000u | (rnd() % 2) << 24 | (uint32_t)k << 5 | (uint32_t)rt;
    case 2: return (rnd() % 2) << 31 | 0x36000000u | (rnd() % 2) << 24 | (rnd() % 32) << 19 | (uint32_t)k << 5 | (uint32_t)rt;
    case 3: return 0x14000000u | (uint32_t)k;
    default: return 0x94000000u | (uint32_t)k; /* BL: x30 = the next address, the same in both runs */
    }
}

/* Does the host run this word at all? (Fresh registers; SP stays put.) */
static int native_ok_alone(uint32_t w) {
    uint64_t save = rng;
    static HvCpu init, out;
    seed_state(&init);
    rng = save ^ 0x5555;
    write_code(&w, 1);
    int s = native_once(&init, &out);
    return s == 0;
}

/* ---- comparison ---- */

static int compare_all(const char *what, const HvCpu *a, const HvCpu *b) {
    int d = compare_cpus(what, a, b);
    /* Memory: everything but the 16 bytes the native epilogue pushes below the final SP. */
    uint64_t lo = a->sp - 16 - (uint64_t)(uintptr_t)stack;
    for (size_t i = 0; i < STACK_BYTES; i++) {
        if (i >= lo && i < lo + 16) continue;
        if (native_mem[i] != jit_mem[i]) {
            printf("  %s: memory at sp0%+lld native %02x jit %02x\n", what, (long long)((int64_t)i - 32768), native_mem[i], jit_mem[i]);
            d++;
            break;
        }
    }
    return d;
}

static void dump_body(const uint32_t *body, int n) {
    printf("  body:");
    for (int i = 0; i < n; i++) printf(" %08x", body[i]);
    printf("\n");
}

static void seed_state(HvCpu *init) {
    memset(init, 0, sizeof *init);
    seed_cpu(init, rng, sp0);
    /* Some small and special values too, so branches and shifts go both ways. */
    for (int i = 0; i < 31; i++) {
        uint32_t r = rnd() % 8;
        if (r == 0) init->x[i] = 0;
        else if (r == 1) init->x[i] = rnd() % 64;
        else if (r == 2) init->x[i] = ~0ull;
        else if (r == 3) init->x[i] = sp0 + (rnd() % 64) * 8; /* a stack address */
    }
    init->nzcv = (uint64_t)(rnd() % 16) << 28;
}

/* Native SIGILL'd but the translator would copy it unchanged: run it through
   the JIT in a child, which the host kills the way it would kill the app. */
static int crash_child(uint32_t insn) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGILL, SIG_DFL);
        uint32_t body[1] = {insn};
        write_code(body, 1);
        static HvCpu init, out;
        seed_state(&init);
        in_guard = 0;
        hv_cpu_reset(&out, &m);
        memcpy(out.x, init.x, sizeof out.x);
        out.sp = init.sp;
        out.cpacr_el1 = 3u << 20;
        out.vbar_el1 = (uint64_t)(uintptr_t)buf + TRAP_OFF;
        out.pc = (uint64_t)(uintptr_t)(buf + jit_start_w);
        m.stop_pc = (uint64_t)(uintptr_t)(buf + jit_end_w);
        m.stop_pc2 = out.vbar_el1 + (out.spsel ? 0x200 : 0);
        int r = hv_run(&out);
        printf("  child: hv_run returned %d, pc %llx esr %llx\n", r, (unsigned long long)out.pc, (unsigned long long)out.esr_el1);
        _exit(r == HV_EXIT_HALT && out.pc == m.stop_pc2 ? 10 : 0);
    }
    int st;
    waitpid(pid, &st, 0);
    if (WIFSIGNALED(st)) return WTERMSIG(st);
    return -WEXITSTATUS(st);
}

/* Regression cases for bugs fixed in the Nov 2026 audit. Each runs in this
   process: a host signal from translated code ends the test (exit 3). */
static int fixed_one(const char *what, const uint32_t *body, int n, int want_js, int compare) {
    rng = 77;
    mem_seed = 5;
    write_code(body, n);
    static HvCpu init, a, b;
    seed_state(&init);
    int js = jit(&init, &b), bad = js != want_js;
    if (!bad && compare) {
        int ns = native(&init, &a);
        bad = ns != 0 || compare_all(what, &a, &b) != 0;
    }
    printf("  %-44s %s (translator %d)\n", what, bad ? "FAIL" : "ok", js);
    return bad;
}

static int fixed_cases(void) {
    int f = 0;
    printf("fixed cases:\n");
    /* CFINV: FlagM is advertised (ID_AA64ISAR0.TS = 1), so it must run, not UNDEF. */
    static const uint32_t cfinv[] = {0xd500401f, 0xd500401f, 0xd500401f};
    f += fixed_one("CFINV flips C", cfinv, 3, 0, 1);
    /* FPCR trap enables (IOE..IXE, IDE): RAZ/WI for the guest. Passed to the
       host they made the next inexact FP op kill the app on M4/M5. */
    static const uint32_t fpcr[] = {
        0xd293e000, /* movz x0, #0x9f00 */
        0xf2a0f800, /* movk x0, #0x07c0, lsl #16 (AHP, DN, FZ, RMode, FZ16) */
        0xd51b4400, /* msr fpcr, x0 */
        0x1e221820, /* fdiv s0, s1, s2 */
        0x1e602020, /* fcmp d1, d0 */
        0xd53b4401, /* mrs x1, fpcr */
    };
    f += fixed_one("FPCR trap enables are dropped", fpcr, 6, 0, 0);
    {
        static HvCpu c, init;
        rng = 77; mem_seed = 5;
        write_code(fpcr, 6);
        seed_state(&init);
        jit(&init, &c);
        int bad = c.x[1] != 0x07c00000u || c.fpcr != 0x07c00000u;
        printf("  %-44s %s (fpcr %llx)\n", "FPCR reads back without trap enables", bad ? "FAIL" : "ok", (unsigned long long)c.x[1]);
        f += bad;
    }
    /* The same through a register the JIT keeps in the HvCpu (x6), then XZR. */
    {
        static const uint32_t fpcr6[] = {
            0xd293e006, /* movz x6, #0x9f00 */
            0xf2a0f806, /* movk x6, #0x07c0, lsl #16 */
            0xd51b4406, /* msr fpcr, x6 */
            0x1e221820, /* fdiv s0, s1, s2 */
            0xd53b4401, /* mrs x1, fpcr */
            0xd51b441f, /* msr fpcr, xzr */
            0xd53b4402, /* mrs x2, fpcr */
        };
        static HvCpu init, c;
        rng = 77; mem_seed = 5;
        write_code(fpcr6, 7);
        seed_state(&init);
        int js = jit(&init, &c);
        int bad = js != 0 || c.x[1] != 0x07c00000u || c.x[2] != 0 || c.x[6] != 0x07c09f00u;
        printf("  %-44s %s (x1 %llx, x2 %llx)\n", "FPCR from x6 and XZR, masked", bad ? "FAIL" : "ok", (unsigned long long)c.x[1], (unsigned long long)c.x[2]);
        f += bad;
    }
    /* Unallocated words: guest UNDEF (EC 0), not a dead host. */
    static const uint32_t ones[] = {0xffffffff};
    f += fixed_one("all-ones word raises UNDEF", ones, 1, 1000, 0);
    /* ADDG (MTE, not advertised): UNDEF rather than copied with the stolen x28. */
    static const uint32_t addg[] = {0x919134fb};
    f += fixed_one("ADDG raises UNDEF", addg, 1, 1000, 0);
    /* A copied word this host lacks, on a page rewritten between translation and
       the trap: still the guest's UNDEF, at that instruction (hv_jit_sigill reads
       the translator's record, not the bytes now there). */
    {
        uint32_t w = 0;
        static HvCpu init, out, c;
        rng = 91;
        for (int tries = 0; tries < 100000 && !w; tries++) {
            uint32_t t = gen_simd();
            if (hv_classify(t) != HV_INSN_COPY || hv_gpr_fields(t)) continue;
            seed_state(&init);
            write_code(&t, 1);
            if (native_once(&init, &out) == SIGILL) w = t;
        }
        if (!w) printf("  %-44s skipped (this host runs every word tried)\n", "UNDEF on a rewritten page");
        else {
            const uint32_t body[] = {0xd503201f, 0xd503201f, w, 0xd503201f};
            rng = 77; mem_seed = 5;
            write_code(body, 4);
            seed_state(&init);
            rewrite_at = jit_start_w + 2;
            rewrite_on_sigill = 1;
            int js = jit(&init, &c);
            uint64_t want = (uint64_t)(uintptr_t)(buf + jit_start_w + 2);
            int bad = js != 1000 || c.elr_el1 != want || rewrite_on_sigill;
            printf("  %-44s %s (translator %d, %08x at +%lld)\n", "UNDEF on a rewritten page", bad ? "FAIL" : "ok", js, w,
                   (long long)(c.elr_el1 - (uint64_t)(uintptr_t)(buf + jit_start_w)));
            f += bad;
        }
    }
    return f;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (hv_machine_init(&m, 8 << 20) != 0) return 1;
    buf = mmap(NULL, BUF_BYTES, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (buf == MAP_FAILED) { perror("mmap"); return 1; }
    pthread_jit_write_protect_np(0);
    for (int i = 0; i < BUF_BYTES / 4; i++) buf[i] = 0xd503201f;
    pthread_jit_write_protect_np(1);
    hv_machine_add_ram(&m, (uint64_t)(uintptr_t)buf, BUF_BYTES, buf, 1);
    stack = aligned_alloc(16384, STACK_BYTES);
    hv_machine_add_ram(&m, (uint64_t)(uintptr_t)stack, STACK_BYTES, stack, 0);
    sp0 = (uint64_t)(uintptr_t)stack + 32768;
    stack_t ss = {.ss_sp = malloc(1 << 16), .ss_size = 1 << 16};
    sigaltstack(&ss, NULL);
    struct sigaction sa = {.sa_sigaction = on_sig, .sa_flags = SA_ONSTACK | SA_SIGINFO};
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);

    if (argc >= 3 && !strcmp(argv[1], "crash")) {
        uint32_t insn = (uint32_t)strtoul(argv[2], NULL, 16);
        rng = 0x1234;
        int s = crash_child(insn);
        printf("%08x through the JIT: %s %d\n", insn, s > 0 ? "child killed by signal" : "child exited", s > 0 ? s : -s);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "long")) { /* argv[3] copies of the words in argv[2] (comma separated) */
        static uint32_t body[7000];
        uint32_t pat[16];
        int np = 0;
        for (char *t = strtok(argv[2], ","); t && np < 16; t = strtok(NULL, ",")) pat[np++] = (uint32_t)strtoul(t, NULL, 16);
        int n = atoi(argv[3]);
        if (n > 7000) n = 7000;
        for (int i = 0; i < n; i++) body[i] = pat[i % np];
        rng = 77;
        mem_seed = 5;
        write_code(body, n);
        static HvCpu init, a, b;
        seed_state(&init);
        uint64_t f0 = HV_STAT_TOTAL(&m, block_flushes);
        int ns = native(&init, &a), js = jit(&init, &b);
        printf("native %d jit %d, %d blocks, flushes %llu\n", ns, js, m.nblocks, (unsigned long long)(HV_STAT_TOTAL(&m, block_flushes) - f0));
        if (!ns && !js) printf("%d differences\n", compare_all("long", &a, &b));
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "one")) { /* one body of words given on the command line */
        uint32_t body[64];
        int n = 0;
        for (int i = 2; i < argc && n < 64; i++) body[n++] = (uint32_t)strtoul(argv[i], NULL, 16);
        rng = 77;
        mem_seed = 5;
        write_code(body, n);
        static HvCpu init, a, b;
        seed_state(&init);
        int ns = native(&init, &a), js = jit(&init, &b);
        printf("native %d jit %d  nzcv in %llx native %llx jit %llx pc %llx esr %llx\n", ns, js, (unsigned long long)init.nzcv, (unsigned long long)a.nzcv, (unsigned long long)b.nzcv, (unsigned long long)b.pc, (unsigned long long)b.esr_el1);
        if (!ns && !js) printf("%d differences\n", compare_all("one", &a, &b));
        return 0;
    }

    int fixed_failures = argc > 1 ? 0 : fixed_cases();
    long iters = argc > 1 ? atol(argv[1]) : 3000;
    rng = argc > 2 ? strtoull(argv[2], NULL, 0) : 0x9e3779b97f4a7c15ull;
    const char *mode = argc > 3 ? argv[3] : "all";
    int single = !strcmp(mode, "single") || !strcmp(mode, "all");
    int seq = !strcmp(mode, "seq") || !strcmp(mode, "all");

    long n_tmpl_ill = 0;
    long n_ok = 0, n_mismatch = 0, n_native_ill_copy = 0, n_native_ill_undef = 0, n_jit_undef_native_ok = 0, n_both_fault = 0, n_other = 0;
    static uint32_t ill_copy[64];
    static uint32_t undef_ok[64];
    int nill = 0, nund = 0;
    for (long it = 0; it < iters; it++) {
        uint32_t body[64];
        int n;
        int kind = single && (!seq || it % 2 == 0) ? 0 : 1;
        if (kind == 0) {
            n = 1;
            uint32_t r = rnd() % 10;
            body[0] = r < 4 ? gen_dp_reg() : r < 8 ? gen_simd() : r < 9 ? gen_dp_imm() : gen_hint();
            HvInsnKind k = hv_classify(body[0]);
            if (k != HV_INSN_COPY && k != HV_INSN_UNDEFINED && k != HV_INSN_PCREL) { it--; continue; }
        } else {
            n = 1 + (int)(rnd() % 24);
            for (int i = 0; i < n; i++) {
                uint32_t r = rnd() % 20;
                uint32_t w;
                for (;;) {
                    w = r < 5 ? gen_dp_reg() : r < 8 ? gen_simd() : r < 10 ? gen_dp_imm() : r < 15 ? gen_mem() : r < 17 ? gen_branch(i, n) : r < 19 ? gen_flags() : gen_hint();
                    HvInsnKind k = hv_classify(w);
                    if (r >= 15 && r < 17) break; /* branches: used as is */
                    if (r >= 10 && r < 19) { if (native_ok_alone(w)) break; n_tmpl_ill++; if (n_tmpl_ill <= 12 && getenv("HV_FUZZ_SHOWFAULT")) printf("templated word the host rejects: %08x\n", w); continue; }
                    if ((k == HV_INSN_COPY || k == HV_INSN_PCREL) && native_ok_alone(w)) break;
                }
                if (w == 0) { /* masked MSR FPCR: movz x9, #bits, lsl #16; msr fpcr, x9 */
                    uint32_t bits = (rnd() & 0x7c8u); /* FPCR bits 26..22, 19 */
                    if (i + 1 < n) { body[i++] = 0xd2a00009u | bits << 5; w = 0xd51b4409u; }
                    else w = 0xd503201fu;
                }
                body[i] = w;
            }
        }
        mem_seed = rng | 1;
        write_code(body, n);
        static HvCpu init, a, b;
        seed_state(&init);
        int ns = native(&init, &a);
        if (ns < 0) { n_other++; continue; }
        if (ns) {
            /* The host refused it. What would the translator do? */
            int copied = 0;
            for (int i = 0; i < n; i++) if (hv_classify(body[i]) == HV_INSN_COPY) copied = 1;
            if (kind == 0 && copied && ns == SIGILL) {
                /* Copied, but this host has no such instruction: the SIGILL
                   handler (hv_jit_sigill) must turn it into a guest UNDEF. */
                n_native_ill_copy++;
                if (nill < 64) ill_copy[nill++] = body[0];
                int js = jit(&init, &b);
                if (js != 1000) {
                    printf("HOST-UNDEF NOT RAISED: native SIGILL, translator gave %d:", js);
                    dump_body(body, n);
                    n_mismatch++;
                }
                continue;
            }
            if (kind == 1) {
                /* A sequence that faults natively: only meaningful if the
                   translator faults too; don't run it if a copied word may SIGILL. */
                n_both_fault++;
                if (getenv("HV_FUZZ_SHOWFAULT") && n_both_fault <= 30) {
                    printf("native signal %d:", ns);
                    dump_body(body, n);
                }
                continue;
            }
            n_native_ill_undef++;
            int js = jit(&init, &b);
            if (js < 1000) {
                printf("MISMATCH: native signal %d, translator ran on (%d)\n", ns, js);
                dump_body(body, n);
                n_mismatch++;
            }
            continue;
        }
        int js = jit(&init, &b);
        if (js) {
            if (js >= 1000) {
                n_jit_undef_native_ok++;
                if (nund < 64) undef_ok[nund++] = body[0];
                if (kind == 1 || n_jit_undef_native_ok <= 10) {
                    printf("NATIVE RAN, TRANSLATOR RAISED EC %#x:", js - 1000);
                    dump_body(body, n);
                }
            } else {
                printf("translator: hv_run failed (%d)\n", js);
                dump_body(body, n);
                n_mismatch++;
            }
            continue;
        }
        char what[32];
        snprintf(what, sizeof what, "iter %ld", it);
        int d = compare_all(what, &a, &b);
        if (d) {
            n_mismatch++;
            dump_body(body, n);
            if (n_mismatch > 200) break;
        } else {
            n_ok++;
        }
    }
    printf("\n%ld iterations: %ld equal, %ld MISMATCH, %ld native SIGILL on a copied word (guest UNDEF via hv_jit_sigill), "
           "%ld native SIGILL + translator UNDEF, %ld translator UNDEF while native ran, %ld sequences faulting natively, %ld other\n",
           iters, n_ok, n_mismatch, n_native_ill_copy, n_native_ill_undef, n_jit_undef_native_ok, n_both_fault, n_other);
    if (nill) {
        printf("copied words this host rejects (first %d):", nill);
        for (int i = 0; i < nill; i++) printf(" %08x", ill_copy[i]);
        printf("\n");
    }
    printf("host undefs caught: %llu\n", (unsigned long long)HV_STAT_TOTAL(&m, host_undefs));
    return n_mismatch != 0 || fixed_failures != 0;
}
