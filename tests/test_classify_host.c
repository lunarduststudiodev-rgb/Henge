/* hv_classify against this CPU: random data-processing (immediate, register)
   and SIMD/FP words, each run natively. The classifier must copy what the
   host runs and call UNDEFINED what it rejects, except for:
   - features the guest is not offered (ID registers, core/sysreg.c), which
     must be UNDEFINED even where this host has them (unoffered() below);
   - SIMD/FP words naming no general register, which may be copied although
     the host rejects them: the SIGILL net (hv_jit_sigill) turns those into a
     guest UNDEF, so new SIMD extensions need no table here.
   The result does not depend on the host generation: a feature this host
   lacks is UNDEFINED both ways.

   build/test_classify_host [words] */
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "classify.h"
#include "operands.h"

static uint32_t *code;
static sigjmp_buf jb;
static void on_sig(int s, siginfo_t *i, void *c) { (void)i; (void)c; siglongjmp(jb, s); }
typedef void (*fn)(void);
static const uint32_t pro[] = {0xa9bf7bfd, 0x6dbf27e8, 0x6dbf2fea, 0x6dbf37ec, 0x6dbf3fee}; /* stp x29,x30; stp d8..d15 */
static const uint32_t epi[] = {0x6cc13fee, 0x6cc137ec, 0x6cc12fea, 0x6cc127e8, 0xa8c17bfd, 0xd65f03c0};
static int run(uint32_t insn) {
    pthread_jit_write_protect_np(0);
    code[5] = insn;
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(code, 64);
    int s = sigsetjmp(jb, 1);
    if (s) return s;
    ((fn)(void *)code)();
    return 0;
}
static uint64_t rng = 88172645463325252ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)rng; }

/* Features the guest is not told about but this host has: expected disagreements. */
static int unoffered(uint32_t i) {
    if ((i & 0x1f800000) == 0x11800000) return 1;                 /* ADDG/SUBG (MTE), min/max imm (CSSC) */
    if ((i & 0x5fe00000) == 0x1ac00000) { uint32_t o = (i >> 10) & 0x3f;  /* 2 source */
        if ((i >> 29) & 1) return o == 0;                          /* SUBPS */
        return o == 0 || o == 4 || o == 5 || (o >= 24 && o <= 27); }   /* SUBP IRG GMI, SMAX.. */
    if ((i & 0x7fff0000) == 0x5ac00000) { uint32_t o = (i >> 10) & 0x3f; return o >= 8 && o <= 10; } /* ABS CNT CTZ */
    if ((i & 0x7fff0000) == 0x5ac10000) return 1;                 /* PAC space: not probed */
    return 0;
}

int main(int argc, char **argv) {
    long n = argc > 1 ? atol(argv[1]) : 1000000;
    setvbuf(stdout, NULL, _IONBF, 0);
    code = mmap(NULL, 16384, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    pthread_jit_write_protect_np(0);
    memcpy(code, pro, sizeof pro); memcpy(code + 6, epi, sizeof epi);
    pthread_jit_write_protect_np(1);
    struct sigaction sa = {.sa_flags = SA_SIGINFO}; sa.sa_sigaction = on_sig;
    sigaction(SIGILL, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGSEGV, &sa, NULL); sigaction(SIGTRAP, &sa, NULL);
    long bad = 0, tried = 0, undef_host = 0, copy_sigill = 0, gpr_simd = 0;
    for (long k = 0; k < n; k++) {
        uint32_t i = rnd();
        int group = k % 3; /* 0: dp immediate, 1: dp register, 2: SIMD/FP */
        if (group == 0) i = (i & ~0x1c000000u) | 0x10000000u;            /* op0 100x */
        else if (group == 1) i = (i & ~0x0e000000u) | 0x0a000000u;       /* op0 x101 */
        else i |= 0x0e000000u;                                            /* op0 x111 */
        HvInsnKind kind = hv_classify(i);
        if (kind == HV_INSN_PCREL || kind == HV_INSN_PAC) continue;
        if (group < 2 && unoffered(i)) continue;
        /* General registers named by the instruction: x9..x12 only, never SP or a reserved one. */
        uint32_t f = kind == HV_INSN_COPY ? hv_gpr_fields(i) : 0;
        if (group < 2) {
            uint32_t rm = (i >> 16) & 0x1f;
            int rm_reg = kind != HV_INSN_COPY || (f & HV_FIELD_RM);
            i = (i & ~0x3ffu) | 9 << 5 | 10;
            if (rm_reg) i = (i & ~(0x1fu << 16)) | (rm == 0 ? 0 : 11u) << 16;
            if (kind != HV_INSN_COPY || (f & HV_FIELD_RA)) i = (i & ~(0x1fu << 10)) | ((i >> 10 & 0x1f) == 31 ? 31u : 12u) << 10;
            kind = hv_classify(i);
            if (kind == HV_INSN_PCREL || kind == HV_INSN_PAC || unoffered(i)) continue;
        } else {
            int g = hv_classify(i) == HV_INSN_COPY ? (int)hv_gpr_fields(i) : -1;
            if (g > 0 || (kind == HV_INSN_UNDEFINED)) { /* a general-register form: keep those fields on x9/x10 */
                gpr_simd++;
                i = (i & ~0x3ffu) | 9 << 5 | 10;
                kind = hv_classify(i);
            }
        }
        tried++;
        int s = run(i);
        int host_ok = s == 0;
        if (!host_ok) undef_host++;
        int ours_ok = kind != HV_INSN_UNDEFINED;
        if (group == 2 && ours_ok && !host_ok) {
            /* allowed (the SIGILL net catches it) only when it names no general register */
            if (hv_gpr_fields(i)) { if (bad++ < 40) printf("SIMD with GPR fields, host UNDEF: %08x sig %d\n", i, s); }
            else copy_sigill++;
            continue;
        }
        if (host_ok != ours_ok) {
            if (bad++ < 40) printf("%s %08x: host %s (sig %d), classify %s\n", group == 0 ? "dpimm" : group == 1 ? "dpreg" : "simd", i,
                                   host_ok ? "runs" : "UNDEF", s, hv_insn_kind_name(kind));
        }
    }
    printf("tried %ld, host-undefined %ld, SIMD copies the net must catch %ld, GPR SIMD forms %ld, mismatches %ld\n",
           tried, undef_host, copy_sigill, gpr_simd, bad);
    return bad != 0;
}
