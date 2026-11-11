// The guest for test_snapshot: two CPUs, an MMU, EL0 code and interrupts, all of which a saved and
// restored machine has to carry on exactly as if it had never stopped.
//
// The test copies this code to the start of guest RAM (0x70000000) and runs it there, MMU on, mapped
// one to one; everything is position independent. CPU 0 builds the page tables, turns CPU 1 on (PSCI)
// and drops to EL0. Both run user_main: a deterministic mix of integer, memory, FP (round towards
// zero), SIMD, exclusive load/store on a counter they share, an address space switch (syscall 1, ASID
// 1 <-> 2, nG mappings) every 1024 rounds and a counter read every 256. CPU 1 first plays ping-pong
// with CPU 0 through SGIs (CPU 1 waits in WFI with interrupts masked; CPU 0 answers from its interrupt
// handler, at EL0 or EL1). Both CPUs' timers fire every 200 us throughout.
//
// What the run leaves in DET (and in the CPUs' registers) is the same every time; what timing decides
// goes to NONDET only (or to registers that are cleared before the end).

#include "snapshot_guest.h"

// D = 0x70200000:
//   +0x00 cpu1 done     +0x08 shared counter  +0x10 pongs CPU 1 got  +0x18 pings CPU 0 answered
//   +0x20 errors        +0x28 CPU 0's sum     +0x30 CPU 1's sum      +0x38 fault ESR, ELR, FAR, vector
//   +0x1000 CPU 0's buffer (64 KB)  +0x11000 CPU 1's
//   +0x40000 NONDET: ticks[2], then per CPU {previous counter, biggest step} at +0x20 and +0x30
//   +0x80000 L1 table A, +0x81000 L1 table B, +0x82000 the L2 table both use
//   +0x90000 / +0x98000 EL1 stack tops, +0xa0000 / +0xa8000 EL0 stack tops

    .text
    .p2align 14
    .globl _snapshot_programs_begin
_snapshot_programs_begin:

// Vectors (VBAR_EL1): a 2 KB-aligned table.
snap_vectors:
    .p2align 7
    mov x9, #0x000
    b bad
    .p2align 7
    mov x9, #0x080
    b bad
    .p2align 7
    mov x9, #0x100
    b bad
    .p2align 7
    mov x9, #0x180
    b bad
    .p2align 7                      // 0x200 sync, current EL with SP_ELx
    mov x9, #0x200
    b bad
    .p2align 7                      // 0x280 IRQ, current EL
    b irq_handler
    .p2align 7
    mov x9, #0x300
    b bad
    .p2align 7
    mov x9, #0x380
    b bad
    .p2align 7                      // 0x400 sync from EL0
    b svc_handler
    .p2align 7                      // 0x480 IRQ from EL0
    b irq_handler
    .p2align 7
    mov x9, #0x500
    b bad
    .p2align 7
    mov x9, #0x580
    b bad
    .rept 4
    .p2align 7
    mov x9, #0x600
    b bad
    .endr

bad:
    movz x28, #0x7020, lsl #16
    mrs x10, esr_el1
    str x10, [x28, #0x38]
    mrs x10, elr_el1
    str x10, [x28, #0x40]
    mrs x10, far_el1
    str x10, [x28, #0x48]
    str x9, [x28, #0x50]
    ldr x10, [x28, #0x20]
    orr x10, x10, #(1 << 32)
    str x10, [x28, #0x20]
    msr daifset, #2
    b _snap_done

// IRQ: the timer (re-armed), CPU 0's ping (answered with SGI 2 to CPU 1), anything else is an error.
irq_handler:
    stp x0, x1, [sp, #-48]!
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    movz x4, #0x7020, lsl #16
    mrs x5, mpidr_el1
    and x5, x5, #0xff
1:  mrs x0, icc_iar1_el1
    cmp x0, #1023
    b.eq 4f
    cmp x0, #27
    b.eq 2f
    cmp x0, #1
    b.eq 3f
    ldr x1, [x4, #0x20]
    add x1, x1, #1
    str x1, [x4, #0x20]
    msr icc_eoir1_el1, x0
    b 1b
2:  mov x1, #TICK
    msr cntv_tval_el0, x1
    add x2, x4, #0x40, lsl #12
    ldr x1, [x2, x5, lsl #3]
    add x1, x1, #1
    str x1, [x2, x5, lsl #3]
    msr icc_eoir1_el1, x0
    b 1b
3:  ldr x1, [x4, #0x18]
    add x1, x1, #1
    str x1, [x4, #0x18]
    movz x1, #0x0200, lsl #16       // SGI 2
    movk x1, #2                     // to Aff0 1 (CPU 1)
    msr icc_sgi1r_el1, x1
    msr icc_eoir1_el1, x0
    b 1b
4:  ldp x4, x5, [sp, #32]
    ldp x2, x3, [sp, #16]
    ldp x0, x1, [sp], #48
    eret

// SVC from EL0: #1 switches the address space (table A, ASID 1 <-> table B, ASID 2); #0 returns to
// the EL1 code whose address is in TPIDR_EL1, interrupts on.
svc_handler:
    stp x0, x1, [sp, #-16]!
    mrs x0, esr_el1
    lsr x1, x0, #26
    cmp x1, #0x15
    b.ne 2f
    and x0, x0, #0xffff
    cbz x0, 1f
    cmp x0, #1
    b.ne 2f
    mrs x0, ttbr0_el1
    eor x0, x0, #(3 << 48)
    eor x0, x0, #0x1000
    msr ttbr0_el1, x0
    isb
    ldp x0, x1, [sp], #16
    eret
1:  ldp x0, x1, [sp], #16
    mrs x1, tpidr_el1
    msr elr_el1, x1
    mov x1, #5                      // EL1h, DAIF clear
    msr spsr_el1, x1
    eret
2:  ldp x0, x1, [sp], #16
    mov x9, #0x401
    b bad

// x0 = this CPU's index: wake its redistributor; SGIs 1 and 2 and the virtual timer (PPI 27) in group 1.
gic_cpu_setup:
    movz x1, #0x1001, lsl #16       // GICR
    add x1, x1, x0, lsl #17
    str wzr, [x1, #0x14]
1:  ldr w2, [x1, #0x14]
    tbnz w2, #2, 1b
    add x1, x1, #0x10, lsl #12      // SGI_base
    movz w2, #0x0800, lsl #16
    movk w2, #6
    str w2, [x1, #0x80]             // IGROUPR0
    str w2, [x1, #0x100]            // ISENABLER0
    mov w2, #0x80
    strb w2, [x1, #0x41b]
    mov w2, #0x40
    strb w2, [x1, #0x401]
    strb w2, [x1, #0x402]
    mov x2, #0xff
    msr icc_pmr_el1, x2
    mov x2, #1
    msr icc_igrpen1_el1, x2
    ret

// CPU 0: the tables. L1[0]: 1 GB of device memory (the GIC); L1[1]: the L2 table, which maps the code
// (2 MB at 0x70000000, read-only, executable, global) and the rest of RAM (EL0 read-write, not
// executable, nG).
build_tables:
    add x0, x28, #0x80, lsl #12
    add x1, x28, #0x82, lsl #12
    movz x2, #0x0401
    movk x2, #0x0060, lsl #48
    str x2, [x0]
    str x2, [x0, #0x1000]
    orr x2, x1, #3
    str x2, [x0, #8]
    str x2, [x0, #0x1008]
    movz x2, #0x7000, lsl #16
    movk x2, #0x07c5
    str x2, [x1, #384 * 8]
    movz x5, #0x4000, lsl #16
    movz x6, #0x0f45
    movk x6, #0x0060, lsl #48
    mov x3, #385
1:  add x2, x5, x3, lsl #21
    orr x2, x2, x6
    str x2, [x1, x3, lsl #3]
    add x3, x3, #1
    cmp x3, #392
    b.ne 1b
    dsb ish
    ret

mmu_enable:
    mov x0, #0xff00                 // MAIR: attr0 device nGnRnE, attr1 normal write-back
    msr mair_el1, x0
    movz x0, #0x3519                // T0SZ 25, inner/outer WB, inner shareable, 4 KB
    movk x0, #0x0080, lsl #16       // EPD1
    movk x0, #5, lsl #32            // IPS 48 bits
    msr tcr_el1, x0
    add x0, x28, #0x80, lsl #12
    orr x0, x0, #(1 << 48)          // ASID 1
    msr ttbr0_el1, x0
    isb
    tlbi vmalle1
    dsb nsh
    mov x0, #(3 << 20)
    msr cpacr_el1, x0
    mov x0, #3
    msr cntkctl_el1, x0
    movz x0, #0x30d0, lsl #16
    movk x0, #0x1805                // M, C, I
    msr sctlr_el1, x0
    isb
    ret

timer_start:
    mov x0, #TICK
    msr cntv_tval_el0, x0
    mov x0, #1
    msr cntv_ctl_el0, x0
    ret

    .globl _snap_cpu0
_snap_cpu0:
    adr x0, snap_vectors
    msr vbar_el1, x0
    movz x28, #0x7020, lsl #16
    add x0, x28, #0x90, lsl #12
    mov sp, x0
    movz x0, #0x1000, lsl #16       // GICD_CTLR: both groups
    mov w1, #3
    str w1, [x0]
    mov x0, #0
    bl gic_cpu_setup
    bl build_tables
    bl mmu_enable
    bl timer_start
    movz x0, #0xc400, lsl #16       // PSCI CPU_ON
    movk x0, #3
    mov x1, #1
    adr x2, snap_cpu1
    mov x3, #0
    hvc #0
    cbz x0, 1f
    mov x9, #0x700
    b bad
1:  adr x0, cpu0_back
    msr tpidr_el1, x0
    adr x0, user_main
    msr elr_el1, x0
    msr spsr_el1, xzr               // EL0, interrupts on
    add x0, x28, #0xa0, lsl #12
    msr sp_el0, x0
    movz x0, #SEED0
    add x1, x28, #0x1, lsl #12
    movz x2, #(ITERS0 & 0xffff)
    movk x2, #(ITERS0 >> 16), lsl #16
    add x3, x28, #8
    add x4, x28, #0x40, lsl #12
    add x4, x4, #0x20
    eret
cpu0_back:                          // EL1, interrupts on: wait for CPU 1, answering its pings
    movz x28, #0x7020, lsl #16
    str x0, [x28, #0x28]
2:  ldar x1, [x28]
    cbnz x1, 3f
    wfi
    b 2b
3:  msr daifset, #2
    b _snap_done

snap_cpu1:
    adr x0, snap_vectors
    msr vbar_el1, x0
    movz x28, #0x7020, lsl #16
    add x0, x28, #0x98, lsl #12
    mov sp, x0
    mov x0, #1
    bl gic_cpu_setup
    bl mmu_enable
    bl timer_start
    mov x20, #PINGS                 // interrupts stay masked: WFI, then IAR
1:  movz x1, #0x0100, lsl #16       // SGI 1
    movk x1, #1                     // to Aff0 0 (CPU 0)
    msr icc_sgi1r_el1, x1
    isb
2:  wfi
    mrs x0, icc_iar1_el1
    cmp x0, #2
    b.eq 4f
    cmp x0, #27
    b.eq 3f
    cmp x0, #1023
    b.eq 2b
    ldr x1, [x28, #0x20]
    add x1, x1, #(1 << 12)
    str x1, [x28, #0x20]
    msr icc_eoir1_el1, x0
    b 2b
3:  mov x1, #TICK
    msr cntv_tval_el0, x1
    add x2, x28, #0x40, lsl #12
    ldr x1, [x2, #8]
    add x1, x1, #1
    str x1, [x2, #8]
    msr icc_eoir1_el1, x0
    b 2b
4:  msr icc_eoir1_el1, x0
    ldr x1, [x28, #0x10]
    add x1, x1, #1
    str x1, [x28, #0x10]
    add x4, x28, #8
    mov x3, #3
5:  ldxr x1, [x4]
    add x1, x1, #1
    stxr w2, x1, [x4]
    cbnz w2, 5b
    subs x3, x3, #1
    b.ne 5b
    subs x20, x20, #1
    b.ne 1b
    mov x2, #0
    adr x0, cpu1_back
    msr tpidr_el1, x0
    adr x0, user_main
    msr elr_el1, x0
    msr spsr_el1, xzr
    add x0, x28, #0xa8, lsl #12
    msr sp_el0, x0
    movz x0, #SEED1
    add x1, x28, #0x11, lsl #12
    movz x2, #(ITERS1 & 0xffff)
    movk x2, #(ITERS1 >> 16), lsl #16
    add x3, x28, #8
    add x4, x28, #0x40, lsl #12
    add x4, x4, #0x30
    eret
cpu1_back:
    movz x28, #0x7020, lsl #16
    str x0, [x28, #0x30]
    msr daifset, #2
    mov x1, #1
    stlr x1, [x28]
    b _snap_done

// EL0. x0 seed, x1 buffer, x2 rounds, x3 the shared counter, x4 this CPU's NONDET clock record.
// Returns the sum in x0 (through SVC #0).
user_main:
    mov x10, x0
    mov x11, x1
    mov x12, x2
    mov x13, #0
    mov x14, #0
    mov x15, x3
    mov x16, x4
    fmov d0, #1.0
    fmov d1, #0.5
    movi v2.16b, #0x5a
    movi v5.16b, #0
    mov x9, #(3 << 22)              // FPCR: round towards zero
    msr fpcr, x9
1:  eor x10, x10, x10, lsl #13
    eor x10, x10, x10, lsr #7
    eor x10, x10, x10, lsl #17
    ubfx x9, x10, #3, #13
    ldr x8, [x11, x9, lsl #3]
    eor x8, x8, x10
    add x8, x8, x13
    str x8, [x11, x9, lsl #3]
    add x14, x14, x8
    ror x14, x14, #7
    ucvtf d3, x9
    fmadd d0, d3, d1, d0
    fmul d0, d0, d1
    fcvtzu x7, d0
    add x14, x14, x7
    dup v4.2d, x10
    eor v2.16b, v2.16b, v4.16b
    add v5.4s, v5.4s, v2.4s
    ext v2.16b, v2.16b, v2.16b, #3
    tst x13, #15
    b.ne 3f
2:  ldxr x6, [x15]
    add x6, x6, #1
    stxr w7, x6, [x15]
    cbnz w7, 2b
3:  tst x13, #1023
    b.ne 4f
    svc #1
4:  tst x13, #255
    b.ne 5f
    mrs x6, cntvct_el0              // the biggest step between two reads goes to NONDET
    ldr x7, [x16]
    str x6, [x16]
    cbz x7, 5f
    sub x7, x6, x7
    ldr x5, [x16, #8]
    cmp x7, x5
    csel x5, x7, x5, hi
    str x5, [x16, #8]
5:  add x13, x13, #1
    subs x12, x12, #1
    b.ne 1b
    fmov x7, d0
    add x14, x14, x7
    mov x7, v5.d[0]
    add x14, x14, x7
    mov x7, v5.d[1]
    eor x14, x14, x7
    mrs x7, fpsr
    add x14, x14, x7
    mov x0, x14
    mov x5, #0
    mov x6, #0
    mov x7, #0
    svc #0

    .p2align 2
    .globl _snap_done
_snap_done:
    b _snap_done

    .p2align 14
    .globl _snapshot_programs_finish
_snapshot_programs_finish:
