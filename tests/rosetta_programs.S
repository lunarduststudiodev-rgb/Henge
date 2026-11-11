#include "harness.h"
    .arch armv8.5-a
    .text
    .p2align 14
    .globl _rosetta_programs_begin, _rosetta_programs_finish
_rosetta_programs_begin:

.macro CASE name, op
PROGRAM \name
    mov w0, #0
1:  mov w1, #0
2:  \op w18, w0, w1
    mrs x2, nzcv
    str w2, [x3], #4
    add w1, w1, #1
    cmp w1, #256
    b.ne 2b
    add w0, w0, #1
    cmp w0, #256
    b.ne 1b
END \name
.endm
CASE ap_add, adds
CASE ap_sub, subs
CASE ap_and, ands

PROGRAM ap_carry
    msr nzcv, x2
    adcs w18, w0, w1
    mrs x3, nzcv
    cfinv
    mrs x4, nzcv
    sbcs w28, w0, w1
    mrs x5, nzcv
END ap_carry

PROGRAM ap_save
    msr nzcv, x0
    mrs x1, nzcv
END ap_save

PROGRAM ap_fpcr
    msr fpcr, x0
    mrs xzr, fpcr
    mrs x1, fpcr
    mrs x28, fpcr
END ap_fpcr

PROGRAM ap_fpcr_read
    mrs x1, fpcr
END ap_fpcr_read

PROGRAM ap_wide
    adds x18, x0, x1, lsl #4
    mrs x3, nzcv
    subs x28, x18, #1
    mrs x4, nzcv
    ands x6, x28, #0xff
    mrs x5, nzcv
    adds x7, sp, w2, sxtb #2
    mrs x8, nzcv
END ap_wide

    .p2align 14
_rosetta_programs_finish:
