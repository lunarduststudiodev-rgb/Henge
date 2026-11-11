// Templates for the differential fuzzer (tests/test_fuzz.c): the native
// prologue (position independent) and an epilogue that finds the HvCpu and
// hv_native_return through two literals the C side fills in after copying.
#include "harness.h"
    .text
    .globl _fuzz_tmpl_native, _fuzz_tmpl, _fuzz_tmpl_end, _fuzz_epi, _fuzz_epi_end
PROGRAM fuzz_tmpl
END fuzz_tmpl

    .p2align 3
_fuzz_epi:
    stp x0, x1, [sp, #-16]!
    ldr x0, 1f
    ldr x0, [x0]
    stp x2, x3, [x0, #16]
    stp x4, x5, [x0, #32]
    stp x6, x7, [x0, #48]
    stp x8, x9, [x0, #64]
    stp x10, x11, [x0, #80]
    stp x12, x13, [x0, #96]
    stp x14, x15, [x0, #112]
    stp x16, x17, [x0, #128]
    stp x18, x19, [x0, #144]
    stp x20, x21, [x0, #160]
    stp x22, x23, [x0, #176]
    stp x24, x25, [x0, #192]
    stp x26, x27, [x0, #208]
    stp x28, x29, [x0, #224]
    str x30, [x0, #240]
    mrs x2, nzcv
    str x2, [x0, #0x108]
    ldp x2, x3, [sp], #16
    stp x2, x3, [x0, #0]
    mov x2, sp
    str x2, [x0, #0xf8]
    mrs x2, fpcr
    str x2, [x0, #0x340]
    mrs x2, fpsr
    str x2, [x0, #0x348]
    stp q0, q1, [x0, #320]
    stp q2, q3, [x0, #352]
    stp q4, q5, [x0, #384]
    stp q6, q7, [x0, #416]
    stp q8, q9, [x0, #448]
    stp q10, q11, [x0, #480]
    stp q12, q13, [x0, #512]
    stp q14, q15, [x0, #544]
    stp q16, q17, [x0, #576]
    stp q18, q19, [x0, #608]
    stp q20, q21, [x0, #640]
    stp q22, q23, [x0, #672]
    stp q24, q25, [x0, #704]
    stp q26, q27, [x0, #736]
    stp q28, q29, [x0, #768]
    stp q30, q31, [x0, #800]
    ldr x1, 2f
    br x1
    .p2align 3
1:  .quad 0
2:  .quad 0
_fuzz_epi_end:
