// Load/store programs. x0 points at a 64 KB data buffer (16 KB aligned); SP is
// in a separate stack buffer. test_mem.c compares registers and the data buffer.
#include "harness.h"

    .arch armv8.4-a
    .text
    .p2align 14
    .globl _mem_programs_begin
    .globl _mem_programs_finish
_mem_programs_begin:

PROGRAM single
    str x1, [x0]
    str w2, [x0, #8]
    strh w3, [x0, #12]
    strb w4, [x0, #14]
    ldr x5, [x0]
    ldr w6, [x0, #8]
    ldrh w7, [x0, #12]
    ldrb w8, [x0, #14]
    ldrsb x9, [x0, #14]
    ldrsh w10, [x0, #12]
    ldrsw x11, [x0, #8]
    ldrsb w12, [x0, #3]
    ldur x13, [x0, #3]
    stur w14, [x0, #-0]
    ldr x15, [x0, #32760]
    str x16, [x0, #32752]
    add x1, x0, #4096
    str x17, [x1, #-8]!
    ldr x19, [x1], #24
    ldr x20, [x1, #-16]!
    mov x2, #40
    str x21, [x0, x2]
    ldr x22, [x0, x2, lsl #0]
    mov w3, #5
    ldr x23, [x0, w3, uxtw #3]
    mov x4, #-2
    add x0, x0, #64
    ldr w24, [x0, w4, sxtw #2]
    ldr x25, [x0, x4, sxtx #3]
    sub x0, x0, #64
    ldr x26, [x0, #6000]
    // unaligned and across a 16 KB page boundary
    mov x2, #16380
    str x27, [x0, x2]
    ldr x29, [x0, x2]
    mov x3, #16382
    ldr w30, [x0, x3]
    mov x3, #16376
    add x3, x0, x3
    ldp x5, x6, [x3]
END single

PROGRAM stolen_mem
    add x28, x0, #512
    str x18, [x28]
    ldr x1, [x28]
    str x28, [x0, #8]
    ldr x18, [x0, #8]
    ldr x2, [x18, #8]!
    str x2, [x28, #16]!
    mov x3, #24
    ldr x18, [x0, x3]
    str x1, [sp, #-16]!
    str x2, [sp, #8]
    ldr x4, [sp], #16
    ldr x5, [sp, #-8]
    mov x6, #8
    ldr x7, [sp, x6]
    ldr x28, [x28, #-8]
    add x18, x0, #1024
    mov x28, #16
    str x4, [x18, x28]
    ldr x8, [x18, x28, lsl #0]
    stp x18, x28, [x18, #32]
    ldp x28, x18, [x18, #32]
    stp x28, x18, [sp, #-32]!
    ldp x9, x10, [sp], #32
END stolen_mem

PROGRAM pairs
    stp x1, x2, [x0]
    stp w3, w4, [x0, #16]
    ldp x5, x6, [x0]
    ldp w7, w8, [x0, #16]
    ldpsw x9, x10, [x0, #16]
    add x11, x0, #256
    stp x12, x13, [x11, #-16]!
    ldp x14, x15, [x11], #32
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    ldp x16, x17, [sp], #16
    stnp x19, x20, [x0, #48]
    ldnp x21, x22, [x0, #48]
    stp q1, q2, [x0, #64]
    ldp q3, q4, [x0, #64]
    stp d5, d6, [x0, #128]
    ldp s7, s8, [x0, #128]
    ldp x23, x24, [x0, #504]
    stp x25, x26, [x0, #496]
END pairs

PROGRAM simd_mem
    str q1, [x0]
    ldr q2, [x0]
    str d3, [x0, #16]
    ldr s4, [x0, #16]
    ldr h5, [x0, #18]
    ldr b6, [x0, #19]
    stur q7, [x0, #33]
    ldur q8, [x0, #33]
    mov x1, x0
    st1 {v9.16b}, [x1], #16
    st1 {v10.4s, v11.4s}, [x1], #32
    st1 {v12.2d, v13.2d, v14.2d}, [x1]
    add x1, x1, #48
    st1 {v15.8h, v16.8h, v17.8h, v18.8h}, [x1], #64
    ld1 {v19.16b, v20.16b}, [x0]
    ld1 {v21.8b}, [x0]
    st2 {v22.4s, v23.4s}, [x1], #32
    sub x1, x1, #32
    ld2 {v24.4s, v25.4s}, [x1]
    st3 {v26.8b, v27.8b, v28.8b}, [x1]
    ld3 {v29.8h, v30.8h, v31.8h}, [x0]
    st4 {v0.4h, v1.4h, v2.4h, v3.4h}, [x1]
    ld4 {v4.16b, v5.16b, v6.16b, v7.16b}, [x0]
    mov x2, #48
    ld1 {v8.2s}, [x1], x2
    ld1 {v9.s}[3], [x0]
    ld1 {v10.b}[9], [x0], #1
    st1 {v11.h}[5], [x1]
    st1 {v12.d}[1], [x1], #8
    ld2 {v13.h, v14.h}[2], [x0]
    ld3 {v15.s, v16.s, v17.s}[1], [x0]
    ld1r {v18.4s}, [x0]
    ld2r {v19.8b, v20.8b}, [x0], #2
    ld4r {v21.2d, v22.2d, v23.2d, v24.2d}, [x0]
    ldr q25, [x0, x2]
    str q26, [x0, x2, lsl #0]
END simd_mem

PROGRAM exclusive
    add x1, x0, #64
    str x2, [x1]
1:  ldxr x3, [x1]
    add x3, x3, #5
    stxr w4, x3, [x1]
    cbnz w4, 1b
2:  ldaxr w5, [x1]
    eor w5, w5, #0xff
    stlxr w6, w5, [x1]
    cbnz w6, 2b
3:  ldxp x7, x8, [x0]
    add x7, x7, #1
    stxp w9, x7, x8, [x0]
    cbnz w9, 3b
    ldxrb w10, [x1]
    clrex
    stxrb w11, w10, [x1]
    add x18, x0, #96
4:  ldaxrh w12, [x18]
    add w12, w12, #3
    stlxrh w13, w12, [x18]
    cbnz w13, 4b
END exclusive

PROGRAM atomics
    add x1, x0, #128
    str x2, [x1]
    mov x3, #7
    ldadd x3, x4, [x1]
    ldaddal x3, x5, [x1]
    ldclr x6, x7, [x1]
    ldeor x8, x9, [x1]
    ldset x10, x11, [x1]
    ldsmax x12, x13, [x1]
    ldsmin x14, x15, [x1]
    ldumax x16, x17, [x1]
    ldumin x19, x20, [x1]
    swp x21, x22, [x1]
    ldaddb w3, w23, [x1]
    ldsminh w24, w25, [x1]
    ldseta w26, w27, [x1]
    stadd x3, [x1]
    ldr x29, [x1]
    mov x6, x29
    cas x6, x30, [x1]
    mov x8, #1
    casal x8, x12, [x1]
    add x14, x0, #256
    stp x2, x3, [x14]
    mov x16, x2
    mov x17, x3
    casp x16, x17, x10, x11, [x14]
    ldp x19, x20, [x14]
    ldar x21, [x1]
    stlr x22, [x14]
    ldarb w23, [x1]
    stlrh w24, [x14, #0]
    ldapr x25, [x1]
    ldaprb w26, [x1]
    stlur x27, [x14, #8]
    ldapur w9, [x14, #8]
    ldapursb x13, [x14, #9]
    add x28, x0, #384
    str x5, [x28]
    ldadd x28, x18, [x28]
    ldr x15, [x28]
    cas x18, x28, [x28]
END atomics

PROGRAM literal
    ldr x1, lit64
    ldr w2, lit64
    ldrsw x3, lit_neg
    ldr d4, lit64
    ldr q5, lit128
    ldr s6, lit_neg
    ldr x18, lit64
    ldr w28, lit_neg
    prfm pldl1keep, lit64
    b 1f
    .p2align 4
lit128: .quad 0x0123456789abcdef, 0xfedcba9876543210
lit64:  .quad 0x1122334455667788
lit_neg: .word 0x80000001
    .word 0
1:
END literal

// A copy loop and a sum: lots of blocks, post-index addressing, TLB reuse.
PROGRAM loops
    mov x1, x0
    add x2, x0, #8192
    mov x3, #512
1:  ldp x4, x5, [x1], #16
    add x4, x4, x3
    stp x4, x5, [x2], #16
    subs x3, x3, #1
    b.ne 1b
    add x2, x0, #8192
    mov x6, #0
    mov x3, #1024
2:  ldr x7, [x2], #8
    eor x6, x6, x7, ror #3
    subs x3, x3, #1
    b.ne 2b
    mov x8, #0
    mov x3, #100
3:  ldr q0, [x0, x8]
    add v1.2d, v1.2d, v0.2d
    str q1, [x0, x8]
    add x8, x8, #160
    subs x3, x3, #1
    b.ne 3b
END loops

// A function epilogue from the boot ROM: x28 restored from an SP-based pair.
PROGRAM epilogue
    sub sp, sp, #0x100
    stp x28, x27, [sp, #0xa0]
    stp x26, x25, [sp, #0xb0]
    stp x29, x30, [sp, #0xf0]
    mov x28, #0x1234
    mov x27, #0x5678
    mov x26, #1
    mov x25, #2
    ldp x29, x30, [sp, #0xf0]
    ldp x26, x25, [sp, #0xb0]
    ldp x28, x27, [sp, #0xa0]
    add sp, sp, #0x100
    mov x1, x28
    // The ROM's outlined prologue: imm7 = 20 puts 10 in bits 20..16, the
    // number of the scratch register standing in for x28.
    mov x28, #0x4321
    sub sp, sp, #0x100
    stp x28, x27, [sp, #0xa0]
    ldp x2, x3, [sp, #0xa0]
    stp x27, x28, [sp, #0x50]
    ldp x4, x5, [sp, #0x50]
    str x28, [sp, #0x50]
    ldr x6, [sp, #0x50]
    add sp, sp, #0x100
END epilogue

// JIT only: x0 is a fake device. Every access must reach it, at its own size.
// DC ZVA: an unaligned address (zeroes the 64-byte block around it), and Xt in x6/x15, which live in the HvCpu.
PROGRAM dczva
    add x1, x0, #100
    dc zva, x1
    add x6, x0, #4096
    add x6, x6, #7
    dc zva, x6
    add x15, x0, #8192
    dc zva, x15
    ldr x2, [x0, #64]
    ldr x3, [x0, #120]
    ldr x4, [x0, #128]
END dczva

// The end of a 16 KB page: accesses in its last bytes (the fast TLB check sends
// them out of line to be rechecked exactly) and ones that run into the next page.
PROGRAM pageend
    add x1, x0, #3, lsl #12
    add x1, x1, #4032
    ldr x2, [x1, #56]
    ldr w3, [x1, #60]
    ldrb w4, [x1, #63]
    ldp x5, x8, [x1, #48]
    ldp x9, x10, [x1, #56]
    ldur x11, [x1, #60]
    stur x2, [x1, #61]
    str w3, [x1, #60]
    stp x4, x5, [x1, #56]
    ldr x12, [x1, #56]
    ldr x13, [x1, #64]
END pageend

// No helper calls (test_mem checks): plain loads and stores in a page's last bytes, which the fast check refuses
// though they fit, to every kind of register (done by the light refill path through the HvCpu); then TLB misses
// on writeback, atomic and vector forms (refilled and run again). Each run starts with an empty TLB.
PROGRAM pageend_light
    add x1, x0, #3, lsl #12
    add x1, x1, #4032           // the first page's last 64 bytes
    ldr x19, [x1, #56]
    ldr w20, [x1, #60]
    ldrb w21, [x1, #63]
    ldrsb x22, [x1, #62]
    ldrsh w23, [x1, #60]
    ldrsw x24, [x1, #60]
    ldp w25, w26, [x1, #56]
    ldpsw x27, x29, [x1, #56]
    ldur w2, [x1, #59]
    ldurh w3, [x1, #61]
    mov x4, #60
    ldr w5, [x1, x4]
    ldr x6, [x1, #56]
    ldr w7, [x1, #60]
    ldrh w15, [x1, #62]
    ldr x16, [x1, #56]
    ldrsb w17, [x1, #63]
    ldr x28, [x1, #56]
    ldr x30, [x1, #56]
    ldnp w8, w9, [x1, #56]
    str x22, [x1, #56]
    str w23, [x1, #60]
    strb w24, [x1, #63]
    strh w25, [x1, #62]
    ldr x10, [x1, #56]
    stp w27, w29, [x1, #56]
    ldr x11, [x1, #56]
    stur w30, [x1, #59]
    ldr x12, [x1, #56]
    str w19, [x1, x4]
    ldr x13, [x1, #56]
    strh w6, [x1, #62]
    str w7, [x1, #56]
    ldr x14, [x1, #56]
    str x28, [x1, #56]
    ldr x8, [x1, #56]
    str wzr, [x1, #60]
    strb w15, [x1, #61]
    stnp w16, w17, [x1, #48]
    ldp x9, x2, [x1, #48]
    // TLB misses on forms that write back, read-modify-write, or are vector, and DC ZVA: each on a page of its own.
    add x3, x0, #16, lsl #12
    ldr x4, [x3, #8]!
    str x4, [x3], #16
    add x5, x0, #20, lsl #12
    ldadd x22, x23, [x5]
    add x5, x5, #64
    cas x24, x25, [x5]
    add x6, x0, #24, lsl #12
    add x6, x6, #256
    ld1 {v0.16b, v1.16b}, [x6], #32
    st1 {v0.16b}, [x6]
    stp x10, x11, [x6, #-16]!
    add x7, x0, #28, lsl #12
    add x7, x7, #192
    dc zva, x7
    ldr x5, [x7, #8]
END pageend_light

PROGRAM groups
    // Accesses grouped on one base (one TLB check for all): SP with writeback,
    // negative and unscaled offsets, sign-extending, pairs of q registers.
    // x15 (never a group base) warms the TLB so the group checks pass.
    mov x15, sp
    ldr x15, [x15, #-0x60]
    ldr x15, [x0]
    ldr x15, [x0, #0x4000]
    stp x22, x21, [sp, #-0x60]!
    stp x20, x19, [sp, #0x10]
    str x1, [sp, #0x20]
    stur w2, [sp, #0x29]
    ldrsb x3, [sp, #0x29]
    ldrsh w4, [sp, #0x2a]
    ldur x5, [sp, #0x23]
    stp q0, q1, [sp, #0x30]
    ldp q2, q3, [sp, #0x30]
    ldr q4, [sp, #0x40]
    ldp x22, x21, [sp], #0x60
    // A plain base, members before and after the first.
    add x6, x0, #0x800
    ldr x7, [x6, #8]
    str x7, [x6, #-8]
    ldp w8, w9, [x6, #-0x40]
    strb w8, [x6, #0x7ff]
    ldrh w10, [x6, #-0x100]
    add x6, x6, #1
    ldr x11, [x6, #8]
    // The same group across a page end: runs the plain copy.
    add x12, x0, #0x4000
    ldr x13, [x12, #-8]
    ldr x14, [x12]
    str x13, [x12, #8]
    ldp x16, x17, [x12, #-16]
    // Base overwritten by a load in the group.
    ldr x12, [x12, #-8]
END groups

PROGRAM mmio
    str w1, [x0]
    ldr w2, [x0, #4]
    strb w3, [x0, #3]
    ldrh w4, [x0, #6]
    stp x5, x6, [x0, #16]
    ldr x7, [x0, #8]
    ldp w8, w9, [x0, #32]
    add x18, x0, #64
    str x18, [x18]
    ldr x28, [x18, #8]
END mmio

    .p2align 14
_mem_programs_finish:
