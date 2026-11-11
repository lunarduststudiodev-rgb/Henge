// Run through Henge only: the capability test accesses EL1 registers.
#include "harness.h"

    .text
    .p2align 14
    .globl _tso_programs_begin, _tso_programs_finish
_tso_programs_begin:

PROGRAM tso_caps
    mrs x0, aidr_el1
    mrs x1, actlr_el1
    mov x2, #2
    msr actlr_el1, x2
    mrs x3, actlr_el1
    mov x2, #0
    msr actlr_el1, x2
    mrs x4, actlr_el1
    END tso_caps

// Message passing, without guest barriers or acquire/release instructions.
// Writer and reader use different host threads and different guest CPUs.
// The acknowledgement keeps the next payload from racing the current read.
PROGRAM tso_writer
    mov x2, #1
1:  str x2, [x0]
    str x2, [x0, #256]
2:  ldr x3, [x0, #512]
    cmp x3, x2
    b.ne 2b
    add x2, x2, #1
    cmp x2, x1
    b.ls 1b
    END tso_writer

PROGRAM tso_reader
    mov x2, #1
    mov x5, #0
1:  ldr x3, [x0, #256]
    cmp x3, x2
    b.ne 1b
    ldr x4, [x0]
    cmp x4, x2
    cinc x5, x5, ne
    str x2, [x0, #512]
    add x2, x2, #1
    cmp x2, x1
    b.ls 1b
    END tso_reader

// Rosetta's kernel sets ACTLR_EL1.EnTSO for an Intel thread, then returns to it
// at EL0: x9 is the ACTLR value and x10 where EL0 starts.
PROGRAM tso_enter
    msr actlr_el1, x9
    msr elr_el1, x10
    msr spsr_el1, xzr
    eret
    END tso_enter

PROGRAM tso_plain
    ldr x3, [x0]
    str x3, [x0, #8]
    END tso_plain

// A call to another 16 KB page, a branch through a register there, and the
// return: the exits that go through the branch caches and the return table.
PROGRAM tso_calls
    bl _tso_far
    .globl _tso_calls_back
_tso_calls_back:
    str x3, [x0, #8]
    END tso_calls

// Called without EnTSO, returned to with it: the callee makes a system call, and
// the kernel (tso_vectors) sets ACTLR_EL1 to x11 before it returns.
PROGRAM tso_calls2
    bl _tso_far2
    .globl _tso_calls2_back
_tso_calls2_back:
    str x3, [x0, #8]
    END tso_calls2

    .p2align 11
    .globl _tso_vectors
_tso_vectors:
    .space 0x400
    msr actlr_el1, x11 // a synchronous exception from EL0
    eret
    .p2align 11

    .p2align 14
    .globl _tso_far, _tso_far_ret
_tso_far:
    ldr x3, [x0]
    adr x12, _tso_far_ret
    br x12
_tso_far_ret:
    ret

    .globl _tso_far2
_tso_far2:
    svc #0
    ldr x3, [x0]
    ret

    .p2align 14
_tso_programs_finish:
