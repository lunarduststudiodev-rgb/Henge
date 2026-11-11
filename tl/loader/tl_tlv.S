// Thread-local variable access for images Henge's loader maps (tl_loader.c).
// x0 = descriptor {thunk, key, offset}; returns the variable's address in x0.
// Like dyld's: every register but x0, x16 and x17 is preserved.
    .text
    .align 2
    .globl _tl_tlv_get_addr
_tl_tlv_get_addr:
    ldr     x16, [x0, #8]           // key
    mrs     x17, TPIDRRO_EL0
    and     x17, x17, #-8
    ldr     x17, [x17, x16, lsl #3] // this thread's block for the key
    cbz     x17, 1f
    ldr     x16, [x0, #16]          // offset
    add     x0, x17, x16
    ret
1:
    stp     fp, lr, [sp, #-16]!
    mov     fp, sp
    stp     x1, x2, [sp, #-16]!
    stp     x3, x4, [sp, #-16]!
    stp     x5, x6, [sp, #-16]!
    stp     x7, x8, [sp, #-16]!
    stp     x9, x10, [sp, #-16]!
    stp     x11, x12, [sp, #-16]!
    stp     x13, x14, [sp, #-16]!
    stp     x15, x16, [sp, #-16]!
    stp     q0, q1, [sp, #-32]!
    stp     q2, q3, [sp, #-32]!
    stp     q4, q5, [sp, #-32]!
    stp     q6, q7, [sp, #-32]!
    stp     x0, x17, [sp, #-16]!
    bl      _tl_tlv_instantiate     // x0 = block
    ldp     x16, x17, [sp], #16     // x16 = descriptor
    ldr     x16, [x16, #16]
    add     x0, x0, x16
    ldp     q6, q7, [sp], #32
    ldp     q4, q5, [sp], #32
    ldp     q2, q3, [sp], #32
    ldp     q0, q1, [sp], #32
    ldp     x15, x16, [sp], #16
    ldp     x13, x14, [sp], #16
    ldp     x11, x12, [sp], #16
    ldp     x9, x10, [sp], #16
    ldp     x7, x8, [sp], #16
    ldp     x5, x6, [sp], #16
    ldp     x3, x4, [sp], #16
    ldp     x1, x2, [sp], #16
    mov     sp, fp
    ldp     fp, lr, [sp], #16
    ret
