// Small programs run natively and through the JIT; test_jit.c compares every
// register, SP, NZCV, FPSR and FPCR afterwards. They do no loads or stores
// (those come with the MMU tests), but SP is used as an ordinary operand.
#include "harness.h"

// Guest code, for the guest's CPU (an Apple M-series, Armv8.5+), not for the A12 the engine is built for
// (Makefile ENGINE_CPU): pstate_bits uses DIT, an Armv8.4 register.
    .arch armv8.4-a
    .text
    .p2align 14
    .globl _jit_programs_begin
    .globl _jit_programs_finish
_jit_programs_begin:

PROGRAM arith
    add x0, x1, x2
    sub x3, x4, x5, lsl #7
    adds x6, x7, x8
    adc x9, x10, x11
    subs w12, w13, w14
    sbc w15, w16, w17
    mul x19, x20, x21
    madd x22, x23, x24, x25
    umulh x26, x27, x29
    smull x0, w1, w2
    udiv x3, x4, x5
    sdiv w6, w7, w8
    lsl x9, x10, #13
    asr x11, x12, x13
    ror w14, w15, #3
    clz x16, x17
    rbit x19, x20
    rev x21, x22
    rev16 w23, w24
    and x25, x26, #0xff00ff00ff00ff00
    eor x27, x29, x30, ror #9
    bic x1, x2, x3
    orn w4, w5, w6
    ubfx x7, x8, #5, #17
    sbfiz x10, x11, #12, #9
    bfi x12, x13, #20, #11
    extr x14, x15, x16, #23
    movz x17, #0xbeef, lsl #16
    movk x17, #0xdead, lsl #48
    movn w20, #0x1234
    negs x21, x22
    cmp x23, x24
END arith

PROGRAM loop
    mov x0, #0
    mov x1, #100
    mov x2, #3
1:  add x0, x0, x1
    mul x3, x1, x2
    eor x0, x0, x3
    subs x1, x1, #1
    b.ne 1b
    mov x4, #0
2:  add x4, x4, #7
    cmp x4, #700
    b.lt 2b
END loop

PROGRAM condbr
    mov x0, #0
    mov x1, #5
    cmp x1, #5
    b.eq 1f
    orr x0, x0, #1
1:  cmp x1, #6
    b.eq 2f
    orr x0, x0, #2
2:  cbz x1, 3f
    orr x0, x0, #4
3:  mov x2, #0
    cbnz x2, 4f
    orr x0, x0, #8
4:  tbz x1, #0, 5f
    orr x0, x0, #16
5:  tbnz x1, #1, 6f
    orr x0, x0, #32
6:  tbnz x1, #2, 7f
    orr x0, x0, #64
7:  cmp x1, #3
    b.hi 8f
    orr x0, x0, #128
8:  b.ls 9f
    orr x0, x0, #256
9:  adds x3, x1, #-5
    b.mi 10f
    b.vs 10f
    orr x0, x0, #512
10: mov x5, #-1
    cmn x5, #1
    b.cs 11f
    orr x0, x0, #1024
11:
END condbr

PROGRAM csel_ops
    cmp x1, x2
    csel x0, x3, x4, lt
    csinc x5, x6, x7, ge
    csinv x8, x9, x10, eq
    csneg x11, x12, x13, ne
    cset w14, hi
    csetm x15, ls
    ccmp x16, x17, #5, ne
    ccmn w19, #7, #2, eq
    cinc x20, x21, mi
    cneg x22, x23, pl
    ccmp x24, #31, #0xf, al
END csel_ops

PROGRAM bl_sub
    mov x0, #3
    bl 1f
    mov x5, x30
    bl 2f
    add x0, x0, #100
    b 3f
1:  add x0, x0, x0
    lsl x0, x0, #2
    ret
2:  mov x9, x30
    bl 1b
    mov x30, x9
    ret
3:
END bl_sub

PROGRAM simd_fp
    fmov d0, #1.5
    fmov d1, #-2.25
    fadd d2, d0, d1
    fmul d3, d2, d0
    fdiv d4, d3, d1
    fsqrt d5, d0
    fmadd d6, d0, d1, d2
    fcvt s7, d6
    scvtf d8, x1
    ucvtf s9, w2
    fcvtzs x3, d4
    fcvtzu w4, s7
    fcmp d0, d1
    fmov x5, d3
    fmov d10, x6
    dup v11.4s, w7
    ins v11.s[2], w8
    umov w9, v12.h[3]
    smov x10, v13.b[5]
    add v14.4s, v15.4s, v16.4s
    mul v17.8h, v18.8h, v19.8h
    eor v20.16b, v21.16b, v22.16b
    fadd v23.2d, v24.2d, v25.2d
    addv s26, v27.4s
    ext v28.16b, v29.16b, v30.16b, #5
    tbl v31.16b, {v0.16b}, v1.16b
    uzp1 v2.4s, v3.4s, v4.4s
    fmaxnm s5, s6, s7
    frintx d12, d0
    fccmp d1, d2, #4, mi
    cnt v13.8b, v14.8b
END simd_fp

// Guest x18, x28 and SP live in the HvCpu, so every use of them is renamed.
PROGRAM stolen
    add x18, x28, x1
    movz x28, #0x1111
    movk x28, #0x2222, lsl #32
    add x2, x18, x28
    ubfx x3, x18, #4, #12
    bfi x18, x2, #8, #8
    csel x4, x18, x28, ne
    cmp x18, x28
    csel x18, x18, x28, hi
    madd x28, x18, x28, x18
    fmov d0, x18
    fmov x28, d1
    dup v2.2d, x28
    umov w18, v3.s[1]
    mov x5, sp
    sub sp, sp, #64
    add x6, sp, #8
    add x7, sp, w1, uxtw #2
    mov x8, sp
    add sp, sp, #64
    adds x9, xzr, x28
    neg x18, x18
    cbz x18, 1f
    add x10, x10, #1
1:  tbnz x28, #3, 2f
    add x11, x11, #1
2:  adr x18, 3f
    br x18
    add x12, x12, #1
3:  adr x28, 4f
    blr x28
    b 5f
4:  mov x13, x30
    ret
5:  adr x18, 6f
    mov x14, #7
    ret x18
    add x14, x14, #1
6:  udiv x15, x28, x14
    eor x18, x18, x28, lsl #3
END stolen

PROGRAM pcrel
    adr x0, .
    adr x1, pcrel_target
    adrp x2, pcrel_target@PAGE
    adrp x18, pcrel_target@PAGE
    adr x28, .
    sub x3, x1, x0
    b pcrel_target
pcrel_target:
    sub x4, x28, x0
END pcrel

// More instructions than one block holds.
// DIT is a PSTATE bit the translator reads and writes inline; x6 and x7 live in the HvCpu.
PROGRAM pstate_bits
    movz x1, #0x100, lsl #16
    msr dit, x1
    mrs x2, dit
    msr dit, xzr
    mrs x3, dit
    msr dit, x6
    mrs x7, dit
    orr x4, x7, #1
    msr dit, x4
    mrs x5, dit
    msr dit, xzr
END pstate_bits

PROGRAM long_block
    mov x0, #0
    .rept 300
    add x0, x0, #3
    eor x1, x0, x1, lsl #1
    .endr
END long_block

// JIT only (the host would really sign): with unsigned pointers every PAC
// instruction must leave the registers as they were; PACGA gives 0.
PROGRAM pac
    paciasp
    autiasp
    xpaclri
    pacia x1, x2
    autia x1, x2
    pacibsp
    autibsp
    pacda x18, x3
    autda x18, x3
    xpaci x28
    autia1716
    pacga x4, x5, x6
    mov x9, x30
    bl 1f
    mov x30, x9
    b 2f
1:  pacibsp
    retab
2:  mov x10, x30
END pac

    .p2align 14
_jit_programs_finish:
