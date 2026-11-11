// Small position-independent guest programs, copied into separately mapped pages.
.text
.macro BEGIN name
    .p2align 2
    .globl _\name, _\name\()_end
_\name:
.endm
.macro END name
_\name\()_end:
.endm

BEGIN r4_a
    ldr x6, [x0]
    add x5, x5, x6
    str x5, [x0, #16]
    br x10
END r4_a
BEGIN r4_b
    ldr x6, [x1]
    add x5, x5, x6
    str x5, [x1, #16]
    br x11
END r4_b
BEGIN r4_c
    ldr x6, [x2]
    add x5, x5, x6
    str x5, [x2, #16]
    br x12
END r4_c
BEGIN r4_d
    ldr x6, [x3]
    add x5, x5, x6
    str x5, [x3, #16]
    subs x4, x4, #1
    b.eq 1f
    br x9
1:  br x15
END r4_d

BEGIN r4_pair
    ldp x5, x6, [x0]
    stp x5, x6, [x1]
    ldr q0, [x0]
    str q0, [x1]
    br x15
END r4_pair
BEGIN r4_atomic
1:  ldaxr x5, [x0]
    add x5, x5, #1
    stlxr w6, x5, [x0]
    cbnz w6, 1b
    ldar x7, [x1]
    stlr x5, [x2]
    br x15
END r4_atomic
BEGIN r4_jump
    br x9
END r4_jump
BEGIN r4_value
    mov x5, #77
    br x15
END r4_value
BEGIN r4_wait
1:  ldar x5, [x0]
    cmp x5, #2
    b.ne 1b
    br x15
END r4_wait
