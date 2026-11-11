#pragma once
#include <stdint.h>

/* Host floating point on a CPU thread. The guest's FPCR and FPSR stay in the host registers while a CPU
   runs (host/enter.S, hv_fp_enter): the dispatcher, the helpers and the MMIO handlers run under them. Host
   C that computes in floating point there would round, flush and set flags the guest's way, and leave
   its own exception flags in the guest's FPSR. So code on a CPU thread that uses floating point, or calls
   something that may (Apple's frameworks, Metal), goes between these two:

       HvHostFp fp = hv_host_fp_begin();
       work();                 // HV_HOST_FP_WORK: not inlined
       hv_host_fp_end(fp);

   The work is a call, not inline code: the compiler doesn't order floating-point arithmetic against an MSR
   FPCR (no FENV_ACCESS), but it can't move a callee's across the call. begin keeps the live FPCR and FPSR
   and sets the host's defaults; end puts both back, so the guest sees neither the host's controls nor its
   flags. They nest, and on a thread that isn't a CPU they put back what was there. tools/check_cpu_thread_fp.py lists the functions allowed to compute in floating point. */
#define HV_HOST_FP_WORK __attribute__((noinline))

typedef struct {
    uint64_t fpcr, fpsr;
} HvHostFp;

static inline __attribute__((always_inline)) HvHostFp hv_host_fp_begin(void) {
    HvHostFp s;
    __asm__ volatile("mrs %0, fpcr\n\tmrs %1, fpsr" : "=r"(s.fpcr), "=r"(s.fpsr)::"memory");
    __asm__ volatile("msr fpcr, xzr" ::: "memory");
    return s;
}

static inline __attribute__((always_inline)) void hv_host_fp_end(HvHostFp s) {
    __asm__ volatile("msr fpsr, %0\n\tmsr fpcr, %1" ::"r"(s.fpsr), "r"(s.fpcr) : "memory");
}
