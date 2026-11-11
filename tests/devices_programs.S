// Guest programs for the device tests (JIT only, EL1, MMU off, on the vmapple map).
#include "harness.h"

    .arch armv8.4-a
    .text
    .p2align 14
    .globl _devices_programs_begin
    .globl _devices_programs_finish
_devices_programs_begin:

// IRQ at 0x280 acknowledges, masks the timer, and EOIs. Anything else records
// ESR in x23 and skips the instruction.
    .p2align 11
dvectors:
    .rept 5
    .p2align 7
    mrs x23, esr_el1
    mrs x24, elr_el1
    add x24, x24, #4
    msr elr_el1, x24
    eret
    .endr
    .p2align 7
    mrs x20, icc_iar1_el1
    add x21, x21, #1
    mov x22, #3              // enable + IMASK: the line drops
    msr cntv_ctl_el0, x22
    msr icc_eoir1_el1, x20
    eret
    .rept 10
    .p2align 7
    mrs x23, esr_el1
    mrs x24, elr_el1
    add x24, x24, #4
    msr elr_el1, x24
    eret
    .endr

// Bring up the GIC the way a kernel does, arm the virtual timer for 1 ms,
// and sleep in WFI until its interrupt (PPI 27) arrives.
PROGRAM p_timer_irq
    adr x9, dvectors
    msr vbar_el1, x9
    mov x20, #0
    mov x21, #0
    mov x23, #0
    movz x0, #0x1000, lsl #16       // GICD
    mov w2, #3
    str w2, [x0]                    // GICD_CTLR: EnableGrp0, EnableGrp1
    movz x1, #0x1001, lsl #16       // GICR RD_base
    str wzr, [x1, #0x14]            // GICR_WAKER: wake up
1:  ldr w2, [x1, #0x14]
    tbnz w2, #2, 1b                 // wait for ChildrenAsleep to clear
    add x1, x1, #0x10000            // SGI_base
    mov w2, #(1 << 27)
    str w2, [x1, #0x80]             // IGROUPR0: PPI 27 is group 1
    str w2, [x1, #0x100]            // ISENABLER0
    mov w2, #0x80
    strb w2, [x1, #0x41b]           // IPRIORITYR27
    mov x2, #0xff
    msr icc_pmr_el1, x2
    mov x2, #1
    msr icc_igrpen1_el1, x2
    mov x2, #24000                  // 1 ms at 24 MHz
    msr cntv_tval_el0, x2
    mov x2, #1
    msr cntv_ctl_el0, x2
    mrs x10, cntvct_el0
    mov x13, #500                   // give up after about half a second of wakeups
    msr daifclr, #2
2:  wfi
    cbnz x21, 3f
    subs x13, x13, #1
    b.ne 2b
3:
    mrs x11, cntvct_el0
    msr daifset, #2
    mrs x12, icc_rpr_el1            // idle again after EOI
END p_timer_irq

PROGRAM p_uart
    movz x0, #0x2001, lsl #16       // PL011
    adr x1, uart_msg
1:  ldrb w2, [x1], #1
    cbz w2, 2f
3:  ldr w3, [x0, #0x18]             // FR: wait while TXFF
    tbnz w3, #5, 3b
    str w2, [x0]
    b 1b
2:  ldr w4, [x0, #0xfe0]            // PeriphID0
END p_uart

uart_msg:
    .asciz "hello from the guest\n"

// Pause: count and watch the counter until the host sets [x0] (progress at [x0 + 8]). x11 ends as
// the biggest step the counter took between two reads: a pause the guest saw shows up there.
PROGRAM p_pause_clock
    mrs x10, cntvct_el0
    mov x11, #0
    mov x12, #0
1:  mrs x13, cntvct_el0
    sub x14, x13, x10
    cmp x14, x11
    csel x11, x14, x11, hi
    mov x10, x13
    add x12, x12, #1
    str x12, [x0, #8]
    ldr x15, [x0]
    cbz x15, 1b
END p_pause_clock

// Pause while asleep: WFI over and over (the board's tick wakes it every 10 ms at most).
PROGRAM p_pause_wfi
    mov x12, #0
1:  wfi
    add x12, x12, #1
    str x12, [x0, #8]
    ldr x15, [x0]
    cbz x15, 1b
END p_pause_wfi

    .p2align 14
_devices_programs_finish:
